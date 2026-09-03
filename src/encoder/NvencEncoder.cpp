#include "encoder/NvencEncoder.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

using NvEncodeAPICreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

NvencEncoder::NvencEncoder(int width, int height, int framerate, int bitrateKbps)
    : width_(width), height_(height), framerate_(framerate), bitrateKbps_(bitrateKbps) {}

NvencEncoder::~NvencEncoder() {
    Release();
}

bool NvencEncoder::LoadNvencApi() {
    nvencModule_ = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!nvencModule_) {
        std::cerr << "[NVENC] nvEncodeAPI64.dll not found" << std::endl;
        return false;
    }

    auto createInstance = reinterpret_cast<NvEncodeAPICreateInstanceFn>(
        GetProcAddress(nvencModule_, "NvEncodeAPICreateInstance"));
    if (!createInstance) {
        std::cerr << "[NVENC] NvEncodeAPICreateInstance not found" << std::endl;
        return false;
    }

    std::memset(&api_, 0, sizeof(api_));
    api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    const NVENCSTATUS status = createInstance(&api_);
    if (status != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] NvEncodeAPICreateInstance failed: " << status << std::endl;
        return false;
    }
    return true;
}

bool NvencEncoder::CreateSession() {
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device = d3dDevice_.Get();
    params.apiVersion = NVENCAPI_VERSION;

    const NVENCSTATUS status = api_.nvEncOpenEncodeSessionEx(&params, &session_);
    if (status != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] nvEncOpenEncodeSessionEx failed: " << status << std::endl;
        return false;
    }
    return true;
}

bool NvencEncoder::CreateInputTexture() {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width_);
    desc.Height = static_cast<UINT>(height_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;

    const HRESULT hr = d3dDevice_->CreateTexture2D(&desc, nullptr, &inputTexture_);
    if (FAILED(hr)) {
        std::cerr << "[NVENC] CreateTexture2D failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    return true;
}

bool NvencEncoder::ConfigureEncoder() {
    std::memset(&initParams_, 0, sizeof(initParams_));
    initParams_.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams_.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initParams_.presetGUID = NV_ENC_PRESET_P1_GUID;
    initParams_.encodeWidth = width_;
    initParams_.encodeHeight = height_;
    initParams_.darWidth = width_;
    initParams_.darHeight = height_;
    initParams_.frameRateNum = framerate_;
    initParams_.frameRateDen = 1;
    initParams_.enablePTD = 1;
    initParams_.enableEncodeAsync = 0;
    initParams_.enableOutputInVidmem = 0;
    // ULTRA_LOW_LATENCY trades a little quality for a shorter encode pipeline,
    // which is the right trade for interactive pen input.
    initParams_.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;

    // A hand-built NV_ENC_CONFIG (zero-initialized, version set manually) is
    // rejected by the driver with NV_ENC_ERR_INVALID_PARAM. Fetching the
    // preset's own config via nvEncGetEncodePresetConfigEx first and only
    // overriding the fields we care about keeps every driver-required field
    // populated, so the modified struct is accepted.
    NV_ENC_PRESET_CONFIG presetConfig{};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
    const NVENCSTATUS presetStatus = api_.nvEncGetEncodePresetConfigEx(
        session_, initParams_.encodeGUID, initParams_.presetGUID,
        initParams_.tuningInfo, &presetConfig);
    if (presetStatus != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] nvEncGetEncodePresetConfigEx failed: " << presetStatus
                   << " - falling back to driver defaults (bitrate/SPS-PPS settings will be ignored)"
                   << std::endl;
        initParams_.encodeConfig = nullptr;
        return true;
    }

    encodeConfig_ = presetConfig.presetCfg;
    encodeConfig_.version = NV_ENC_CONFIG_VER;
    encodeConfig_.gopLength = NVENC_INFINITE_GOPLENGTH;
    encodeConfig_.frameIntervalP = 1;

    // This SDK header only defines CONSTQP/VBR/CBR (no *_LOWDELAY_HQ variant).
    // Plain CBR plus NV_ENC_TUNING_INFO_LOW_LATENCY (already set above) gives
    // the low-latency behavior we want.
    encodeConfig_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    const uint32_t bitrateBps = static_cast<uint32_t>(bitrateKbps_) * 1000u;
    encodeConfig_.rcParams.averageBitRate = bitrateBps;
    encodeConfig_.rcParams.maxBitRate = bitrateBps;
    encodeConfig_.rcParams.vbvBufferSize = bitrateBps / static_cast<uint32_t>(std::max(1, framerate_));
    encodeConfig_.rcParams.vbvInitialDelay = 0;

    // Every one of these costs latency in exchange for quality/efficiency,
    // so turn them off. zeroReorderDelay tells the encoder to emit each frame
    // as soon as it is coded (num_reorder_frames = 0), which also means the
    // Android decoder never has to hold a frame back to fix output order.
    encodeConfig_.rcParams.zeroReorderDelay = 1;
    encodeConfig_.rcParams.enableLookahead = 0;
    encodeConfig_.rcParams.enableAQ = 0;
    encodeConfig_.rcParams.enableTemporalAQ = 0;

    encodeConfig_.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    encodeConfig_.encodeCodecConfig.h264Config.idrPeriod = static_cast<uint32_t>(std::max(1, framerate_));
    encodeConfig_.encodeCodecConfig.h264Config.sliceMode = 0;
    encodeConfig_.encodeCodecConfig.h264Config.sliceModeData = 0;

    initParams_.encodeConfig = &encodeConfig_;
    std::cout << "[NVENC] Applied preset-derived config: bitrate=" << bitrateKbps_
              << "kbps repeatSPSPPS=1 idrPeriod=" << encodeConfig_.encodeCodecConfig.h264Config.idrPeriod
              << std::endl;
    return true;
}

bool NvencEncoder::InitializeEncoderSession() {
    const NVENCSTATUS status = api_.nvEncInitializeEncoder(session_, &initParams_);
    if (status != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] nvEncInitializeEncoder failed: " << status << std::endl;
        return false;
    }
    return true;
}

bool NvencEncoder::CreateBuffers() {
    NV_ENC_CREATE_BITSTREAM_BUFFER params{};
    params.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (api_.nvEncCreateBitstreamBuffer(session_, &params) != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] nvEncCreateBitstreamBuffer failed" << std::endl;
        return false;
    }
    bitstreamBuffer_ = params.bitstreamBuffer;
    return true;
}

bool NvencEncoder::RegisterInputTexture() {
    NV_ENC_REGISTER_RESOURCE params{};
    params.version = NV_ENC_REGISTER_RESOURCE_VER;
    params.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    params.width = width_;
    params.height = height_;
    params.resourceToRegister = inputTexture_.Get();
    params.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    params.bufferUsage = NV_ENC_INPUT_IMAGE;
    const NVENCSTATUS status = api_.nvEncRegisterResource(session_, &params);
    if (status != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] nvEncRegisterResource failed: " << status << std::endl;
        return false;
    }
    registeredInput_ = params.registeredResource;
    return true;
}

bool NvencEncoder::Initialize(ID3D11Device* d3dDevice) {
    if (!d3dDevice || width_ <= 0 || height_ <= 0 || (width_ % 2) != 0 ||
        (height_ % 2) != 0 || framerate_ <= 0 || bitrateKbps_ <= 0) {
        return false;
    }
    d3dDevice_ = d3dDevice;
    d3dDevice_->GetImmediateContext(&d3dContext_);

    if (!LoadNvencApi() || !CreateSession() || !CreateInputTexture() ||
        !ConfigureEncoder() || !InitializeEncoderSession() || !CreateBuffers() ||
        !RegisterInputTexture()) {
        Release();
        return false;
    }
    std::cout << "[NVENC] H.264 hardware encoder initialized" << std::endl;
    return true;
}

bool NvencEncoder::CopyInput(ID3D11Texture2D* texture) {
    if (!d3dContext_ || !inputTexture_ || !texture) return false;
    // Queues a GPU-side copy. D3D keeps the source alive for the queued
    // command, so the caller may release the DXGI duplication frame as soon
    // as this returns instead of holding it across the whole encode.
    d3dContext_->CopyResource(inputTexture_.Get(), texture);
    // We never Present, so nothing else would push this command buffer to the
    // GPU promptly - it would sit batched until NVENC's own submission forced
    // it out, and that wait shows up inside nvEncLockBitstream. Submitting now
    // lets the copy start while we set up the encode call.
    d3dContext_->Flush();
    return true;
}

std::vector<uint8_t> NvencEncoder::EncodeFrame(ID3D11Texture2D* texture) {
    if (!CopyInput(texture)) return std::vector<uint8_t>();
    return Encode();
}

std::vector<uint8_t> NvencEncoder::Encode() {
    std::vector<uint8_t> output;
    if (!session_ || !registeredInput_ || !bitstreamBuffer_) {
        return output;
    }

    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = registeredInput_;
    if (api_.nvEncMapInputResource(session_, &map) != NV_ENC_SUCCESS) {
        return output;
    }

    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = width_;
    picture.inputHeight = height_;
    picture.inputPitch = 0;
    picture.inputBuffer = map.mappedResource;
    picture.outputBitstream = bitstreamBuffer_;
    picture.bufferFmt = map.mappedBufferFmt;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picture.inputTimeStamp = frameIndex_;
    picture.frameIdx = frameIndex_++;
    // Only force the opening keyframes so a client that attaches immediately
    // has something decodable. The periodic refresh is left to NVENC's own
    // idrPeriod - forcing it again here just doubled up on expensive IDRs.
    // (The old OutputDebugStringA log lived on this path too; it blocks on the
    // attached debugger for milliseconds, so it has no business in the hot loop.)
    if (picture.frameIdx < 5) {
        picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }

    const auto submitStart = std::chrono::steady_clock::now();
    const NVENCSTATUS encodeStatus = api_.nvEncEncodePicture(session_, &picture);
    api_.nvEncUnmapInputResource(session_, map.mappedResource);
    const auto submitEnd = std::chrono::steady_clock::now();
    lastSubmitMs_ = std::chrono::duration<double, std::milli>(submitEnd - submitStart).count();
    if (encodeStatus != NV_ENC_SUCCESS) {
        lastReadbackMs_ = 0.0;
        return output;
    }

    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = bitstreamBuffer_;
    if (api_.nvEncLockBitstream(session_, &lock) == NV_ENC_SUCCESS) {
        auto* begin = static_cast<uint8_t*>(lock.bitstreamBufferPtr);
        output.assign(begin, begin + lock.bitstreamSizeInBytes);
        api_.nvEncUnlockBitstream(session_, bitstreamBuffer_);
    }
    lastReadbackMs_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - submitEnd).count();
    lastBitstreamBytes_ = output.size();
    return output;
}

void NvencEncoder::Release() {
    if (session_ && registeredInput_) {
        api_.nvEncUnregisterResource(session_, registeredInput_);
        registeredInput_ = nullptr;
    }
    if (session_ && bitstreamBuffer_) {
        api_.nvEncDestroyBitstreamBuffer(session_, bitstreamBuffer_);
        bitstreamBuffer_ = nullptr;
    }
    if (session_) {
        api_.nvEncDestroyEncoder(session_);
        session_ = nullptr;
    }
    inputTexture_.Reset();
    d3dContext_.Reset();
    d3dDevice_.Reset();
    if (nvencModule_) {
        FreeLibrary(nvencModule_);
        nvencModule_ = nullptr;
    }
    std::memset(&api_, 0, sizeof(api_));
}
