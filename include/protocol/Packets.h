#pragma once

#include <cstddef>
#include <cstdint>

#pragma pack(push, 1)
struct VideoPacketHeader {
    uint32_t magic;
    uint32_t frameIndex;
    uint32_t dataSize;
};

struct PenInputPacket {
    uint32_t magic;
    uint32_t timestamp_ms;
    float x;
    float y;
    float pressure;
    float tilt_x;
    float tilt_y;
    uint8_t buttons;
    uint8_t reserved;
};
#pragma pack(pop)

constexpr uint32_t kVideoPacketMagic = 0x564944;
constexpr uint32_t kPenInputPacketMagic = 0x50454E;
constexpr size_t kMaxUdpPayload = 65507;

static_assert(sizeof(VideoPacketHeader) == 12, "Video packet header layout changed");
