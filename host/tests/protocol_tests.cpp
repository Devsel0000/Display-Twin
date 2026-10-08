// Protocol unit tests. Pure header logic - no sockets, no GPU, no Windows
// UI - so this runs anywhere the toolset does and is the one part of the
// system that can be verified without a tablet on the desk.
//
// Build/run:  msbuild tests\ProtocolTests.vcxproj -p:Configuration=Debug -p:Platform=x64
//             tests\x64\Debug\ProtocolTests.exe

#include <cmath>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#include <string>
#include <vector>

#include <d3d11.h>

#include "encoder/MediaFoundationEncoder.h"
#include "protocol/Control.h"
#include "protocol/Packets.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

std::vector<uint8_t> MakeFrame(size_t bytes, uint8_t seed) {
    std::vector<uint8_t> frame(bytes);
    for (size_t i = 0; i < bytes; ++i) {
        frame[i] = static_cast<uint8_t>((i * 31u + seed) & 0xFF);
    }
    return frame;
}

void TestSequenceComparison() {
    std::printf("sequence comparison\n");
    CHECK(IsSequenceNewer(2, 1));
    CHECK(!IsSequenceNewer(1, 2));
    // Rollover: 0 is newer than 65535, not 65535 older by 65535.
    CHECK(IsSequenceNewer(0, 65535));
    CHECK(!IsSequenceNewer(65535, 0));
}

void TestVideoRoundTrip() {
    std::printf("video packetize/parse round trip\n");
    const auto frame = MakeFrame(5000, 7);  // 4 fragments: 1280*3 + 1160
    const auto packets = BuildVideoPackets(0xDEADBEEF, 1234, 99, frame);
    CHECK(packets.size() == 4);

    std::vector<uint8_t> rebuilt;
    for (size_t i = 0; i < packets.size(); ++i) {
        VideoPacketView view;
        const uint8_t* payload = nullptr;
        size_t payloadLength = 0;
        CHECK(ParseVideoPacket(packets[i].data(), packets[i].size(), view, payload, payloadLength));
        CHECK(view.sessionId == 0xDEADBEEF);
        CHECK(view.frameId == 1234);
        CHECK(view.timestampMs == 99);
        CHECK(view.fragmentIndex == i);
        CHECK(view.fragmentCount == 4);
        CHECK(!view.isParity);
        rebuilt.insert(rebuilt.end(), payload, payload + payloadLength);
    }
    CHECK(rebuilt == frame);
}

void TestVideoRejectsMalformed() {
    std::printf("video parser rejects malformed datagrams\n");
    auto packets = BuildVideoPackets(1, 1, 1, MakeFrame(2000, 3));
    CHECK(!packets.empty());
    VideoPacketView view;
    const uint8_t* payload = nullptr;
    size_t payloadLength = 0;

    auto bad = packets[0];
    bad[0] = 0x00;  // wrong magic
    CHECK(!ParseVideoPacket(bad.data(), bad.size(), view, payload, payloadLength));

    bad = packets[0];
    bad[2] = 0x01;  // v1 packet against a v2 parser
    CHECK(!ParseVideoPacket(bad.data(), bad.size(), view, payload, payloadLength));

    // PayloadLength that disagrees with the datagram size is the buffer
    // overrun case the spec calls out explicitly.
    bad = packets[0];
    WriteU16BE(bad.data() + 17, 900);
    CHECK(!ParseVideoPacket(bad.data(), bad.size(), view, payload, payloadLength));

    // Truncated header.
    CHECK(!ParseVideoPacket(packets[0].data(), 5, view, payload, payloadLength));
}

void TestFecRecoversOneLostFragment() {
    std::printf("FEC recovers a single lost fragment per group\n");
    // 10 fragments: 9 protected (the last is short and stays out of FEC),
    // so two groups - 8 + 1. The one-member group gets no parity.
    const auto frame = MakeFrame(1280 * 9 + 300, 11);
    const uint16_t fragmentCount = VideoFragmentCountFor(frame.size());
    CHECK(fragmentCount == 10);
    CHECK(FecProtectedFragmentCount(fragmentCount) == 9);
    CHECK(FecGroupCount(fragmentCount) == 2);

    const auto data = BuildVideoPackets(5, 5, 5, frame);
    const auto parity = BuildFecPackets(5, 5, 5, frame);
    // Group 1 holds a single fragment, so only group 0 produces parity.
    CHECK(parity.size() == 1);

    VideoPacketView view;
    const uint8_t* payload = nullptr;
    size_t payloadLength = 0;
    CHECK(ParseVideoPacket(parity[0].data(), parity[0].size(), view, payload, payloadLength));
    CHECK(view.isParity);
    CHECK(view.parityGroup == 0);
    CHECK(payloadLength == kVideoFragmentPayload);

    // Drop fragment 3 and rebuild it by XOR-ing the parity with the group's
    // survivors - the same arithmetic VideoReassembler.kt performs.
    std::vector<uint8_t> recovered(payload, payload + payloadLength);
    uint16_t begin = 0, end = 0;
    FecGroupRange(fragmentCount, 0, begin, end);
    CHECK(begin == 0 && end == 8);
    for (uint16_t index = begin; index < end; ++index) {
        if (index == 3) continue;
        const uint8_t* fragment = data[index].data() + kVideoHeaderSize;
        for (uint16_t byte = 0; byte < kVideoFragmentPayload; ++byte) {
            recovered[byte] ^= fragment[byte];
        }
    }
    const uint8_t* original = frame.data() + 3 * kVideoFragmentPayload;
    CHECK(std::memcmp(recovered.data(), original, kVideoFragmentPayload) == 0);
}

void TestFecSkippedForSmallFrames() {
    std::printf("FEC produces nothing when there is nothing to protect\n");
    // One fragment: nothing is protected, so no parity and no wasted packet.
    CHECK(BuildFecPackets(1, 1, 1, MakeFrame(500, 2)).empty());
    // Two fragments: only fragment 0 is protected, and a group of one
    // cannot recover anything.
    CHECK(BuildFecPackets(1, 1, 1, MakeFrame(1280 + 100, 2)).empty());
}

void TestPenRoundTrip() {
    std::printf("pen packet parse\n");
    uint8_t packet[kPenHeaderSize] = {};
    WriteU16BE(packet, kProtocolMagic);
    packet[2] = kProtocolVersion;
    WriteU32BE(packet + 3, 0x11223344);
    WriteU16BE(packet + 7, 4242);
    packet[9] = kPenActionMove;
    packet[10] = kPenFlagStylus | kPenFlagEraser;
    WriteU16BE(packet + 11, 2048);
    packet[13] = 120;  // +30 degrees
    packet[14] = 60;   // -30 degrees
    WriteU16BE(packet + 15, 16384);
    WriteU16BE(packet + 17, 8192);
    WriteU32BE(packet + 19, 777);

    PenInputPacket parsed;
    CHECK(ParsePenPacket(packet, sizeof(packet), parsed));
    CHECK(parsed.sessionId == 0x11223344);
    CHECK(parsed.sequence == 4242);
    CHECK(parsed.action == kPenActionMove);
    CHECK((parsed.flags & kPenFlagStylus) != 0);
    CHECK(parsed.timestamp_ms == 777);
    CHECK(std::fabs(parsed.x - 0.5f) < 0.001f);
    CHECK(std::fabs(parsed.y - 0.25f) < 0.001f);
    CHECK(std::fabs(parsed.pressure - 0.5001f) < 0.001f);
    // Tip in contact for MOVE, eraser bit carried through to buttons.
    CHECK((parsed.buttons & 0x01) != 0);
    CHECK((parsed.buttons & 0x02) != 0);

    // The wire carries degrees; VirtualPen converts back with *180/pi. A
    // 30-degree tilt must arrive as 30 degrees, not 15 - the halving bug
    // this test exists to pin down.
    const float degreesX = (parsed.tilt_x / 3.14159265f) * 180.0f;
    const float degreesY = (parsed.tilt_y / 3.14159265f) * 180.0f;
    CHECK(std::fabs(degreesX - 30.0f) < 0.01f);
    CHECK(std::fabs(degreesY + 30.0f) < 0.01f);
}

void TestPenRejectsMalformed() {
    std::printf("pen parser rejects malformed datagrams\n");
    uint8_t packet[kPenHeaderSize] = {};
    WriteU16BE(packet, kProtocolMagic);
    packet[2] = kProtocolVersion;
    packet[9] = kPenActionMove;
    PenInputPacket parsed;
    CHECK(ParsePenPacket(packet, sizeof(packet), parsed));

    CHECK(!ParsePenPacket(packet, sizeof(packet) - 1, parsed));  // wrong length
    packet[9] = 0x7F;                                            // unknown action
    CHECK(!ParsePenPacket(packet, sizeof(packet), parsed));
}

void TestControlRoundTrip() {
    std::printf("control packet build/parse\n");
    const auto pli = BuildControlPacket(kControlPli, 0xABCD1234);
    ControlPacketView view;
    CHECK(ParseControlPacket(pli.data(), pli.size(), view));
    CHECK(view.type == kControlPli);
    CHECK(view.sessionId == 0xABCD1234);
    CHECK(view.payloadLength == 0);

    const auto pong = BuildPong(7, 100, 200, 205);
    CHECK(ParseControlPacket(pong.data(), pong.size(), view));
    CHECK(view.type == kControlPong);
    CHECK(view.payloadLength == 24);
    CHECK(ReadU64BE(view.payload) == 100);
    CHECK(ReadU64BE(view.payload + 8) == 200);
    CHECK(ReadU64BE(view.payload + 16) == 205);

    const auto welcome = BuildWelcome(0x0BADF00D, "DisplayTwin-PC/4.2");
    CHECK(ParseControlPacket(welcome.data(), welcome.size(), view));
    CHECK(view.type == kControlWelcome);
    CHECK(ReadU32BE(view.payload) == 0x0BADF00D);
    CHECK(std::string(reinterpret_cast<const char*>(view.payload + 4), view.payloadLength - 4) ==
          "DisplayTwin-PC/4.2");

    const auto ack = BuildPenAck(9, 31337);
    CHECK(ParseControlPacket(ack.data(), ack.size(), view));
    CHECK(view.type == kControlPenAck);
    CHECK(ReadU16BE(view.payload) == 31337);

    // A truncated/lying PayloadLength must not be trusted.
    auto broken = pong;
    WriteU16BE(broken.data() + 8, 99);
    CHECK(!ParseControlPacket(broken.data(), broken.size(), view));
}

void TestStatusRoundTrip() {
    std::printf("client status round trip\n");
    ClientStatus sent;
    sent.decodeQueueLength = 3;
    sent.lossPermille = 27;
    sent.completedFps = 58;
    sent.droppedFrames = 4;
    sent.decodeMsX10 = 93;
    sent.rttMs = 12;

    const auto packet = BuildStatus(42, sent);
    ControlPacketView view;
    CHECK(ParseControlPacket(packet.data(), packet.size(), view));
    CHECK(view.type == kControlStatus);

    ClientStatus received;
    CHECK(ParseStatus(view.payload, view.payloadLength, received));
    CHECK(received.decodeQueueLength == sent.decodeQueueLength);
    CHECK(received.lossPermille == sent.lossPermille);
    CHECK(received.completedFps == sent.completedFps);
    CHECK(received.droppedFrames == sent.droppedFrames);
    CHECK(received.decodeMsX10 == sent.decodeMsX10);
    CHECK(received.rttMs == sent.rttMs);
    CHECK(!ParseStatus(view.payload, 4, received));  // too short
}

// The software encoder can't be exercised on a machine that has NVENC (the
// host never falls back), so drive it directly: a WARP device stands in for
// the GPU, a synthetic frame for the desktop.
void TestSoftwareEncoder() {
    std::printf("software H.264 encoder (Media Foundation)\n");
    const int srcW = 1920, srcH = 1080;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL obtained;
    // WARP: no GPU needed, which is the situation this encoder exists for.
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
                                   D3D11_SDK_VERSION, &device, &obtained, &context);
    CHECK(SUCCEEDED(hr));
    if (FAILED(hr)) return;

    std::vector<uint8_t> pixels(static_cast<size_t>(srcW) * srcH * 4);
    for (int y = 0; y < srcH; ++y) {
        for (int x = 0; x < srcW; ++x) {
            uint8_t* p = pixels.data() + (static_cast<size_t>(y) * srcW + x) * 4;
            p[0] = static_cast<uint8_t>(x);          // B
            p[1] = static_cast<uint8_t>(y);          // G
            p[2] = static_cast<uint8_t>(x ^ y);      // R
            p[3] = 255;
        }
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = srcW;
    desc.Height = srcH;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = pixels.data();
    init.SysMemPitch = srcW * 4;
    ID3D11Texture2D* frame = nullptr;
    CHECK(SUCCEEDED(device->CreateTexture2D(&desc, &init, &frame)));

    MediaFoundationEncoder encoder(srcW, srcH, 60, 15000);
    const bool ready = encoder.Initialize(device);
    CHECK(ready);
    if (ready) {
        // 1080p in, 720p out - the whole point of the cap.
        CHECK(encoder.OutputWidth() == 1280);
        CHECK(encoder.OutputHeight() == 720);

        encoder.RequestKeyframe();
        std::vector<uint8_t> bitstream;
        bool sawIdrFlag = false;
        // Encoders buffer a few frames before the first output.
        for (int i = 0; i < 30 && bitstream.empty(); ++i) {
            CHECK(encoder.CopyInput(frame));
            bitstream = encoder.Encode();
            if (!bitstream.empty()) sawIdrFlag = encoder.LastWasIdr();
        }
        CHECK(!bitstream.empty());
        if (!bitstream.empty()) {
            // Annex-B: the tablet's reassembler feeds this straight to
            // MediaCodec, so it must start on a start code.
            CHECK(bitstream.size() > 4);
            CHECK(bitstream[0] == 0 && bitstream[1] == 0 &&
                  (bitstream[2] == 1 || (bitstream[2] == 0 && bitstream[3] == 1)));
            // First output must be a keyframe carrying SPS (type 7) - that is
            // what H264Decoder.isKeyframe() and the tablet's keyframe gate
            // look for.
            bool sawSps = false, sawIdr = false;
            for (size_t i = 0; i + 4 < bitstream.size(); ++i) {
                if (bitstream[i] != 0 || bitstream[i + 1] != 0) continue;
                size_t nal = 0;
                if (bitstream[i + 2] == 1) nal = i + 3;
                else if (bitstream[i + 2] == 0 && bitstream[i + 3] == 1) nal = i + 4;
                else continue;
                if (nal >= bitstream.size()) break;
                const int type = bitstream[nal] & 0x1f;
                if (type == 7) sawSps = true;
                if (type == 5) sawIdr = true;
            }
            CHECK(sawSps);
            CHECK(sawIdr);
            CHECK(sawIdrFlag);
        }
    }

    if (frame) frame->Release();
    if (context) context->Release();
    if (device) device->Release();
}

}  // namespace

int main() {
    TestSequenceComparison();
    TestVideoRoundTrip();
    TestVideoRejectsMalformed();
    TestFecRecoversOneLostFragment();
    TestFecSkippedForSmallFrames();
    TestPenRoundTrip();
    TestPenRejectsMalformed();
    TestControlRoundTrip();
    TestStatusRoundTrip();
    TestSoftwareEncoder();

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
