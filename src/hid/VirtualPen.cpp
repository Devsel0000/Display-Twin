// File: PenDisplayPC/src/hid/VirtualPen.cpp
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "hid/VirtualPen.h"
#include <winuser.h>   // CreateSyntheticPointerDevice
#include <cmath>

VirtualPen::VirtualPen() {
    // Screen size in pixels (used to convert normalized coordinates).
    screenWidth_ = GetSystemMetrics(SM_CXSCREEN);
    screenHeight_ = GetSystemMetrics(SM_CYSCREEN);
}

VirtualPen::~VirtualPen() {
    if (hDevice) {
        DestroySyntheticPointerDevice(hDevice);
        hDevice = nullptr;
    }
}

bool VirtualPen::Initialize() {
    // Create a virtual pointer device (PT_PEN = pen, 1 = max simultaneous
    // contacts, default feedback mode).
    hDevice = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
    return (hDevice != nullptr);
}

bool VirtualPen::ConsumePressureStats(uint32_t& count, uint32_t& minOut,
                                      uint32_t& maxOut, uint32_t& lastOut) {
    count = pressureSamples_.exchange(0, std::memory_order_relaxed);
    if (count == 0) return false;
    minOut = pressureMin_.load(std::memory_order_relaxed);
    maxOut = pressureMax_.load(std::memory_order_relaxed);
    lastOut = pressureLast_.load(std::memory_order_relaxed);
    return true;
}

void VirtualPen::InjectInput(const PenInputPacket& packet) {
    if (!hDevice) return;

    // 1. Whether the tip is pressed (button bit 0).
    bool tipDown = (packet.buttons & 0x01) != 0;
    auto clamp01 = [](float value) {
        if (!std::isfinite(value)) return 0.0f;
        if (value < 0.0f) return 0.0f;
        if (value > 1.0f) return 1.0f;
        return value;
    };

    // 2. Fill in the POINTER_PEN_INFO structure.
    POINTER_PEN_INFO penInfo = {};
    penInfo.pointerInfo.pointerType = PT_PEN;
    penInfo.pointerInfo.ptPixelLocation.x = static_cast<long>(clamp01(packet.x) * (screenWidth_ - 1));
    penInfo.pointerInfo.ptPixelLocation.y = static_cast<long>(clamp01(packet.y) * (screenHeight_ - 1));

    // 3. Set flags (InRange, InContact, Down/Move-Update/Up). A held-down
    // move MUST carry POINTER_FLAG_UPDATE (not just INRANGE|INCONTACT) or
    // Windows won't treat it as a proper contact update - position still
    // drags along via generic pointer movement, but per-sample data like
    // pressure is silently dropped without this flag.
    if (tipDown) {
        if (!tipDown_) {
            penInfo.pointerInfo.pointerFlags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
        } else {
            penInfo.pointerInfo.pointerFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
        }
    }
    else {
        penInfo.pointerInfo.pointerFlags = tipDown_ ? POINTER_FLAG_UP : (POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE);
    }
    tipDown_ = tipDown;

    // 4. Pressure (0.0~1.0 -> 0~1024). Windows treats 0 as "no pressure", so
    // while the tip is in contact clamp to at least 1 - a light stroke used
    // to round down to 0 here and read as a pressure-less contact.
    penInfo.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
    if (tipDown) {
        uint32_t pressure = static_cast<uint32_t>(clamp01(packet.pressure) * 1024.0f + 0.5f);
        if (pressure < 1) pressure = 1;
        if (pressure > 1024) pressure = 1024;
        penInfo.pressure = pressure;
    } else {
        penInfo.pressure = 0;  // Hovering / lifted: no contact pressure.
    }
    if (tipDown) {
        const uint32_t value = penInfo.pressure;
        pressureLast_.store(value, std::memory_order_relaxed);
        if (pressureSamples_.fetch_add(1, std::memory_order_relaxed) == 0) {
            pressureMin_.store(value, std::memory_order_relaxed);
            pressureMax_.store(value, std::memory_order_relaxed);
        } else {
            uint32_t prevMin = pressureMin_.load(std::memory_order_relaxed);
            while (value < prevMin &&
                   !pressureMin_.compare_exchange_weak(prevMin, value, std::memory_order_relaxed)) {
            }
            uint32_t prevMax = pressureMax_.load(std::memory_order_relaxed);
            while (value > prevMax &&
                   !pressureMax_.compare_exchange_weak(prevMax, value, std::memory_order_relaxed)) {
            }
        }
    }

    // 5. Tilt (radians -> degrees, -90~+90).
    const float tiltX = std::isfinite(packet.tilt_x) ? packet.tilt_x : 0.0f;
    const float tiltY = std::isfinite(packet.tilt_y) ? packet.tilt_y : 0.0f;
    penInfo.tiltX = static_cast<int32_t>((tiltX / 3.14159265f) * 90.0f);
    penInfo.tiltY = static_cast<int32_t>((tiltY / 3.14159265f) * 90.0f);
    if (penInfo.tiltX < -90) penInfo.tiltX = -90;
    if (penInfo.tiltX > 90) penInfo.tiltX = 90;
    if (penInfo.tiltY < -90) penInfo.tiltY = -90;
    if (penInfo.tiltY > 90) penInfo.tiltY = 90;

    // 6. Inject the virtual input.
    POINTER_TYPE_INFO pointerInfo{};
    pointerInfo.type = PT_PEN;
    pointerInfo.penInfo = penInfo;
    InjectSyntheticPointerInput(hDevice, &pointerInfo, 1);
}
