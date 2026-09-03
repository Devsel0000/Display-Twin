#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

struct HostSettings {
    std::string tabletIp = "192.168.42.129";
    int videoPort = 5000;
    int inputPort = 5001;
    int framerate = 60;
    int bitrateKbps = 15000;
};

class HostController {
public:
    using LogCallback = std::function<void(const std::string&)>;

    ~HostController();
    bool Start(const HostSettings& settings, LogCallback log);
    void Stop();
    bool IsRunning() const { return running_.load(); }

private:
    void Run(HostSettings settings, LogCallback log);
    std::atomic_bool running_{false};
    std::thread worker_;
};
