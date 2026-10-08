// File: PenDisplayPC/src/capture/ScreenCapture.cpp
#include "capture/ScreenCapture.h"
#include <iostream>
#include <iomanip>

namespace {
// How often to retry recreating duplication while it is unavailable. Short
// enough to resume within a blink after a UAC prompt closes, long enough to
// keep the retry loop off the CPU.
constexpr ULONGLONG kRecoveryPollMs = 200;

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

    // Capture the PRIMARY monitor, not blindly output 0. The two are not
    // the same thing: DXGI enumerates outputs in the adapter's own order, so
    // on a multi-monitor desktop output 0 can easily be a secondary screen.
    // Picking the primary keeps this end aligned with VirtualPen, which maps
    // the tablet's normalized coordinates onto the rectangle reported below
    // - if the two disagreed the pen would draw on a different monitor than
    // the one the tablet is showing.
    {
        Microsoft::WRL::ComPtr<IDXGIOutput> candidate;
        for (UINT index = 0; adapter_->EnumOutputs(index, &candidate) != DXGI_ERROR_NOT_FOUND; ++index) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(candidate->GetDesc(&desc)) && desc.AttachedToDesktop &&
                desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0) {
                // The primary monitor is the one anchored at the virtual
                // desktop's origin.
                output_ = candidate;
                break;
            }
            candidate.Reset();
        }
        if (!output_) {
            hr = adapter_->EnumOutputs(0, &output_);
            if (FAILED(hr)) {
                std::cerr << "[ERROR] EnumOutputs failed: 0x" << std::hex << hr << std::endl;
                return false;
            }
            std::cerr << "[ScreenCapture] Primary output not found; falling back to output 0." << std::endl;
        }
    }

    hr = output_->QueryInterface(IID_PPV_ARGS(&output1_));
    if (FAILED(hr)) {
        std::cerr << "[ERROR] QueryInterface for Output1 failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    PrintCaptureEnvironment(adapter_.Get(), output_.Get());

    DXGI_OUTPUT_DESC outputDesc{};
    if (SUCCEEDED(output_->GetDesc(&outputDesc))) {
        desktopLeft_ = outputDesc.DesktopCoordinates.left;
        desktopTop_ = outputDesc.DesktopCoordinates.top;
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

void ScreenCapture::MarkDuplicationLost(const char* reason) {
    if (frameAcquired_ && duplication_) {
        duplication_->ReleaseFrame();
    }
    frameAcquired_ = false;
    frameTexture_.Reset();
    duplication_.Reset();
    nextRetryTick_ = GetTickCount64() + kRecoveryPollMs;
    if (!lostLogged_) {
        lostLogged_ = true;
        std::cerr << "[ScreenCapture] Desktop duplication lost (" << reason
            << "). Retrying every " << kRecoveryPollMs << "ms..." << std::endl;
    }
}

bool ScreenCapture::TryRecreateDuplication() {
    if (!output1_ || !d3dDevice_) return false;

    const HRESULT hr = output1_->DuplicateOutput(d3dDevice_.Get(), &duplication_);
    if (FAILED(hr)) {
        // Expected while the secure desktop (UAC prompt, Ctrl+Alt+Del, lock
        // screen) owns the session - E_ACCESSDENIED here just means "not yet".
        duplication_.Reset();
        nextRetryTick_ = GetTickCount64() + kRecoveryPollMs;
        return false;
    }

    // A mode switch can change the desktop size while duplication was down.
    // The encoder was created for the old size and CopyResource requires
    // matching dimensions, so warn loudly rather than streaming nothing.
    DXGI_OUTPUT_DESC desc{};
    if (SUCCEEDED(output_->GetDesc(&desc))) {
        // The monitor may also have moved within the virtual desktop while
        // duplication was down; keep the pen mapping origin in sync.
        desktopLeft_ = desc.DesktopCoordinates.left;
        desktopTop_ = desc.DesktopCoordinates.top;
        const int newWidth = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        const int newHeight = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        if (newWidth != width_ || newHeight != height_) {
            std::cerr << "[ScreenCapture] Desktop resolution changed from " << width_ << "x" << height_
                << " to " << newWidth << "x" << newHeight
                << ". The encoder is still set to the old size - restart the host." << std::endl;
        }
    }

    lostLogged_ = false;
    std::cout << "[ScreenCapture] Desktop duplication recovered." << std::endl;
    return true;
}

bool ScreenCapture::CaptureFrame() {
    // Recovery path: a UAC prompt / lock screen / Ctrl+Alt+Del switches
    // Windows to the secure desktop and kills duplication. Recreating it
    // fails until that desktop goes away, so keep retrying on a timer
    // instead of giving up permanently. Sleeping here also keeps the host
    // loop from spinning at 100% CPU while we're in the lost state, since
    // AcquireNextFrame isn't around to pace it.
    if (!duplication_) {
        const ULONGLONG now = GetTickCount64();
        if (now < nextRetryTick_) {
            Sleep(static_cast<DWORD>(nextRetryTick_ - now));
        }
        if (!TryRecreateDuplication()) {
            return false;
        }
    }

    if (frameAcquired_) ReleaseFrame();
    IDXGIResource* resource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO info{};
    HRESULT hr = duplication_->AcquireNextFrame(10, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        MarkDuplicationLost("access lost");
        return false;
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        // The D3D device itself is gone. NVENC still holds a reference to it,
        // so a silent partial recovery here would leave the encoder pointing
        // at a dead device - surface it instead of hiding it.
        std::cerr << "[ScreenCapture] D3D device removed/reset (0x" << std::hex << hr
            << std::dec << "). Restart the host to recover." << std::endl;
        MarkDuplicationLost("device removed");
        return false;
    }
    if (FAILED(hr)) {
        MarkDuplicationLost("AcquireNextFrame failed");
        return false;
    }
    if (!resource) {
        // Succeeded without a surface: nothing to encode this tick.
        duplication_->ReleaseFrame();
        return false;
    }

    frameAcquired_ = true;
    hr = resource->QueryInterface(IID_PPV_ARGS(&frameTexture_));
    resource->Release();
    if (FAILED(hr)) {
        ReleaseFrame();
        return false;
    }
    return true;
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
