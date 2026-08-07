// File: PenDisplayPC/src/main.cpp (단계별 메시지 박스 추가)
#include "network/UdpTransport.h"
#include "capture/ScreenCapture.h"
#include "encoder/NvencEncoder.h"
#include "hid/VirtualPen.h"
#include "app/HostConfig.h"
#include "protocol/Packets.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <cstring>
#include <cmath>
#include <vector>
#include <windows.h>

std::atomic<bool> g_running{ true };

void SignalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_running = false;
    }
}

int main() {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    std::cout << "[PC] Pen Display Host Starting..." << std::endl;
    std::cout.flush();

    ScreenCapture capture;

    if (!capture.Initialize()) {
        std::cerr << "[ERROR] ScreenCapture initialization failed" << std::endl;
        return -1;
    }
    std::cout << "[OK] ScreenCapture initialized." << std::endl;

    if (capture.GetWidth() <= 0 || capture.GetHeight() <= 0) {
        std::cerr << "[ERROR] Invalid captured output dimensions" << std::endl;
        return -1;
    }
    NvencEncoder encoder(capture.GetWidth(), capture.GetHeight(), kTargetFramerate, kVideoBitrateKbps);

    if (!encoder.Initialize(capture.GetD3DDevice())) {
        std::cerr << "[ERROR] NVENC encoder initialization failed" << std::endl;
        return -1;
    }
    std::cout << "[OK] NVENC encoder initialized." << std::endl;

    UdpSender videoSender(DISPLAY_TWIN_TABLET_IP, DISPLAY_TWIN_VIDEO_PORT);
    if (!videoSender.IsValid()) {
        std::cerr << "[ERROR] Video UDP sender initialization failed" << std::endl;
        return -1;
    }
    std::cout << "[OK] UdpSender initialized." << std::endl;

    VirtualPen virtualPen;
    if (!virtualPen.Initialize()) {
        std::cerr << "[WARN] VirtualPen initialization failed" << std::endl;
    }
    else {
        std::cout << "[OK] VirtualPen initialized." << std::endl;
    }

    UdpReceiver inputReceiver(DISPLAY_TWIN_INPUT_PORT);
    inputReceiver.SetCallback([&](const uint8_t* data, size_t len) {
        if (len >= sizeof(PenInputPacket)) {
            PenInputPacket packet;
            memcpy(&packet, data, sizeof(packet));
            if (packet.magic == kPenInputPacketMagic &&
                std::isfinite(packet.x) && std::isfinite(packet.y) &&
                std::isfinite(packet.pressure) && std::isfinite(packet.tilt_x) &&
                std::isfinite(packet.tilt_y)) {
                virtualPen.InjectInput(packet);
            }
        }
        });
    if (inputReceiver.Start()) {
        std::cout << "[OK] UdpReceiver started." << std::endl;
    } else {
        std::cerr << "[WARN] Input UDP receiver failed to start" << std::endl;
    }

    std::cout << "[PC] Ready. Streaming to " << DISPLAY_TWIN_TABLET_IP << ":" << DISPLAY_TWIN_VIDEO_PORT << std::endl;
    std::cout << "Press Ctrl+C to stop." << std::endl;

    const auto frameInterval = std::chrono::microseconds(1000000 / kTargetFramerate);
    auto nextFrameTime = std::chrono::steady_clock::now();
    uint32_t frameIndex = 0;
    uint32_t frameCounter = 0;

    while (g_running) {
        auto now = std::chrono::steady_clock::now();
        if (now < nextFrameTime) {
            std::this_thread::sleep_until(nextFrameTime);
        }
        nextFrameTime = std::chrono::steady_clock::now() + frameInterval;

        if (capture.CaptureFrame()) {
            auto texture = capture.GetFrameTexture();
            auto encodedData = encoder.EncodeFrame(texture);

            if (!encodedData.empty()) {
                uint32_t dataSize = static_cast<uint32_t>(encodedData.size());
                std::vector<uint8_t> sendBuffer(sizeof(VideoPacketHeader) + encodedData.size());
                auto* packet = reinterpret_cast<VideoPacketHeader*>(sendBuffer.data());
                packet->magic = kVideoPacketMagic;
                packet->frameIndex = frameIndex++;
                packet->dataSize = dataSize;
                memcpy(sendBuffer.data() + sizeof(VideoPacketHeader), encodedData.data(), encodedData.size());

                if (sendBuffer.size() <= kMaxUdpPayload && videoSender.Send(sendBuffer)) {
                    frameCounter++;
                }
                else if (sendBuffer.size() > kMaxUdpPayload) {
                    static bool oversizedWarningShown = false;
                    if (!oversizedWarningShown) {
                        std::cerr << "[WARN] Encoded frame exceeds UDP payload limit: " << sendBuffer.size() << std::endl;
                        oversizedWarningShown = true;
                    }
                }
            }
            capture.ReleaseFrame();
        }

        static auto lastStatsTime = std::chrono::steady_clock::now();
        auto statsElapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - lastStatsTime).count();
        if (statsElapsed >= 1) {
            std::cout << "[Stats] Frames: " << frameCounter << " fps" << std::endl;
            frameCounter = 0;
            lastStatsTime = std::chrono::steady_clock::now();
        }
    }

    inputReceiver.Stop();
    std::cout << "[PC] Clean shutdown complete." << std::endl;
    return 0;
}
