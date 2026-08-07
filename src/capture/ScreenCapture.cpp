// File: PenDisplayPC/src/capture/ScreenCapture.cpp
#include "capture/ScreenCapture.h"
#include <iostream>
#include <iomanip>

namespace {
void PrintCaptureEnvironment(IDXGIAdapter* adapter, IDXGIOutput* output) {
    DXGI_ADAPTER_DESC adapterDesc{};
    DXGI_OUTPUT_DESC outputDesc{};
    if (adapter && SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
        std::wcerr << L"[ScreenCapture] Adapter: " << adapterDesc.Description
            << L" (vendor=0x" << std::hex << adapterDesc.VendorId
            << L", device=0x" << adapterDesc.DeviceId << L")" << std::dec << std::endl;
    }
    if (output && SUCCEEDED(output->GetDesc(&outputDesc))) {
        std::wcerr << L"[ScreenCapture] Output: " << outputDesc.DeviceName
            << L" (attached=" << (outputDesc.AttachedToDesktop ? L"yes" : L"no")
            << L")" << std::endl;
    }

    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    HANDLE token = nullptr;
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        elevated = GetTokenInformation(token, TokenElevation, &elevation,
            sizeof(elevation), &returned) && elevation.TokenIsElevated != 0;
        CloseHandle(token);
    }
    std::cerr << "[ScreenCapture] session=" << sessionId
        << ", remote=" << (GetSystemMetrics(SM_REMOTESESSION) ? "yes" : "no")
        << ", elevated=" << (elevated ? "yes" : "no") << std::endl;
}
}

ScreenCapture::ScreenCapture() = default;
ScreenCapture::~ScreenCapture() {
    ReleaseFrame();
}

bool ScreenCapture::Initialize() {
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) {
        std::cerr << "[ERROR] CreateDXGIFactory1 failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = factory_->EnumAdapters(0, &adapter_);
    if (FAILED(hr)) {
        std::cerr << "[ERROR] EnumAdapters failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, nullptr, 0,
        D3D11_SDK_VERSION, d3dDevice_.GetAddressOf(), nullptr, nullptr);
    if (FAILED(hr)) {
        std::cerr << "[ERROR] D3D11CreateDevice failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    hr = adapter_->EnumOutputs(0, &output_);
    if (FAILED(hr)) {
        std::cerr << "[ERROR] EnumOutputs failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    hr = output_->QueryInterface(IID_PPV_ARGS(&output1_));
    if (FAILED(hr)) {
        std::cerr << "[ERROR] QueryInterface for Output1 failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    PrintCaptureEnvironment(adapter_.Get(), output_.Get());

    DXGI_OUTPUT_DESC outputDesc{};
    if (SUCCEEDED(output_->GetDesc(&outputDesc))) {
        width_ = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
        height_ = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
    }

    hr = output1_->DuplicateOutput(d3dDevice_.Get(), &duplication_);
    if (FAILED(hr)) {
        std::cerr << "[ERROR] DuplicateOutput failed: 0x" << std::hex << hr << std::endl;
        if (hr == E_ACCESSDENIED) {
            std::cerr << "[INFO] Desktop capture access was denied. Run the host elevated and make sure Windows is not on the secure desktop." << std::endl;
        }
        if (hr == DXGI_ERROR_UNSUPPORTED) {
            std::cerr << "[INFO] Desktop Duplication not supported on this system." << std::endl;
        }
        else if (hr == E_ACCESSDENIED) {
            std::cerr << "[INFO] Access denied while creating desktop duplication." << std::endl;
        }
        return false;
    }

    std::cout << "[ScreenCapture] Initialized successfully." << std::endl;
    return true;
}

bool ScreenCapture::CaptureFrame() {
    if (!duplication_) return false;
    if (frameAcquired_) ReleaseFrame();
    IDXGIResource* resource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO info{};
    HRESULT hr = duplication_->AcquireNextFrame(10, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        std::cerr << "[ScreenCapture] Desktop duplication access lost" << std::endl;
        duplication_.Reset();
        frameTexture_.Reset();
        output1_->DuplicateOutput(d3dDevice_.Get(), &duplication_);
        return false;
    }
    frameAcquired_ = true;
    if (!resource) {
        ReleaseFrame();
        return false;
    }

    hr = resource->QueryInterface(IID_PPV_ARGS(&frameTexture_));
    resource->Release();
    if (FAILED(hr)) ReleaseFrame();
    return frameAcquired_;
}

ID3D11Texture2D* ScreenCapture::GetFrameTexture() const {
    return frameTexture_.Get();
}

void ScreenCapture::ReleaseFrame() {
    if (duplication_ && frameAcquired_) {
        duplication_->ReleaseFrame();
        frameAcquired_ = false;
        frameTexture_.Reset();
    }
}

ID3D11Device* ScreenCapture::GetD3DDevice() const {
    return d3dDevice_.Get();
}

int ScreenCapture::GetWidth() const { return width_; }
int ScreenCapture::GetHeight() const { return height_; }
