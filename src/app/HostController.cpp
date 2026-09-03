#include "app/HostController.h"

#include "app/HostConfig.h"
#include "capture/ScreenCapture.h"
#include "encoder/NvencEncoder.h"
#include "hid/VirtualPen.h"
#include "network/UdpTransport.h"
#include "protocol/Packets.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <thread>
#include <utility>

namespace {
void Log(const HostController::LogCallback& callback, const std::string& message) {
    if (callback) callback(message);
}
}

HostController::~HostController() { Stop(); }

bool HostController::Start(const HostSettings& settings, LogCallback log) {
    if (running_.exchange(true)) return false;
    worker_ = std::thread(&HostController::Run, this, settings, std::move(log));
    return true;
}

void HostController::Stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

void HostController::Run(HostSettings settings, LogCallback log) {
    Log(log, "[PC] Starting host...");
    ScreenCapture capture;
    if (!capture.Initialize()) {
        Log(log, "[ERROR] ScreenCapture initialization failed");
        running_ = false;
        return;
    }

    {
        std::ostringstream caps;
        caps << "[PC] Capturing " << capture.GetWidth() << "x" << capture.GetHeight()
             << " -> encoding at " << settings.framerate << "fps / "
             << settings.bitrateKbps << "kbps";
        Log(log, caps.str());
    }

    NvencEncoder encoder(capture.GetWidth(), capture.GetHeight(), settings.framerate, settings.bitrateKbps);
    if (!encoder.Initialize(capture.GetD3DDevice())) {
        Log(log, "[ERROR] NVENC encoder initialization failed");
        running_ = false;
        return;
    }

    UdpSender videoSender(settings.tabletIp, settings.videoPort);
    if (!videoSender.IsValid()) {
        Log(log, "[ERROR] Video UDP sender initialization failed");
        running_ = false;
        return;
    }

    VirtualPen virtualPen;
    if (!virtualPen.Initialize()) Log(log, "[WARN] VirtualPen initialization failed");

    UdpReceiver inputReceiver(settings.inputPort);
    // The protocol has no session handshake yet, so pin the input channel to
    // whichever address sends the first valid packet to reduce the chance of
    // another device on the network injecting pen input.
    inputReceiver.EnableSourceLock(true);
    // Wi-Fi UDP can deliver packets out of order. Android now also fans out
    // MotionEvent's historical samples as separate packets right before the
    // current one, which makes reordering more likely to actually happen in
    // a burst. Applying a stale MOVE after a newer one (or after UP) makes
    // the injected pointer visibly jitter/snap backward, so drop anything
    // that isn't newer than the last accepted Sequence.
    bool havePenSequence = false;
    uint16_t lastPenSequence = 0;
    inputReceiver.SetCallback([&virtualPen, &havePenSequence, &lastPenSequence](const uint8_t* data, size_t len) {
        PenInputPacket packet;
        if (!ParsePenPacket(data, len, packet) ||
            !std::isfinite(packet.x) || !std::isfinite(packet.y) ||
            !std::isfinite(packet.pressure) || !std::isfinite(packet.tilt_x) ||
            !std::isfinite(packet.tilt_y)) {
            return;
        }
        if (havePenSequence && !IsSequenceNewer(packet.sequence, lastPenSequence)) {
            return; // Stale/out-of-order packet - drop to avoid jitter.
        }
        havePenSequence = true;
        lastPenSequence = packet.sequence;
        virtualPen.InjectInput(packet);
    });
    if (!inputReceiver.Start()) Log(log, "[WARN] Input UDP receiver failed to start");

    std::ostringstream ready;
    ready << "[PC] Streaming to " << settings.tabletIp << ":" << settings.videoPort;
    Log(log, ready.str());

    uint16_t frameId = 0;
    uint32_t frameCounter = 0;
    constexpr uint32_t sessionId = 0;

    // AcquireNextFrame()'s own timeout (see ScreenCapture::CaptureFrame) is
    // the pacer here - it blocks until DXGI actually has a new frame, which
    // tracks the display's real vsync/update cadence. Do NOT also gate this
    // loop with an external sleep_until(): a fixed external clock competing
    // with AcquireNextFrame's wait window causes it to miss frames that
    // arrive just after the external sleep already ate most of the budget,
    // silently dropping ~half the frames instead of a small amount of jitter.
    double captureMsTotal = 0, encodeMsTotal = 0, sendMsTotal = 0;
    double submitMsTotal = 0, readbackMsTotal = 0;
    double bitstreamBytesTotal = 0;
    uint32_t timedFrameCount = 0;
    uint32_t idleCount = 0;   // AcquireNextFrame timeouts: desktop unchanged
    uint32_t skipCount = 0;   // Frames dropped by the framerate cap

    // On a high-refresh monitor, DXGI can hand us frames faster than the
    // target framerate. Encoding/sending those extra frames burns GPU,
    // network, and queueing budget for no visual benefit - budget better
    // spent on latency. Cap by SKIPPING encode+send for frames that arrive
    // too soon after the last one we actually processed (not by sleeping
    // before AcquireNextFrame, which is what caused the missed-frame bug).
    const auto frameInterval = std::chrono::microseconds(1000000 / std::max(1, settings.framerate));
    // Skip only frames that are SUBSTANTIALLY early. A 60Hz desktop delivers
    // frames at almost exactly frameInterval, so comparing against the exact
    // interval makes microsecond-level jitter decide whether a frame is kept;
    // the ones rejected by a hair cost a full extra interval and drag the
    // rate down to ~40fps. The 15% margin keeps every frame at a matching
    // refresh rate while still dropping the genuine extras on a 120/144Hz
    // display.
    const auto skipThreshold = frameInterval * 85 / 100;
    auto lastProcessedTime = std::chrono::steady_clock::now() - frameInterval;

    while (running_) {
        const auto captureStart = std::chrono::steady_clock::now();
        const bool gotFrame = capture.CaptureFrame();
        const auto captureEnd = std::chrono::steady_clock::now();

        if (!gotFrame) ++idleCount;

        if (gotFrame && captureEnd - lastProcessedTime < skipThreshold) {
            ++skipCount;
            capture.ReleaseFrame();
            continue;
        }

        if (gotFrame) {
            lastProcessedTime = captureEnd;

            // Copy into the encoder's own texture and hand the frame straight
            // back to DXGI. Desktop Duplication cannot produce the next frame
            // while we still hold this one, so releasing it before the encode
            // and the send loop (rather than after) takes that dead time off
            // the end-to-end path.
            const bool copied = encoder.CopyInput(capture.GetFrameTexture());
            capture.ReleaseFrame();

            auto encoded = copied ? encoder.Encode() : std::vector<uint8_t>();
            const auto encodeEnd = std::chrono::steady_clock::now();
            if (!encoded.empty()) {
                const auto timestamp = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                auto packets = BuildVideoPackets(sessionId, frameId++, timestamp, encoded);
                bool sent = !packets.empty();
                for (const auto& packet : packets) {
                    if (!videoSender.Send(packet)) { sent = false; break; }
                }
                if (sent) ++frameCounter;
            }
            const auto sendEnd = std::chrono::steady_clock::now();

            captureMsTotal += std::chrono::duration<double, std::milli>(captureEnd - captureStart).count();
            encodeMsTotal += std::chrono::duration<double, std::milli>(encodeEnd - captureEnd).count();
            sendMsTotal += std::chrono::duration<double, std::milli>(sendEnd - encodeEnd).count();
            submitMsTotal += encoder.LastSubmitMs();
            readbackMsTotal += encoder.LastReadbackMs();
            bitstreamBytesTotal += static_cast<double>(encoder.LastBitstreamBytes());
            ++timedFrameCount;
        }

        static auto statsAt = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - statsAt).count() >= 1) {
            std::ostringstream stats;
            stats << "[Stats] Frames: " << frameCounter << " fps";
            if (timedFrameCount > 0) {
                stats << " | avg capture=" << (captureMsTotal / timedFrameCount)
                      << "ms encode=" << (encodeMsTotal / timedFrameCount)
                      << "ms (submit=" << (submitMsTotal / timedFrameCount)
                      << " readback=" << (readbackMsTotal / timedFrameCount)
                      << ") send=" << (sendMsTotal / timedFrameCount) << "ms"
                      << " frame=" << static_cast<int>(bitstreamBytesTotal / timedFrameCount / 1024.0) << "KB";
            }
            // idle = seconds where the desktop simply did not change, so there
            // was nothing to capture. A low fps with a high idle count is not
            // a performance problem.
            stats << " | idle=" << idleCount << " capped=" << skipCount;
            // Pressure actually delivered to Windows (0-1024). A varying
            // min..max here means the tablet -> host path is fine and any
            // remaining problem is in the receiving application.
            uint32_t penCount = 0, penMin = 0, penMax = 0, penLast = 0;
            if (virtualPen.ConsumePressureStats(penCount, penMin, penMax, penLast)) {
                stats << " | pen samples=" << penCount
                      << " pressure " << penMin << ".." << penMax
                      << " (last " << penLast << "/1024)";
            }
            Log(log, stats.str());
            frameCounter = 0;
            captureMsTotal = encodeMsTotal = sendMsTotal = 0;
            submitMsTotal = readbackMsTotal = bitstreamBytesTotal = 0;
            timedFrameCount = 0;
            idleCount = 0;
            skipCount = 0;
            statsAt = std::chrono::steady_clock::now();
        }
    }

    inputReceiver.Stop();
    Log(log, "[PC] Streaming stopped");
    running_ = false;
}
