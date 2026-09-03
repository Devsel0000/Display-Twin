// File: PenDisplayPC/include/hid/VirtualPen.h
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winuser.h>  // CreateSyntheticPointerDevice API
#include <atomic>
#include <cstdint>
#include "../protocol/Packets.h"

// Injects the pen packets received from Android as Windows synthetic pointer
// input (as-is, no coordinate remapping beyond screen-space scaling).
class VirtualPen {
public:
    VirtualPen();
    ~VirtualPen();

    bool Initialize();                    // Calls CreateSyntheticPointerDevice
    void InjectInput(const PenInputPacket& packet); // Calls InjectSyntheticPointerInput

    // Diagnostics: reports the pressure values (0-1024) actually handed to
    // Windows since the last call, then resets the window. Returns false if
    // no in-contact samples arrived. Lets the host log prove whether varying
    // pressure is reaching the OS, which separates a transport/device problem
    // from a receiving-application problem.
    bool ConsumePressureStats(uint32_t& count, uint32_t& minOut, uint32_t& maxOut, uint32_t& lastOut);

private:
    HSYNTHETICPOINTERDEVICE hDevice = nullptr;
    int screenWidth_ = 0;
    int screenHeight_ = 0;
    bool tipDown_ = false;

    // Written on the UDP receive thread, read on the host loop thread.
    std::atomic<uint32_t> pressureSamples_{0};
    std::atomic<uint32_t> pressureMin_{0};
    std::atomic<uint32_t> pressureMax_{0};
    std::atomic<uint32_t> pressureLast_{0};
};
