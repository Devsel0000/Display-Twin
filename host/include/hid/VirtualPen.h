// File: PenDisplayPC/include/hid/VirtualPen.h
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winuser.h>  // CreateSyntheticPointerDevice API
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include "../protocol/Packets.h"

// Injects the pen packets received from Android as Windows synthetic pointer
// input (as-is, no coordinate remapping beyond screen-space scaling).
class VirtualPen {
public:
    VirtualPen();
    ~VirtualPen();

    bool Initialize();                    // Calls CreateSyntheticPointerDevice
    void InjectInput(const PenInputPacket& packet); // Calls InjectSyntheticPointerInput

    // Desktop rectangle the normalized (0..1) pen coordinates map onto.
    // Defaults to the primary monitor. The host sets this to the rectangle
    // of the output it actually captures, so on a multi-monitor desktop the
    // pen lands on the monitor the tablet is showing instead of wherever
    // the primary monitor happens to be.
    void SetTargetRect(int left, int top, int width, int height);

    // Lifts the tip if it is currently down, otherwise a no-op. Call this
    // when the input session is considered gone (timeout, reconnect) so a
    // stroke that was mid-drag when the link dropped doesn't leave Windows
    // thinking the pen is still pressed forever.
    void ReleaseIfDown();

    // Finger touch, injected as a second synthetic device (PT_TOUCH, 2
    // contacts) so Windows itself turns it into taps, long-press, drag,
    // two-finger scroll/pinch/rotate. Each call carries the COMPLETE set of
    // current contacts (normalized 0..1); the diff against the previous call
    // becomes DOWN / UPDATE / UP, so a lost datagram heals on the next one.
    struct TouchContact { uint8_t id; float x; float y; };
    static constexpr int kMaxTouchContacts = 2;  // 3+ finger system gestures stay out on purpose
    void InjectTouch(const TouchContact* contacts, int count);
    // Lifts contacts that have gone quiet for timeoutMs (the lift datagram
    // was lost, or the link dropped). Called from the host loop.
    void ReleaseStaleTouch(uint32_t timeoutMs);

    // Diagnostics: reports the pressure values (0-1024) actually handed to
    // Windows since the last call, then resets the window. Returns false if
    // no in-contact samples arrived. Lets the host log prove whether varying
    // pressure is reaching the OS, which separates a transport/device problem
    // from a receiving-application problem.
    bool ConsumePressureStats(uint32_t& count, uint32_t& minOut, uint32_t& maxOut, uint32_t& lastOut);

private:
    HSYNTHETICPOINTERDEVICE hDevice = nullptr;
    HSYNTHETICPOINTERDEVICE hTouch_ = nullptr;
    std::mutex touchMutex_;
    struct ActiveTouch { uint8_t id; long x; long y; };
    std::vector<ActiveTouch> touchActive_;  // guarded by touchMutex_
    ULONGLONG touchLastTick_ = 0;
    void SendTouchFrame(const std::vector<POINTER_TYPE_INFO>& frame);
    // Target rectangle in virtual-desktop pixels. Read on the UDP receive
    // thread, written from the host loop thread when the capture output is
    // known, hence atomic.
    std::atomic<int> targetLeft_{0};
    std::atomic<int> targetTop_{0};
    std::atomic<int> targetWidth_{0};
    std::atomic<int> targetHeight_{0};
    // InjectInput() runs on the UDP receive thread; ReleaseIfDown() is also
    // called from the host's main loop thread on a session timeout/reset -
    // atomic so that doesn't race with a concurrent InjectInput() call.
    std::atomic<bool> tipDown_{false};
    std::atomic<long> lastPixelX_{0};
    std::atomic<long> lastPixelY_{0};

    // Written on the UDP receive thread, read on the host loop thread.
    std::atomic<uint32_t> pressureSamples_{0};
    std::atomic<uint32_t> pressureMin_{0};
    std::atomic<uint32_t> pressureMax_{0};
    std::atomic<uint32_t> pressureLast_{0};
};
