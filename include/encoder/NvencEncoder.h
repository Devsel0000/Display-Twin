#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

#include "../Interface/nvEncodeAPI.h"

// H.264 hardware encoder using the NVIDIA Video Codec SDK (NVENC).
class NvencEncoder {
public:
    NvencEncoder(int width, int height, int framerate, int bitrateKbps);
    ~NvencEncoder();

    bool Initialize(ID3D11Device* d3dDevice);

    // Convenience: CopyInput() followed by Encode().
    std::vector<uint8_t> EncodeFrame(ID3D11Texture2D* texture);

    // Split form, for callers that want to hand the captured surface back to
    // DXGI before spending time encoding. Call CopyInput(), release the
    // capture frame, then Encode().
    bool CopyInput(ID3D11Texture2D* texture);
    std::vector<uint8_t> Encode();

    void Release();

    // Timing breakdown of the last Encode() call. "Submit" is the blocking
    // nvEncEncodePicture call (GPU encode work in synchronous mode);
    // "readback" is locking and copying the bitstream out.
    double LastSubmitMs() const { return lastSubmitMs_; }
    double LastReadbackMs() const { return lastReadbackMs_; }
    size_t LastBitstreamBytes() const { return lastBitstreamBytes_; }

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
