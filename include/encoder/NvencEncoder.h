#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

#include "../Interface/nvEncodeAPI.h"

// NVIDIA Video Codec SDK(NVENC)를 이용한 H.264 하드웨어 인코더.
class NvencEncoder {
public:
    NvencEncoder(int width, int height, int framerate, int bitrateKbps);
    ~NvencEncoder();

    bool Initialize(ID3D11Device* d3dDevice);
    std::vector<uint8_t> EncodeFrame(ID3D11Texture2D* texture);
    void Release();

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
