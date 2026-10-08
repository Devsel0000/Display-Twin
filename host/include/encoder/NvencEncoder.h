#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "../Interface/nvEncodeAPI.h"
#include "VideoEncoder.h"

// H.264 hardware encoder using the NVIDIA Video Codec SDK (NVENC).
class NvencEncoder : public IVideoEncoder {
public:
    NvencEncoder(int width, int height, int framerate, int bitrateKbps);
    ~NvencEncoder() override;

    bool Initialize(ID3D11Device* d3dDevice) override;

    // Convenience: CopyInput() followed by Encode().
    std::vector<uint8_t> EncodeFrame(ID3D11Texture2D* texture);

    // Split form, for callers that want to hand the captured surface back to
    // DXGI before spending time encoding. Call CopyInput(), release the
    // capture frame, then Encode().
    bool CopyInput(ID3D11Texture2D* texture) override;
    std::vector<uint8_t> Encode() override;

    void Release();

    // Makes the next Encode() emit an IDR with SPS/PPS in front of it.
    // This is the PC half of loss recovery: the tablet sends a PLI when it
    // has to throw a frame away, and a fresh IDR is what lets it start a
    // clean reference chain instead of showing corruption until the next
    // periodic keyframe. Safe to call from another thread.
    void RequestKeyframe() override { forceIdr_.store(true); }

    // QoS: change the target bitrate and/or framerate on the live encoder.
    // Uses nvEncReconfigureEncoder, so the session (and the decoder's
    // reference chain) survives; pass 0 to leave a value unchanged.
    bool Reconfigure(int bitrateKbps, int framerate) override;

    // Whether the last Encode() produced a keyframe, for the IDR
    // size/timing statistics the recovery loop is judged by.
    bool LastWasIdr() const override { return lastWasIdr_; }

    int CurrentBitrateKbps() const { return bitrateKbps_; }
    int CurrentFramerate() const { return framerate_; }

    // Timing breakdown of the last Encode() call. "Submit" is the blocking
    // nvEncEncodePicture call (GPU encode work in synchronous mode);
    // "readback" is locking and copying the bitstream out.
    double LastSubmitMs() const override { return lastSubmitMs_; }
    double LastReadbackMs() const override { return lastReadbackMs_; }
    size_t LastBitstreamBytes() const override { return lastBitstreamBytes_; }

private:
    bool LoadNvencApi();
    bool CreateSession();
    bool CreateInputTexture();
    bool ConfigureEncoder();
    bool InitializeEncoderSession();
    bool CreateBuffers();
    bool RegisterInputTexture();

    int width_;
    int height_;
    int framerate_;
    int bitrateKbps_;
    uint32_t frameIndex_ = 0;
    // Set from the control-channel thread when a PLI arrives, consumed by
    // the encode loop.
    std::atomic_bool forceIdr_{false};
    bool lastWasIdr_ = false;
    // Reconfigure() (QoS thread) mutates encodeConfig_/initParams_, which
    // Encode() reads indirectly through the driver - serialize the two.
    std::mutex reconfigureMutex_;
    double lastSubmitMs_ = 0.0;
    double lastReadbackMs_ = 0.0;
    size_t lastBitstreamBytes_ = 0;

    HMODULE nvencModule_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api_{};
    void* session_ = nullptr;
    NV_ENC_INITIALIZE_PARAMS initParams_{};
    NV_ENC_CONFIG encodeConfig_{};
    NV_ENC_INPUT_PTR inputBuffer_ = nullptr;
    NV_ENC_OUTPUT_PTR bitstreamBuffer_ = nullptr;
    NV_ENC_REGISTERED_PTR registeredInput_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> inputTexture_;
};
