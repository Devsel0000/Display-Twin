#pragma once

#include <windows.h>
#include <d3d11.h>
#include <mfidl.h>
#include <mftransform.h>
#include <icodecapi.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "VideoEncoder.h"

// Software H.264 encoder built on Windows' own Media Foundation H.264
// encoder MFT, for PCs with no NVENC. Everything runs on the CPU - the
// captured frame is read back from the GPU, scaled and converted to NV12
// here - so the output is capped at kMaxHeight to keep that affordable.
// Expect noticeably more latency than NVENC; this is the "it works at all"
// path, not the good one.
class MediaFoundationEncoder : public IVideoEncoder {
public:
    // 720p: at 1080p the readback + colour conversion alone costs more than
    // the whole NVENC path, and the encoder itself is several times slower.
    static constexpr int kMaxHeight = 720;
    // A software encoder gains nothing from a 15 Mbps target at 720p; it
    // just burns CPU. QoS can still lower this further.
    static constexpr int kMaxBitrateKbps = 8000;

    MediaFoundationEncoder(int sourceWidth, int sourceHeight, int framerate, int bitrateKbps);
    ~MediaFoundationEncoder() override;

    bool Initialize(ID3D11Device* d3dDevice) override;
    bool CopyInput(ID3D11Texture2D* texture) override;
    std::vector<uint8_t> Encode() override;

    void RequestKeyframe() override { forceIdr_.store(true); }
    bool Reconfigure(int bitrateKbps, int framerate) override;

    bool LastWasIdr() const override { return lastWasIdr_; }
    double LastSubmitMs() const override { return lastSubmitMs_; }
    double LastReadbackMs() const override { return lastReadbackMs_; }
    size_t LastBitstreamBytes() const override { return lastBitstreamBytes_; }

    int OutputWidth() const { return width_; }
    int OutputHeight() const { return height_; }

private:
    bool CreateTransform();
    bool ConfigureTypes();
    void Release();
    // Box-filters the mapped BGRA frame down to width_ x height_ and writes
    // it into nv12_.
    void ConvertToNv12(const uint8_t* bgra, int srcWidth, int srcHeight, int srcPitch);

    int sourceWidth_;
    int sourceHeight_;
    int width_;
    int height_;
    int framerate_;
    int bitrateKbps_;

    std::atomic_bool forceIdr_{false};
    bool lastWasIdr_ = false;
    double lastSubmitMs_ = 0.0;
    double lastReadbackMs_ = 0.0;
    size_t lastBitstreamBytes_ = 0;
    int64_t frameIndex_ = 0;
    bool mfStarted_ = false;
    bool haveFrame_ = false;

    std::mutex reconfigureMutex_;

    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<ICodecAPI> codecApi_;

    std::vector<uint8_t> nv12_;         // Y plane then interleaved UV.
    std::vector<uint8_t> scaledRgb_;    // Scratch: downscaled BGR, 3 bytes per pixel.
    std::vector<uint8_t> sequenceHeader_;  // SPS/PPS, prepended to every keyframe.
};
