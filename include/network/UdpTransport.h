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

#pragma comment(lib, "ws2_32.lib")

class UdpSender {
public:
    UdpSender(const std::string& ip, int port);
    ~UdpSender();
    bool Send(const std::vector<uint8_t>& data);
    bool Send(const uint8_t* data, size_t len);
    bool IsValid() const;
private:
    SOCKET sock_ = INVALID_SOCKET;
    sockaddr_in addr_;
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
};
