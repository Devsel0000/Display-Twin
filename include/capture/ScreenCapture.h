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
	bool CaptureFrame();          // ?ㅼ쓬 ?꾨젅???띾뱷 (non-blocking)
	ID3D11Texture2D* GetFrameTexture() const;
	void ReleaseFrame();          // AcquireNextFrame ?댁젣
	ID3D11Device* GetD3DDevice() const;
	int GetWidth() const;
	int GetHeight() const;

private:
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
};
