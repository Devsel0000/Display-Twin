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
    int framerate = 60;
    int bitrateKbps = 15000;
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
