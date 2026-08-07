// File: PenDisplayPC/src/hid/VirtualPen.cpp
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "hid/VirtualPen.h"
#include <winuser.h>   // ??異붽?: CreateSyntheticPointerDevice ?ъ슜
#include <cmath>

VirtualPen::VirtualPen() {
    // ?붾㈃ ?댁긽??誘몃━ ???(醫뚰몴 蹂?섏슜)
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
    // 媛?????μ튂 ?앹꽦 (PT_PEN = ????? 1 = 理쒕? ?숈떆 ?묒큺 ?? ?쇰뱶諛?湲곕낯媛?
    hDevice = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
    return (hDevice != nullptr);
}

void VirtualPen::InjectInput(const PenInputPacket& packet) {
    if (!hDevice) return;

    // 1. ??Tip)???뚮졇?붿? ?뺤씤 (踰꾪듉 鍮꾪듃 0)
    bool tipDown = (packet.buttons & 0x01) != 0;
    auto clamp01 = [](float value) {
        if (!std::isfinite(value)) return 0.0f;
        if (value < 0.0f) return 0.0f;
        if (value > 1.0f) return 1.0f;
        return value;
    };

    // 2. POINTER_PEN_INFO 援ъ“泥?梨꾩슦湲?
    POINTER_PEN_INFO penInfo = {};
    penInfo.pointerInfo.pointerType = PT_PEN;
    penInfo.pointerInfo.ptPixelLocation.x = static_cast<long>(clamp01(packet.x) * (screenWidth_ - 1));
    penInfo.pointerInfo.ptPixelLocation.y = static_cast<long>(clamp01(packet.y) * (screenHeight_ - 1));

    // 3. ?뚮옒洹??ㅼ젙 (InRange, InContact, Down/Up)
    if (tipDown) {
        penInfo.pointerInfo.pointerFlags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
        if (!tipDown_) penInfo.pointerInfo.pointerFlags |= POINTER_FLAG_DOWN;
    }
    else {
        penInfo.pointerInfo.pointerFlags = tipDown_ ? POINTER_FLAG_UP : POINTER_FLAG_INRANGE;
    }
    tipDown_ = tipDown;

    // 4. ?꾩븬 (0.0~1.0 ??0~1024)
    penInfo.pressure = static_cast<uint32_t>(clamp01(packet.pressure) * 1024.0f);
    penInfo.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
    if (penInfo.pressure > 1024) penInfo.pressure = 1024;

    // 5. ?명듃 (?쇰뵒????-90~+90??
    const float tiltX = std::isfinite(packet.tilt_x) ? packet.tilt_x : 0.0f;
    const float tiltY = std::isfinite(packet.tilt_y) ? packet.tilt_y : 0.0f;
    penInfo.tiltX = static_cast<int32_t>((tiltX / 3.14159265f) * 90.0f);
    penInfo.tiltY = static_cast<int32_t>((tiltY / 3.14159265f) * 90.0f);
    if (penInfo.tiltX < -90) penInfo.tiltX = -90;
    if (penInfo.tiltX > 90) penInfo.tiltX = 90;
    if (penInfo.tiltY < -90) penInfo.tiltY = -90;
    if (penInfo.tiltY > 90) penInfo.tiltY = 90;

    // 6. 媛???낅젰 二쇱엯
    POINTER_TYPE_INFO pointerInfo{};
    pointerInfo.type = PT_PEN;
    pointerInfo.penInfo = penInfo;
    InjectSyntheticPointerInput(hDevice, &pointerInfo, 1);
}
