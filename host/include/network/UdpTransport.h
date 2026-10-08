// File: PenDisplayPC/include/network/UdpTransport.h
#pragma once

// Winsock macros must be defined before windows.h/winsock2.h are included.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_   // Prevent windows.h from pulling in the legacy winsock.h
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <atomic>
#include <mutex>

#pragma comment(lib, "ws2_32.lib")

class UdpSender {
public:
    UdpSender(const std::string& ip, int port);
    ~UdpSender();
    bool Send(const std::vector<uint8_t>& data);
    bool Send(const uint8_t* data, size_t len);
    bool IsValid() const;

    // Re-points future sends at a new destination IP (same port), without
    // tearing down the socket. Thread-safe with Send(). Lets a live stream
    // follow the tablet to a new address - e.g. after USB tethering drops
    // and reconnects with a different DHCP-assigned IP - without a full
    // Stop/Start.
    bool SetDestination(const std::string& ip);

private:
    SOCKET sock_ = INVALID_SOCKET;
    mutable std::mutex addrMutex_;
    sockaddr_in addr_;
    int port_ = 0;
    bool winsockStarted_ = false;
};

// A single bound socket that both receives and replies, which is what the
// control channel (UDP 5002) and discovery (UDP 9999) need: every datagram
// has to be answered to whoever sent it, so the callback gets the source
// address and SendTo() can use it directly.
class UdpEndpoint {
public:
    UdpEndpoint(int port, bool enableBroadcast = false);
    ~UdpEndpoint();

    bool IsValid() const { return sock_ != INVALID_SOCKET; }
    bool Start();
    void Stop();
    void SetCallback(std::function<void(const uint8_t*, size_t, const sockaddr_in&)> callback);

    bool SendTo(const sockaddr_in& destination, const uint8_t* data, size_t length);
    bool SendTo(const sockaddr_in& destination, const std::vector<uint8_t>& data);
    bool SendToIp(const std::string& ip, int port, const std::vector<uint8_t>& data);

private:
    static DWORD WINAPI ReceiveThread(LPVOID param);
    void RunLoop();

    SOCKET sock_ = INVALID_SOCKET;
    HANDLE thread_ = nullptr;
    std::atomic_bool running_{ false };
    std::function<void(const uint8_t*, size_t, const sockaddr_in&)> callback_;
    bool winsockStarted_ = false;
};

class UdpReceiver {
public:
    UdpReceiver(int port);
    ~UdpReceiver();
    bool Start();
    void Stop();
    void SetCallback(std::function<void(const uint8_t*, size_t)> callback);

    // When enabled, the receiver remembers the source IP:port of the first
    // datagram it accepts and silently drops any later datagram that comes
    // from a different source. This is a lightweight mitigation against
    // other devices on the same network injecting packets on this port
    // while the protocol still has no session handshake/authentication.
    // Call ResetSourceLock() to allow a new source to bind (e.g. after a
    // client reconnects from a different address).
    void EnableSourceLock(bool enable);
    void ResetSourceLock();
    bool IsSourceLocked() const { return sourceLocked_.load(); }

    // Milliseconds since the last datagram this receiver accepted (i.e. that
    // passed the source lock and reached the callback), or MAXDWORD if none
    // has ever arrived. Backed by GetTickCount64(), safe to poll from
    // another thread - used to detect "the locked source went silent",
    // which is what a mid-session reconnect from a new address looks like.
    DWORD MillisecondsSinceLastPacket() const;

private:
    static DWORD WINAPI ReceiveThread(LPVOID param);
    void RunLoop();
    SOCKET sock_ = INVALID_SOCKET;
    HANDLE thread_ = nullptr;
    std::atomic_bool running_{ false };
    std::function<void(const uint8_t*, size_t)> callback_;
    bool winsockStarted_ = false;

    bool sourceLockEnabled_ = false;
    std::atomic_bool sourceLocked_{ false };
    sockaddr_in lockedSource_{};
    std::atomic<ULONGLONG> lastPacketTick_{ 0 };
};
