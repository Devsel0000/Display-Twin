#pragma once

// Control channel (UDP 5002) and discovery (UDP 9999).
//
// Everything the two ends need to say to each other that isn't video or pen
// samples travels here: the session handshake, clock sync, the tablet's
// "I lost a frame, please refresh" (PLI), its periodic decoder report that
// drives PC-side QoS, and the acknowledgements for pen state changes.
//
// Layout, shared with the Kotlin side (ControlChannel.kt):
//
//   0   2  Magic (0xABBA)
//   2   1  Version (0x02)
//   3   4  SessionID (0 until WELCOME assigns one)
//   7   1  Type
//   8   2  PayloadLength
//   10  N  Payload

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Packets.h"

constexpr uint16_t kControlHeaderSize = 10;

constexpr uint8_t kControlPing = 0x01;
constexpr uint8_t kControlPong = 0x02;
constexpr uint8_t kControlPli = 0x11;
constexpr uint8_t kControlHello = 0x20;
constexpr uint8_t kControlWelcome = 0x21;
constexpr uint8_t kControlBusy = 0x22;
constexpr uint8_t kControlStatus = 0x30;
constexpr uint8_t kControlPenAck = 0x40;

// Tablet -> PC finger state. Payload: count(1), then per contact
// id(1) x(u16 BE) y(u16 BE), x/y normalized 0..32767. Always the COMPLETE
// current set (count 0 = all fingers up), resent every ~100ms while a finger
// is down, so a lost datagram is harmless; the PC derives DOWN/UP by diffing.
constexpr uint8_t kControlTouch = 0x50;

// Tablet -> PC decoder/network report, sent once a second. Drives QoS.
constexpr uint16_t kStatusPayloadSize = 12;
struct ClientStatus {
    uint8_t decodeQueueLength = 0;  // frames waiting for the decoder
    uint16_t lossPermille = 0;      // fragments lost per 1000 expected
    uint16_t completedFps = 0;      // frames fully reassembled last second
    uint16_t droppedFrames = 0;     // frames discarded (incomplete or stale)
    uint16_t decodeMsX10 = 0;       // average decode+render time, tenths of ms
    uint16_t rttMs = 0;             // last measured round trip
};

// Discovery is plain ASCII so it stays readable in a packet capture.
// (Arrays rather than inline variables: the project builds as C++14.)
constexpr const char kDiscoveryRequest[] = "DTWIN-DISCOVER/1";
constexpr const char kDiscoveryOfferPrefix[] = "DTWIN-OFFER/1|";

struct ControlPacketView {
    uint32_t sessionId = 0;
    uint8_t type = 0;
    const uint8_t* payload = nullptr;
    size_t payloadLength = 0;
};

inline std::vector<uint8_t> BuildControlPacket(uint8_t type, uint32_t sessionId,
                                               const uint8_t* payload, size_t payloadLength) {
    std::vector<uint8_t> packet(kControlHeaderSize + payloadLength);
    packet[0] = static_cast<uint8_t>(kProtocolMagic >> 8);
    packet[1] = static_cast<uint8_t>(kProtocolMagic);
    packet[2] = kProtocolVersion;
    WriteU32BE(packet.data() + 3, sessionId);
    packet[7] = type;
    WriteU16BE(packet.data() + 8, static_cast<uint16_t>(payloadLength));
    if (payload && payloadLength > 0) {
        std::copy_n(payload, payloadLength, packet.data() + kControlHeaderSize);
    }
    return packet;
}

inline std::vector<uint8_t> BuildControlPacket(uint8_t type, uint32_t sessionId) {
    return BuildControlPacket(type, sessionId, nullptr, 0);
}

inline bool ParseControlPacket(const uint8_t* data, size_t length, ControlPacketView& view) {
    if (!data || length < kControlHeaderSize ||
        ReadU16BE(data) != kProtocolMagic || data[2] != kProtocolVersion) {
        return false;
    }
    view.sessionId = ReadU32BE(data + 3);
    view.type = data[7];
    view.payloadLength = ReadU16BE(data + 8);
    if (view.payloadLength != length - kControlHeaderSize) {
        return false;
    }
    view.payload = data + kControlHeaderSize;
    return true;
}

// PONG carries the three timestamps the tablet needs for the NTP-style
// offset calculation: its own send time echoed back, plus when the PC
// received and answered.
inline std::vector<uint8_t> BuildPong(uint32_t sessionId, uint64_t t1, uint64_t t2, uint64_t t3) {
    uint8_t payload[24];
    WriteU64BE(payload, t1);
    WriteU64BE(payload + 8, t2);
    WriteU64BE(payload + 16, t3);
    return BuildControlPacket(kControlPong, sessionId, payload, sizeof(payload));
}

inline std::vector<uint8_t> BuildWelcome(uint32_t sessionId, const std::string& version) {
    std::vector<uint8_t> payload(4 + version.size());
    WriteU32BE(payload.data(), sessionId);
    std::copy(version.begin(), version.end(), payload.begin() + 4);
    return BuildControlPacket(kControlWelcome, sessionId, payload.data(), payload.size());
}

inline std::vector<uint8_t> BuildPenAck(uint32_t sessionId, uint16_t sequence) {
    uint8_t payload[2];
    WriteU16BE(payload, sequence);
    return BuildControlPacket(kControlPenAck, sessionId, payload, sizeof(payload));
}

inline bool ParseStatus(const uint8_t* payload, size_t length, ClientStatus& status) {
    if (!payload || length < kStatusPayloadSize) return false;
    status.decodeQueueLength = payload[0];
    // payload[1] is reserved padding so the numeric fields stay 2-aligned.
    status.lossPermille = ReadU16BE(payload + 2);
    status.completedFps = ReadU16BE(payload + 4);
    status.droppedFrames = ReadU16BE(payload + 6);
    status.decodeMsX10 = ReadU16BE(payload + 8);
    status.rttMs = ReadU16BE(payload + 10);
    return true;
}

inline std::vector<uint8_t> BuildStatus(uint32_t sessionId, const ClientStatus& status) {
    uint8_t payload[kStatusPayloadSize] = {};
    payload[0] = status.decodeQueueLength;
    payload[1] = 0;
    WriteU16BE(payload + 2, status.lossPermille);
    WriteU16BE(payload + 4, status.completedFps);
    WriteU16BE(payload + 6, status.droppedFrames);
    WriteU16BE(payload + 8, status.decodeMsX10);
    WriteU16BE(payload + 10, status.rttMs);
    return BuildControlPacket(kControlStatus, sessionId, payload, sizeof(payload));
}
