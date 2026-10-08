#pragma once

#include <d3d11.h>
#include <cstdint>
#include <vector>

// What HostController needs from an encoder, so the host can run on a PC
// without an NVIDIA GPU: NvencEncoder when NVENC is there, and
// MediaFoundationEncoder (Windows' own H.264 encoder, 720p) when it is not.
class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    virtual bool Initialize(ID3D11Device* d3dDevice) = 0;

    // Split in two so the caller can hand the captured surface back to DXGI
    // before spending time encoding.
    virtual bool CopyInput(ID3D11Texture2D* texture) = 0;
    virtual std::vector<uint8_t> Encode() = 0;

    // Makes the next Encode() emit an IDR with SPS/PPS in front of it - the
    // PC half of loss recovery (the tablet asks with a PLI). Safe to call
    // from another thread.
    virtual void RequestKeyframe() = 0;

    // QoS: change target bitrate and/or framerate live; 0 leaves a value
    // alone. Must not reset the encoder (that would cost an IDR).
    virtual bool Reconfigure(int bitrateKbps, int framerate) = 0;

    virtual bool LastWasIdr() const = 0;
    virtual double LastSubmitMs() const = 0;
    virtual double LastReadbackMs() const = 0;
    virtual size_t LastBitstreamBytes() const = 0;
};
