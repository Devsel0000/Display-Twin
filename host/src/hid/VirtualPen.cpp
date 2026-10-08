// File: PenDisplayPC/src/hid/VirtualPen.cpp
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "hid/VirtualPen.h"
#include <winuser.h>   // CreateSyntheticPointerDevice
#include <cmath>

VirtualPen::VirtualPen() {
    // Default target: the primary monitor. SetTargetRect() replaces this
    // with the rectangle of the output the host actually captures.
    targetLeft_ = 0;
    targetTop_ = 0;
    targetWidth_ = GetSystemMetrics(SM_CXSCREEN);
    targetHeight_ = GetSystemMetrics(SM_CYSCREEN);
}

void VirtualPen::SetTargetRect(int left, int top, int width, int height) {
    if (width <= 0 || height <= 0) return;
    targetLeft_ = left;
    targetTop_ = top;
    targetWidth_ = width;
    targetHeight_ = height;
}

VirtualPen::~VirtualPen() {
    if (hDevice) {
        DestroySyntheticPointerDevice(hDevice);
        hDevice = nullptr;
    }
    if (hTouch_) {
        DestroySyntheticPointerDevice(hTouch_);
        hTouch_ = nullptr;
    }
}

bool VirtualPen::Initialize() {
    // Create a virtual pointer device (PT_PEN = pen, 1 = max simultaneous
    // contacts, default feedback mode).
    hDevice = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
    hTouch_ = CreateSyntheticPointerDevice(PT_TOUCH, kMaxTouchContacts, POINTER_FEEDBACK_DEFAULT);
    return (hDevice != nullptr);
}

void VirtualPen::SendTouchFrame(const std::vector<POINTER_TYPE_INFO>& frame) {
    if (hTouch_ && !frame.empty()) {
        InjectSyntheticPointerInput(hTouch_, frame.data(), static_cast<UINT32>(frame.size()));
    }
}

void VirtualPen::InjectTouch(const TouchContact* contacts, int count) {
    if (!hTouch_) return;
    if (count > kMaxTouchContacts) count = kMaxTouchContacts;

    std::lock_guard<std::mutex> lock(touchMutex_);
    std::vector<POINTER_TYPE_INFO> frame;
    std::vector<ActiveTouch> next;

    auto make = [](uint8_t id, long x, long y, POINTER_FLAGS flags) {
        POINTER_TYPE_INFO info{};
        info.type = PT_TOUCH;
        POINTER_TOUCH_INFO& t = info.touchInfo;
        t.pointerInfo.pointerType = PT_TOUCH;
        t.pointerInfo.pointerId = id;
        t.pointerInfo.ptPixelLocation.x = x;
        t.pointerInfo.ptPixelLocation.y = y;
        t.pointerInfo.pointerFlags = flags;
        t.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE;
        t.rcContact = {x - 4, y - 4, x + 4, y + 4};
        t.pressure = (flags & POINTER_FLAG_UP) ? 0 : 512;
        return info;
    };

    for (int i = 0; i < count; ++i) {
        const long x = targetLeft_.load() + static_cast<long>(
            std::fmin(std::fmax(contacts[i].x, 0.0f), 1.0f) * (targetWidth_.load() - 1));
        const long y = targetTop_.load() + static_cast<long>(
            std::fmin(std::fmax(contacts[i].y, 0.0f), 1.0f) * (targetHeight_.load() - 1));
        bool known = false;
        for (const ActiveTouch& a : touchActive_) known |= (a.id == contacts[i].id);
        frame.push_back(make(contacts[i].id, x, y,
            known ? (POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT)
                  : (POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT)));
        next.push_back({contacts[i].id, x, y});
    }
    for (const ActiveTouch& a : touchActive_) {
        bool still = false;
        for (const ActiveTouch& n : next) still |= (n.id == a.id);
        if (!still) frame.push_back(make(a.id, a.x, a.y, POINTER_FLAG_UP));
    }
    touchActive_ = next;
    touchLastTick_ = GetTickCount64();
    SendTouchFrame(frame);
}

void VirtualPen::ReleaseStaleTouch(uint32_t timeoutMs) {
    std::lock_guard<std::mutex> lock(touchMutex_);
    if (touchActive_.empty() || GetTickCount64() - touchLastTick_ < timeoutMs) return;
    std::vector<POINTER_TYPE_INFO> frame;
    for (const ActiveTouch& a : touchActive_) {
        POINTER_TYPE_INFO info{};
        info.type = PT_TOUCH;
        info.touchInfo.pointerInfo.pointerType = PT_TOUCH;
        info.touchInfo.pointerInfo.pointerId = a.id;
        info.touchInfo.pointerInfo.ptPixelLocation = {a.x, a.y};
        info.touchInfo.pointerInfo.pointerFlags = POINTER_FLAG_UP;
        frame.push_back(info);
    }
    touchActive_.clear();
    SendTouchFrame(frame);
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

void VirtualPen::ReleaseIfDown() {
    if (!hDevice || !tipDown_) return;

    POINTER_PEN_INFO penInfo = {};
    penInfo.pointerInfo.pointerType = PT_PEN;
    penInfo.pointerInfo.pointerFlags = POINTER_FLAG_UP;
    penInfo.pointerInfo.ptPixelLocation.x = lastPixelX_;
    penInfo.pointerInfo.ptPixelLocation.y = lastPixelY_;
    tipDown_ = false;

    POINTER_TYPE_INFO pointerInfo{};
    pointerInfo.type = PT_PEN;
    pointerInfo.penInfo = penInfo;
    InjectSyntheticPointerInput(hDevice, &pointerInfo, 1);
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
    penInfo.pointerInfo.ptPixelLocation.x =
        targetLeft_.load() + static_cast<long>(clamp01(packet.x) * (targetWidth_.load() - 1));
    penInfo.pointerInfo.ptPixelLocation.y =
        targetTop_.load() + static_cast<long>(clamp01(packet.y) * (targetHeight_.load() - 1));
    lastPixelX_ = penInfo.pointerInfo.ptPixelLocation.x;
    lastPixelY_ = penInfo.pointerInfo.ptPixelLocation.y;

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

    // 5. Tilt (radians -> degrees, -90~+90). radians * 180/pi - using 90
    //    here (as this did originally) halves every tilt angle, so a pen
    //    leaned 30 degrees reached Windows as 15.
    const float tiltX = std::isfinite(packet.tilt_x) ? packet.tilt_x : 0.0f;
    const float tiltY = std::isfinite(packet.tilt_y) ? packet.tilt_y : 0.0f;
    penInfo.tiltX = static_cast<int32_t>((tiltX / 3.14159265f) * 180.0f);
    penInfo.tiltY = static_cast<int32_t>((tiltY / 3.14159265f) * 180.0f);
    if (penInfo.tiltX < -90) penInfo.tiltX = -90;
    if (penInfo.tiltX > 90) penInfo.tiltX = 90;
    if (penInfo.tiltY < -90) penInfo.tiltY = -90;
    if (penInfo.tiltY > 90) penInfo.tiltY = 90;

    // 6. Eraser. The protocol carries an eraser bit (Flags bit 0, parsed
    //    into buttons bit 1) which used to be dropped on the floor here.
    //    PEN_FLAG_ERASER is what Windows Ink looks at; INVERTED reports the
    //    pen as being used tail-first, which is how a physical eraser end
    //    presents itself and what apps that only check inversion expect.
    if ((packet.buttons & 0x02) != 0) {
        penInfo.penFlags = PEN_FLAG_ERASER | PEN_FLAG_INVERTED;
    }

    // 7. Inject the virtual input.
    POINTER_TYPE_INFO pointerInfo{};
    pointerInfo.type = PT_PEN;
    pointerInfo.penInfo = penInfo;
    InjectSyntheticPointerInput(hDevice, &pointerInfo, 1);
}
