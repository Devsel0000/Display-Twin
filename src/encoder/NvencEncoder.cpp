#include "encoder/NvencEncoder.h"

#include <cstring>
#include <iostream>

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
    initParams_.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;

    // 일부 최신 드라이버에서는 프리셋 조회 API의 구버전 구조체를 거부한다.
    // 이 경우 프리셋 설정을 직접 전달하지 않고 드라이버가 P1 기본값을 적용하게
    // 하면 드라이버 버전과 무관하게 세션을 초기화할 수 있다.
    initParams_.encodeConfig = nullptr;
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

std::vector<uint8_t> NvencEncoder::EncodeFrame(ID3D11Texture2D* texture) {
    std::vector<uint8_t> output;
    if (!session_ || !registeredInput_ || !bitstreamBuffer_ || !texture) {
        return output;
    }

    d3dContext_->CopyResource(inputTexture_.Get(), texture);

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

    const NVENCSTATUS encodeStatus = api_.nvEncEncodePicture(session_, &picture);
    api_.nvEncUnmapInputResource(session_, map.mappedResource);
    if (encodeStatus != NV_ENC_SUCCESS) {
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
