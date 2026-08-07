// File: PenDisplayPC/include/network/UdpTransport.h
#pragma once

// ????留ㅽ겕濡쒕뱾? 諛섎뱶??winsock2.h ?꾩뿉 ?????
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_   // windows.h媛 winsock.h瑜??ы븿?섏? ?딅룄濡?李⑤떒
#endif

// ?댁젣 ?덉쟾?섍쾶 winsock2.h ?ы븿
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
private:
    static DWORD WINAPI ReceiveThread(LPVOID param);
    void RunLoop();
    SOCKET sock_ = INVALID_SOCKET;
    HANDLE thread_ = nullptr;
    std::atomic_bool running_{ false };
    std::function<void(const uint8_t*, size_t)> callback_;
    bool winsockStarted_ = false;
};
