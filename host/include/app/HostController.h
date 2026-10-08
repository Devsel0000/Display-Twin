#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct HostSettings {
    std::string tabletIp = "192.168.42.129";
    int videoPort = 5000;
    int inputPort = 5001;
    int controlPort = 5002;
    int discoveryPort = 9999;
    int framerate = 60;
    int bitrateKbps = 15000;
    // XOR parity fragments cost ~12.5% of the video bandwidth on large
    // frames and buy back single-fragment losses without a round trip.
    bool enableFec = true;
    // QoS may lower the bitrate and drop to 30fps on its own when the
    // tablet reports sustained loss or a backed-up decoder.
    bool enableQos = true;
    // Session log file under logs/ next to the executable.
    bool writeSessionLog = true;
};

// Snapshot of the per-second stats HostController::Run already computes for
// its log line, kept as atomics so the GUI can read real numbers for its
// stat tiles every frame without parsing log text or touching the worker
// thread's locals directly.
struct HostStats {
    std::atomic<uint32_t> fps{0};
    std::atomic<float> captureMs{0};
    std::atomic<float> encodeMs{0};
    std::atomic<float> sendMs{0};
    std::atomic<uint32_t> frameKB{0};
    std::atomic<uint32_t> idleCount{0};
    std::atomic<uint32_t> cappedCount{0};
    std::atomic<bool> hasPenData{false};
    std::atomic<uint32_t> penSamples{0};
    std::atomic<uint32_t> penMin{0};
    std::atomic<uint32_t> penMax{0};
    std::atomic<uint32_t> penLast{0};

    // Session / control channel.
    std::atomic<bool> sessionActive{false};
    std::atomic<uint32_t> sessionId{0};
    std::atomic<uint32_t> rttMs{0};

    // Reported by the tablet once a second (STATUS), so these are the only
    // numbers here that describe the far end of the link.
    std::atomic<uint32_t> lossPermille{0};
    std::atomic<uint32_t> decodeQueue{0};
    std::atomic<uint32_t> clientFps{0};
    std::atomic<float> clientDecodeMs{0};

    // Recovery / QoS.
    std::atomic<uint32_t> pliCount{0};       // PLIs received since start
    std::atomic<uint32_t> idrCount{0};       // keyframes emitted last second
    std::atomic<uint32_t> idrKB{0};          // size of the most recent keyframe
    std::atomic<uint32_t> qosFps{0};         // framerate QoS currently targets
    std::atomic<uint32_t> qosBitrateKbps{0}; // bitrate QoS currently targets
    std::atomic<uint32_t> qosDowngrades{0};
};

class HostController {
public:
    using LogCallback = std::function<void(const std::string&)>;

    ~HostController();
    bool Start(const HostSettings& settings, LogCallback log);
    void Stop();
    bool IsRunning() const { return running_.load(); }

    // Re-points the running video stream at a new tablet address and forces
    // an immediate pen-session reset (source lock + stuck tip release),
    // without a full Stop/Start. Meant to be called after "Detect
    // tethering" finds the tablet at a different address than the one the
    // stream was started with - e.g. USB tethering dropped and reconnected
    // with a new DHCP-assigned IP. No-op if the host isn't running.
    void UpdateTabletIp(const std::string& ip);

    // Reads the latest per-second stats snapshot. Safe to call every UI
    // frame from any thread; stale (all-zero) before the first stats tick.
    const HostStats& GetStats() const { return stats_; }

private:
    void Run(HostSettings settings, LogCallback log);
    std::atomic_bool running_{false};
    std::thread worker_;

    std::mutex pendingMutex_;
    std::string pendingTabletIp_;
    bool hasPendingTabletIp_ = false;
    bool pendingPenReset_ = false;

    HostStats stats_;
};
