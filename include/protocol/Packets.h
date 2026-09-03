#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Protocol v2: all multi-byte fields are serialized in network byte order.
constexpr uint8_t kProtocolVersion = 2;
constexpr uint16_t kProtocolMagic = 0xABBA;
constexpr uint16_t kVideoHeaderSize = 19;
constexpr uint16_t kPenHeaderSize = 23;
constexpr size_t kMaxUdpPayload = 1400;
constexpr uint16_t kVideoFragmentPayload = 1280;

constexpr uint8_t kPenActionDown = 0x00;
constexpr uint8_t kPenActionMove = 0x01;
constexpr uint8_t kPenActionUp = 0x02;
constexpr uint8_t kPenActionCancel = 0x03;

constexpr uint8_t kPenFlagEraser = 1u << 0;
constexpr uint8_t kPenFlagPalmRejection = 1u << 1;
constexpr uint8_t kPenFlagStylus = 1u << 2;
constexpr uint8_t kPenFlagUpHasLastMove = 1u << 3;

// Internal representation consumed by the Windows synthetic pen layer.
// It is not sent on the wire; use ParsePenPacket for v2 datagrams.
struct PenInputPacket {
    uint16_t sequence = 0;
    uint32_t timestamp_ms = 0;
    float x = 0.0f;
    float y = 0.0f;
    float pressure = 0.0f;
    float tilt_x = 0.0f;
    float tilt_y = 0.0f;
    uint8_t buttons = 0;
    uint8_t reserved = 0;
};

// RFC 1982 style rollover-safe comparison, shared by the video FrameID and
// pen Sequence checks.
inline bool IsSequenceNewer(uint16_t a, uint16_t b) {
    return static_cast<uint16_t>(a - b) < 0x8000;
}

inline void WriteU16BE(uint8_t* dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value >> 8);
    dst[1] = static_cast<uint8_t>(value);
}

inline void WriteU32BE(uint8_t* dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value >> 24);
    dst[1] = static_cast<uint8_t>(value >> 16);
    dst[2] = static_cast<uint8_t>(value >> 8);
    dst[3] = static_cast<uint8_t>(value);
}

inline uint16_t ReadU16BE(const uint8_t* src) {
    return static_cast<uint16_t>((static_cast<uint16_t>(src[0]) << 8) | src[1]);
}

inline uint32_t ReadU32BE(const uint8_t* src) {
    return (static_cast<uint32_t>(src[0]) << 24) |
           (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) |
           static_cast<uint32_t>(src[3]);
}

struct VideoPacketView {
    uint32_t sessionId = 0;
    uint16_t frameId = 0;
    uint32_t timestampMs = 0;
    uint16_t fragmentIndex = 0;
    uint16_t fragmentCount = 0;
};

inline std::vector<std::vector<uint8_t>> BuildVideoPackets(
    uint32_t sessionId,
    uint16_t frameId,
    uint32_t timestampMs,
    const std::vector<uint8_t>& encodedFrame) {
    const size_t fragmentCount = encodedFrame.empty()
        ? 0
        : (encodedFrame.size() + kVideoFragmentPayload - 1) / kVideoFragmentPayload;
    if (fragmentCount == 0 || fragmentCount > 0xFFFFu) {
        return {};
    }

    std::vector<std::vector<uint8_t>> packets;
    packets.reserve(fragmentCount);
    for (size_t index = 0; index < fragmentCount; ++index) {
        const size_t offset = index * kVideoFragmentPayload;
        const size_t payloadLength = std::min(
            static_cast<size_t>(kVideoFragmentPayload), encodedFrame.size() - offset);

        std::vector<uint8_t> packet(kVideoHeaderSize + payloadLength);
        packet[0] = static_cast<uint8_t>(kProtocolMagic >> 8);
        packet[1] = static_cast<uint8_t>(kProtocolMagic);
        packet[2] = kProtocolVersion;
        WriteU32BE(packet.data() + 3, sessionId);
        WriteU16BE(packet.data() + 7, frameId);
        WriteU32BE(packet.data() + 9, timestampMs);
        WriteU16BE(packet.data() + 13, static_cast<uint16_t>(index));
        WriteU16BE(packet.data() + 15, static_cast<uint16_t>(fragmentCount));
        WriteU16BE(packet.data() + 17, static_cast<uint16_t>(payloadLength));
        std::copy_n(encodedFrame.data() + offset, payloadLength, packet.data() + kVideoHeaderSize);
        packets.push_back(std::move(packet));
    }
    return packets;
}

inline bool ParseVideoPacket(
    const uint8_t* data,
    size_t length,
    VideoPacketView& view,
    const uint8_t*& payload,
    size_t& payloadLength) {
    if (!data || length < kVideoHeaderSize ||
        ReadU16BE(data) != kProtocolMagic || data[2] != kProtocolVersion) {
        return false;
    }

    view.sessionId = ReadU32BE(data + 3);
    view.frameId = ReadU16BE(data + 7);
    view.timestampMs = ReadU32BE(data + 9);
    view.fragmentIndex = ReadU16BE(data + 13);
    view.fragmentCount = ReadU16BE(data + 15);
    payloadLength = ReadU16BE(data + 17);
    if (view.fragmentCount == 0 || view.fragmentIndex >= view.fragmentCount ||
        payloadLength > kVideoFragmentPayload ||
        payloadLength != length - kVideoHeaderSize) {
        return false;
    }
    payload = data + kVideoHeaderSize;
    return true;
}

inline bool ParsePenPacket(const uint8_t* data, size_t length, PenInputPacket& packet) {
    if (!data || length != kPenHeaderSize ||
        ReadU16BE(data) != kProtocolMagic || data[2] != kProtocolVersion) {
        return false;
    }

    const uint8_t action = data[9];
    const uint8_t flags = data[10];
    if (action > kPenActionCancel) {
        return false;
    }

    packet.sequence = ReadU16BE(data + 7);
    packet.timestamp_ms = ReadU32BE(data + 19);
    packet.x = static_cast<float>(ReadU16BE(data + 15)) / 32767.0f;
    packet.y = static_cast<float>(ReadU16BE(data + 17)) / 32767.0f;
    packet.pressure = static_cast<float>(ReadU16BE(data + 11)) / 4095.0f;
    packet.tilt_x = (static_cast<float>(data[13]) - 90.0f) * 3.14159265358979323846f / 180.0f;
    packet.tilt_y = (static_cast<float>(data[14]) - 90.0f) * 3.14159265358979323846f / 180.0f;
    packet.buttons = ((action == kPenActionDown || action == kPenActionMove) ? 0x01 : 0x00);
    if ((flags & kPenFlagEraser) != 0) {
        packet.buttons |= 0x02;
    }
    return true;
}

static_assert(kVideoHeaderSize == 19, "Video header layout changed");
static_assert(kPenHeaderSize == 23, "Pen header layout changed");
