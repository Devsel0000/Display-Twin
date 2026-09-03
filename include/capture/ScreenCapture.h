// File: PenDisplayPC/include/capture/ScreenCapture.h
#pragma once
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <wrl/client.h>

class ScreenCapture {
public:
	ScreenCapture();
	~ScreenCapture();

	bool Initialize();
	bool CaptureFrame();          // Acquire the next frame (non-blocking)
	ID3D11Texture2D* GetFrameTexture() const;
	void ReleaseFrame();          // Release what AcquireNextFrame acquired
	ID3D11Device* GetD3DDevice() const;
	int GetWidth() const;
	int GetHeight() const;

	// True while desktop duplication is unavailable (secure desktop, etc.).
	bool IsDuplicationLost() const { return duplication_ == nullptr; }

private:
	// Drops the current duplication object and arms the recovery retry.
	void MarkDuplicationLost(const char* reason);
	// Tries to recreate the duplication object; returns true on success.
	bool TryRecreateDuplication();

	Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
	Microsoft::WRL::ComPtr<IDXGIFactory1> factory_;
	Microsoft::WRL::ComPtr<IDXGIAdapter> adapter_;
	Microsoft::WRL::ComPtr<IDXGIOutput> output_;
	Microsoft::WRL::ComPtr<IDXGIOutput1> output1_;
	Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> frameTexture_;
	int width_ = 0;
	int height_ = 0;
	bool frameAcquired_ = false;
	bool lostLogged_ = false;      // Log the loss/recovery once, not per frame.
	ULONGLONG nextRetryTick_ = 0;  // GetTickCount64() gate for recovery retries.
};
