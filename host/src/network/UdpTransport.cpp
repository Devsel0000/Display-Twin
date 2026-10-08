// File: PenDisplayPC/src/network/UdpTransport.cpp
#include "network/UdpTransport.h"
#include <iostream>
#include <cstring>
#include <limits>

namespace {
bool StartWinsock() {
	WSADATA data{};
	return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

bool SameEndpoint(const sockaddr_in& a, const sockaddr_in& b) {
	return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}
}

// ----- UdpSender -----
UdpSender::UdpSender(const std::string& ip, int port) : port_(port) {
	winsockStarted_ = StartWinsock();
	if (!winsockStarted_) return;
	sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	std::memset(&addr_, 0, sizeof(addr_));
	addr_.sin_family = AF_INET;
	addr_.sin_port = htons(port);
	if (sock_ == INVALID_SOCKET || inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) != 1) {
		if (sock_ != INVALID_SOCKET) closesocket(sock_);
		sock_ = INVALID_SOCKET;
		return;
	}
	// A frame is sent as dozens of back-to-back datagrams. With the default
	// send buffer that burst can stall inside sendto(), which lands directly
	// on the end-to-end latency path.
	int sndbuf = 4 * 1024 * 1024;
	setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuf, sizeof(sndbuf));
}

UdpSender::~UdpSender() {
	if (sock_ != INVALID_SOCKET) closesocket(sock_);
	if (winsockStarted_) WSACleanup();
}

bool UdpSender::IsValid() const { return sock_ != INVALID_SOCKET; }

bool UdpSender::SetDestination(const std::string& ip) {
	sockaddr_in newAddr{};
	newAddr.sin_family = AF_INET;
	newAddr.sin_port = htons(static_cast<u_short>(port_));
	if (inet_pton(AF_INET, ip.c_str(), &newAddr.sin_addr) != 1) return false;
	std::lock_guard<std::mutex> lock(addrMutex_);
	addr_ = newAddr;
	return true;
}

bool UdpSender::Send(const std::vector<uint8_t>& data) {
	return Send(data.data(), data.size());
}

bool UdpSender::Send(const uint8_t* data, size_t len) {
	if (sock_ == INVALID_SOCKET || !data || len == 0 || len > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
	sockaddr_in addrCopy;
	{
		std::lock_guard<std::mutex> lock(addrMutex_);
		addrCopy = addr_;
	}
	int ret = sendto(sock_, (const char*)data, (int)len, 0, (sockaddr*)&addrCopy, sizeof(addrCopy));
	return ret != SOCKET_ERROR;
}

// ----- UdpEndpoint -----
UdpEndpoint::UdpEndpoint(int port, bool enableBroadcast) {
	winsockStarted_ = StartWinsock();
	if (!winsockStarted_) return;
	sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock_ == INVALID_SOCKET) return;
	if (enableBroadcast) {
		BOOL broadcast = TRUE;
		setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast));
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<u_short>(port));
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(sock_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
		closesocket(sock_);
		sock_ = INVALID_SOCKET;
		return;
	}
	int bufSize = 1 * 1024 * 1024;
	setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, (char*)&bufSize, sizeof(bufSize));
	setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, (char*)&bufSize, sizeof(bufSize));
	// Bounded wait so Stop() is noticed promptly on a quiet channel.
	DWORD timeoutMs = 100;
	setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeoutMs, sizeof(timeoutMs));
}

UdpEndpoint::~UdpEndpoint() { Stop(); }

void UdpEndpoint::SetCallback(std::function<void(const uint8_t*, size_t, const sockaddr_in&)> callback) {
	callback_ = std::move(callback);
}

bool UdpEndpoint::Start() {
	if (sock_ == INVALID_SOCKET || running_) return false;
	running_ = true;
	thread_ = CreateThread(nullptr, 0, ReceiveThread, this, 0, nullptr);
	if (!thread_) running_ = false;
	return thread_ != nullptr;
}

void UdpEndpoint::Stop() {
	running_ = false;
	if (sock_ != INVALID_SOCKET) shutdown(sock_, SD_RECEIVE);
	if (thread_) {
		WaitForSingleObject(thread_, INFINITE);
		CloseHandle(thread_);
		thread_ = nullptr;
	}
	if (sock_ != INVALID_SOCKET) {
		closesocket(sock_);
		sock_ = INVALID_SOCKET;
	}
	if (winsockStarted_) {
		WSACleanup();
		winsockStarted_ = false;
	}
}

bool UdpEndpoint::SendTo(const sockaddr_in& destination, const uint8_t* data, size_t length) {
	if (sock_ == INVALID_SOCKET || !data || length == 0 ||
		length > static_cast<size_t>(std::numeric_limits<int>::max())) {
		return false;
	}
	const int sent = sendto(sock_, (const char*)data, (int)length, 0,
		(const sockaddr*)&destination, sizeof(destination));
	return sent != SOCKET_ERROR;
}

bool UdpEndpoint::SendTo(const sockaddr_in& destination, const std::vector<uint8_t>& data) {
	return SendTo(destination, data.data(), data.size());
}

bool UdpEndpoint::SendToIp(const std::string& ip, int port, const std::vector<uint8_t>& data) {
	sockaddr_in destination{};
	destination.sin_family = AF_INET;
	destination.sin_port = htons(static_cast<u_short>(port));
	if (inet_pton(AF_INET, ip.c_str(), &destination.sin_addr) != 1) return false;
	return SendTo(destination, data);
}

DWORD WINAPI UdpEndpoint::ReceiveThread(LPVOID param) {
	((UdpEndpoint*)param)->RunLoop();
	return 0;
}

void UdpEndpoint::RunLoop() {
	uint8_t buffer[2048];
	while (running_) {
		sockaddr_in sender{};
		int senderLen = sizeof(sender);
		const int len = recvfrom(sock_, (char*)buffer, sizeof(buffer), 0,
			(sockaddr*)&sender, &senderLen);
		if (len > 0 && running_ && callback_) {
			callback_(buffer, static_cast<size_t>(len), sender);
		} else if (len == SOCKET_ERROR) {
			const int error = WSAGetLastError();
			if (error != WSAETIMEDOUT && error != WSAEINTR) running_ = false;
		}
	}
}

// ----- UdpReceiver -----
UdpReceiver::UdpReceiver(int port) {
	winsockStarted_ = StartWinsock();
	if (!winsockStarted_) return;
	sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock_ == INVALID_SOCKET) return;
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(sock_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
		closesocket(sock_);
		sock_ = INVALID_SOCKET;
		return;
	}
	// Enlarge the receive buffer to reduce kernel-level packet drops.
	int rcvbuf = 4 * 1024 * 1024;
	setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf));
	DWORD timeoutMs = 100;
	setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeoutMs, sizeof(timeoutMs));
}

UdpReceiver::~UdpReceiver() { Stop(); }

void UdpReceiver::SetCallback(std::function<void(const uint8_t*, size_t)> cb) { callback_ = cb; }

void UdpReceiver::EnableSourceLock(bool enable) {
	sourceLockEnabled_ = enable;
	if (!enable) ResetSourceLock();
}

void UdpReceiver::ResetSourceLock() {
	sourceLocked_ = false;
	std::memset(&lockedSource_, 0, sizeof(lockedSource_));
}

DWORD UdpReceiver::MillisecondsSinceLastPacket() const {
	const ULONGLONG last = lastPacketTick_.load();
	if (last == 0) return MAXDWORD;
	return static_cast<DWORD>(GetTickCount64() - last);
}

bool UdpReceiver::Start() {
	if (sock_ == INVALID_SOCKET || running_) return false;
	running_ = true;
	thread_ = CreateThread(nullptr, 0, ReceiveThread, this, 0, nullptr);
	if (!thread_) running_ = false;
	return thread_ != nullptr;
}

void UdpReceiver::Stop() {
	running_ = false;
	if (sock_ != INVALID_SOCKET) shutdown(sock_, SD_RECEIVE);
	if (thread_) {
		WaitForSingleObject(thread_, INFINITE);
		CloseHandle(thread_);
		thread_ = nullptr;
	}
	if (sock_ != INVALID_SOCKET) {
		closesocket(sock_);
		sock_ = INVALID_SOCKET;
	}
	if (winsockStarted_) {
		WSACleanup();
		winsockStarted_ = false;
	}
}

DWORD WINAPI UdpReceiver::ReceiveThread(LPVOID param) {
	((UdpReceiver*)param)->RunLoop();
	return 0;
}

void UdpReceiver::RunLoop() {
	uint8_t buffer[65536];
	while (running_) {
		sockaddr_in sender{};
		int senderLen = sizeof(sender);
		int len = recvfrom(sock_, (char*)buffer, sizeof(buffer), 0,
			(sockaddr*)&sender, &senderLen);
		if (len > 0 && running_ && callback_) {
			if (sourceLockEnabled_) {
				if (!sourceLocked_) {
					lockedSource_ = sender;
					sourceLocked_ = true;
				} else if (!SameEndpoint(lockedSource_, sender)) {
					continue; // Drop packets from an unrecognized source.
				}
			}
			lastPacketTick_.store(GetTickCount64());
			callback_(buffer, len);
		}
		else if (len == SOCKET_ERROR) {
			const int error = WSAGetLastError();
			if (error != WSAETIMEDOUT && error != WSAEINTR) running_ = false;
		}
	}
}
