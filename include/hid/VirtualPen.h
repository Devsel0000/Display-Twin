// File: PenDisplayPC/include/hid/VirtualPen.h
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winuser.h>  // CreateSyntheticPointerDevice API ?ъ슜
#include <cstdint>
#include "../protocol/Packets.h"

// Android?먯꽌 ?ㅻ뒗 ?⑦궥 援ъ“泥?(洹몃?濡??좎?)
class VirtualPen {
public:
    VirtualPen();
    ~VirtualPen();

    bool Initialize();                    // CreateSyntheticPointerDevice ?몄텧
    void InjectInput(const PenInputPacket& packet); // InjectSyntheticPointerInput ?몄텧

private:
    HSYNTHETICPOINTERDEVICE hDevice = nullptr;
    int screenWidth_ = 0;
    int screenHeight_ = 0;
    bool tipDown_ = false;
};
