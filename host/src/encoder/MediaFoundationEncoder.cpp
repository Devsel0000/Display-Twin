#include "encoder/MediaFoundationEncoder.h"

#include <mfapi.h>
#include <mferror.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "strmiids.lib")

using Microsoft::WRL::ComPtr;

namespace {

// MF timestamps are in 100ns units.
constexpr int64_t kHnsPerSecond = 10000000;

// The project builds as C++14, so no std::clamp.
uint8_t ClampByte(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

void SetCodecUint32(ICodecAPI* api, const GUID& id, uint32_t value) {
    if (!api) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    api->SetValue(&id, &v);  // Best-effort: not every encoder exposes every knob.
    VariantClear(&v);
}

void SetCodecBool(ICodecAPI* api, const GUID& id, bool value) {
    if (!api) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    api->SetValue(&id, &v);
    VariantClear(&v);
}

}  // namespace

MediaFoundationEncoder::MediaFoundationEncoder(int sourceWidth, int sourceHeight, int framerate, int bitrateKbps)
    : sourceWidth_(sourceWidth), sourceHeight_(sourceHeight), framerate_(framerate),
      bitrateKbps_(std::min(bitrateKbps, kMaxBitrateKbps)) {
    // Scale to at most kMaxHeight, keeping the aspect ratio. H.264 wants even
    // dimensions, and NV12 chroma is subsampled 2x2, so round both down.
    const double scale = sourceHeight_ > kMaxHeight
        ? static_cast<double>(kMaxHeight) / static_cast<double>(sourceHeight_)
        : 1.0;
    width_ = (static_cast<int>(sourceWidth_ * scale) / 2) * 2;
    height_ = (static_cast<int>(sourceHeight_ * scale) / 2) * 2;
    if (width_ < 2) width_ = 2;
    if (height_ < 2) height_ = 2;
}

MediaFoundationEncoder::~MediaFoundationEncoder() {
    Release();
}

bool MediaFoundationEncoder::Initialize(ID3D11Device* d3dDevice) {
    if (!d3dDevice || sourceWidth_ <= 0 || sourceHeight_ <= 0 || framerate_ <= 0 || bitrateKbps_ <= 0) {
        return false;
    }
    d3dDevice_ = d3dDevice;
    d3dDevice_->GetImmediateContext(&d3dContext_);

    // A CPU-readable copy of the captured frame. Desktop Duplication hands
    // out GPU-only textures, and everything below this line runs on the CPU.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(sourceWidth_);
    desc.Height = static_cast<UINT>(sourceHeight_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(d3dDevice_->CreateTexture2D(&desc, nullptr, &staging_))) {
        std::cerr << "[MFEnc] Staging texture creation failed" << std::endl;
        return false;
    }

    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        std::cerr << "[MFEnc] MFStartup failed" << std::endl;
        return false;
    }
    mfStarted_ = true;

    if (!CreateTransform() || !ConfigureTypes()) {
        Release();
        return false;
    }

    nv12_.assign(static_cast<size_t>(width_) * height_ * 3 / 2, 0);
    scaledRgb_.assign(static_cast<size_t>(width_) * height_ * 3, 0);

    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    std::cout << "[MFEnc] Software H.264 encoder ready: " << width_ << "x" << height_
              << " @" << framerate_ << "fps / " << bitrateKbps_ << "kbps" << std::endl;
    return true;
}

bool MediaFoundationEncoder::CreateTransform() {
    MFT_REGISTER_TYPE_INFO inputType{ MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outputType{ MFMediaType_Video, MFVideoFormat_H264 };

    // Synchronous software MFTs only. Hardware MFTs are asynchronous (event
    // driven), and this path exists precisely for machines without usable
    // video hardware - not worth a second code path for.
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputType, &outputType, &activates, &count);
    if (FAILED(hr) || count == 0) {
        std::cerr << "[MFEnc] No software H.264 encoder registered on this system" << std::endl;
        if (activates) CoTaskMemFree(activates);
        return false;
    }

    HRESULT activateHr = activates[0]->ActivateObject(IID_PPV_ARGS(&transform_));
    for (UINT32 i = 0; i < count; ++i) {
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    if (FAILED(activateHr) || !transform_) {
        std::cerr << "[MFEnc] ActivateObject failed: 0x" << std::hex << activateHr << std::dec << std::endl;
        return false;
    }
    transform_.As(&codecApi_);  // Optional - only used for the tuning knobs below.
    return true;
}

bool MediaFoundationEncoder::ConfigureTypes() {
    // Low latency first: these have to be set before the output type, or the
    // encoder has already committed to a B-frame/lookahead pipeline.
    SetCodecBool(codecApi_.Get(), CODECAPI_AVLowLatencyMode, true);
    SetCodecUint32(codecApi_.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    SetCodecUint32(codecApi_.Get(), CODECAPI_AVEncCommonMeanBitRate,
                   static_cast<uint32_t>(bitrateKbps_) * 1000u);
    // No periodic IDR: keyframes come from PLI (see docs/Protocol.md). A GOP
    // this long is effectively "never" for a drawing session.
    SetCodecUint32(codecApi_.Get(), CODECAPI_AVEncMPVGOPSize,
                   static_cast<uint32_t>(framerate_) * 600u);

    // Output type (must be set before the input type for an encoder MFT).
    ComPtr<IMFMediaType> out;
    if (FAILED(MFCreateMediaType(&out))) return false;
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrateKbps_) * 1000u);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width_), static_cast<UINT32>(height_));
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(framerate_), 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    HRESULT hr = transform_->SetOutputType(0, out.Get(), 0);
    if (FAILED(hr)) {
        std::cerr << "[MFEnc] SetOutputType failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    ComPtr<IMFMediaType> in;
    if (FAILED(MFCreateMediaType(&in))) return false;
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width_), static_cast<UINT32>(height_));
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(framerate_), 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = transform_->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) {
        std::cerr << "[MFEnc] SetInputType failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // SPS/PPS live on the negotiated output type. The tablet drops every
    // P-frame until it sees an IDR, and after a reconnect it needs the
    // parameter sets with it, so they get prepended to every keyframe (what
    // repeatSPSPPS=1 does on the NVENC path).
    ComPtr<IMFMediaType> negotiated;
    if (SUCCEEDED(transform_->GetOutputCurrentType(0, &negotiated))) {
        UINT32 size = 0;
        if (SUCCEEDED(negotiated->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0) {
            sequenceHeader_.resize(size);
            negotiated->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequenceHeader_.data(), size, &size);
        }
    }
    return true;
}

bool MediaFoundationEncoder::CopyInput(ID3D11Texture2D* texture) {
    if (!d3dContext_ || !staging_ || !texture) return false;
    const auto start = std::chrono::steady_clock::now();

    d3dContext_->CopyResource(staging_.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d3dContext_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    ConvertToNv12(static_cast<const uint8_t*>(mapped.pData), sourceWidth_, sourceHeight_,
                  static_cast<int>(mapped.RowPitch));
    d3dContext_->Unmap(staging_.Get(), 0);

    lastReadbackMs_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    haveFrame_ = true;
    return true;
}

// Box filter straight into NV12: each destination pixel averages the source
// pixels it covers, which is what keeps downscaled text readable (nearest
// neighbour drops half the strokes).
void MediaFoundationEncoder::ConvertToNv12(const uint8_t* bgra, int srcWidth, int srcHeight, int srcPitch) {
    const int dstW = width_;
    const int dstH = height_;
    uint8_t* rgb = scaledRgb_.data();

    for (int y = 0; y < dstH; ++y) {
        const int y0 = y * srcHeight / dstH;
        int y1 = (y + 1) * srcHeight / dstH;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dstW; ++x) {
            const int x0 = x * srcWidth / dstW;
            int x1 = (x + 1) * srcWidth / dstW;
            if (x1 <= x0) x1 = x0 + 1;

            uint32_t b = 0, g = 0, r = 0, n = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const uint8_t* row = bgra + static_cast<size_t>(sy) * srcPitch + static_cast<size_t>(x0) * 4;
                for (int sx = x0; sx < x1; ++sx, row += 4) {
                    b += row[0];
                    g += row[1];
                    r += row[2];
                    ++n;
                }
            }
            uint8_t* out = rgb + (static_cast<size_t>(y) * dstW + x) * 3;
            out[0] = static_cast<uint8_t>(b / n);
            out[1] = static_cast<uint8_t>(g / n);
            out[2] = static_cast<uint8_t>(r / n);
        }
    }

    // BT.601 studio range, the same conversion the MFT would do internally.
    uint8_t* yPlane = nv12_.data();
    uint8_t* uvPlane = nv12_.data() + static_cast<size_t>(dstW) * dstH;
    for (int y = 0; y < dstH; ++y) {
        for (int x = 0; x < dstW; ++x) {
            const uint8_t* p = rgb + (static_cast<size_t>(y) * dstW + x) * 3;
            const int luma = (66 * p[2] + 129 * p[1] + 25 * p[0] + 128) >> 8;
            yPlane[static_cast<size_t>(y) * dstW + x] = static_cast<uint8_t>(luma + 16);
        }
    }
    for (int y = 0; y < dstH; y += 2) {
        for (int x = 0; x < dstW; x += 2) {
            uint32_t b = 0, g = 0, r = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const uint8_t* p = rgb + (static_cast<size_t>(y + dy) * dstW + (x + dx)) * 3;
                    b += p[0];
                    g += p[1];
                    r += p[2];
                }
            }
            const int bb = static_cast<int>(b / 4), gg = static_cast<int>(g / 4), rr = static_cast<int>(r / 4);
            const int u = ((-38 * rr - 74 * gg + 112 * bb + 128) >> 8) + 128;
            const int v = ((112 * rr - 94 * gg - 18 * bb + 128) >> 8) + 128;
            uint8_t* uv = uvPlane + (static_cast<size_t>(y / 2) * dstW) + x;
            uv[0] = ClampByte(u);
            uv[1] = ClampByte(v);
        }
    }
}

std::vector<uint8_t> MediaFoundationEncoder::Encode() {
    std::vector<uint8_t> output;
    if (!transform_ || !haveFrame_) return output;

    std::lock_guard<std::mutex> guard(reconfigureMutex_);
    const auto start = std::chrono::steady_clock::now();

    if (forceIdr_.exchange(false)) {
        SetCodecUint32(codecApi_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
    }

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(nv12_.size()), &buffer))) return output;
    BYTE* dst = nullptr;
    if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) return output;
    std::memcpy(dst, nv12_.data(), nv12_.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(nv12_.size()));

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return output;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(frameIndex_ * kHnsPerSecond / std::max(1, framerate_));
    sample->SetSampleDuration(kHnsPerSecond / std::max(1, framerate_));
    ++frameIndex_;

    HRESULT hr = transform_->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
        std::cerr << "[MFEnc] ProcessInput failed: 0x" << std::hex << hr << std::dec << std::endl;
        return output;
    }

    MFT_OUTPUT_STREAM_INFO info{};
    transform_->GetOutputStreamInfo(0, &info);
    lastWasIdr_ = false;

    for (;;) {
        ComPtr<IMFMediaBuffer> outBuffer;
        if (FAILED(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 1u << 20), &outBuffer))) break;
        ComPtr<IMFSample> outSample;
        if (FAILED(MFCreateSample(&outSample))) break;
        outSample->AddBuffer(outBuffer.Get());

        MFT_OUTPUT_DATA_BUFFER data{};
        data.pSample = outSample.Get();
        DWORD status = 0;
        hr = transform_->ProcessOutput(0, 1, &data, &status);
        if (data.pEvents) data.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (FAILED(hr)) {
            std::cerr << "[MFEnc] ProcessOutput failed: 0x" << std::hex << hr << std::dec << std::endl;
            break;
        }

        UINT32 cleanPoint = 0;
        outSample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);
        if (cleanPoint) {
            lastWasIdr_ = true;
            output.insert(output.end(), sequenceHeader_.begin(), sequenceHeader_.end());
        }

        ComPtr<IMFMediaBuffer> contiguous;
        if (SUCCEEDED(outSample->ConvertToContiguousBuffer(&contiguous))) {
            BYTE* bytes = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(contiguous->Lock(&bytes, nullptr, &length))) {
                output.insert(output.end(), bytes, bytes + length);
                contiguous->Unlock();
            }
        }
    }

    lastSubmitMs_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    lastBitstreamBytes_ = output.size();
    return output;
}

bool MediaFoundationEncoder::Reconfigure(int bitrateKbps, int framerate) {
    std::lock_guard<std::mutex> guard(reconfigureMutex_);
    const int newBitrate = std::min(bitrateKbps > 0 ? bitrateKbps : bitrateKbps_, kMaxBitrateKbps);
    if (framerate > 0) framerate_ = framerate;  // Only paces the caller; the MFT keeps its rate.
    if (newBitrate == bitrateKbps_) return true;
    bitrateKbps_ = newBitrate;
    SetCodecUint32(codecApi_.Get(), CODECAPI_AVEncCommonMeanBitRate,
                   static_cast<uint32_t>(bitrateKbps_) * 1000u);
    return true;
}

void MediaFoundationEncoder::Release() {
    if (transform_) {
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    codecApi_.Reset();
    transform_.Reset();
    staging_.Reset();
    d3dContext_.Reset();
    d3dDevice_.Reset();
    if (mfStarted_) {
        MFShutdown();
        mfStarted_ = false;
    }
}
