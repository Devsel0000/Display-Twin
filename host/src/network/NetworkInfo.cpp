// File: PenDisplayPC/src/network/NetworkInfo.cpp
#include "network/NetworkInfo.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <ipifcons.h>  // IF_TYPE_SOFTWARE_LOOPBACK
#include <netioapi.h>  // GetIpInterfaceEntry / SetIpInterfaceEntry

#include <algorithm>
#include <cstdint>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

std::string ToUtf8(const wchar_t* text) {
    if (!text) return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return std::string();
    std::string out(static_cast<size_t>(needed) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], needed, nullptr, nullptr);
    return out;
}

std::string FormatIPv4(const SOCKET_ADDRESS& address) {
    if (!address.lpSockaddr || address.lpSockaddr->sa_family != AF_INET) {
        return std::string();
    }
    const auto* v4 = reinterpret_cast<const sockaddr_in*>(address.lpSockaddr);
    char buffer[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &v4->sin_addr, buffer, sizeof(buffer))) {
        return std::string();
    }
    return std::string(buffer);
}

bool ContainsNoCase(const std::string& haystack, const char* needle) {
    std::string lower(haystack);
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return lower.find(needle) != std::string::npos;
}

}  // namespace

std::vector<AdapterInfo> EnumerateIPv4Adapters() {
    std::vector<AdapterInfo> adapters;

    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                        GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    ULONG status = GetAdaptersAddresses(AF_INET, flags, nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(&buffer[0]), &size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        status = GetAdaptersAddresses(AF_INET, flags, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(&buffer[0]), &size);
    }
    if (status != NO_ERROR) {
        return adapters;
    }

    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(&buffer[0]);
         adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        AdapterInfo info;
        info.name = ToUtf8(adapter->FriendlyName);
        info.description = ToUtf8(adapter->Description);
        info.ifIndex = adapter->IfIndex;
        info.currentMetric = adapter->Ipv4Metric;

        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            const std::string ip = FormatIPv4(unicast->Address);
            if (!ip.empty()) { info.ipv4 = ip; break; }
        }
        if (info.ipv4.empty()) continue;  // No usable IPv4 on this adapter.

        for (auto* gateway = adapter->FirstGatewayAddress; gateway != nullptr; gateway = gateway->Next) {
            const std::string ip = FormatIPv4(gateway->Address);
            if (!ip.empty() && ip != "0.0.0.0") { info.gateway = ip; break; }
        }

        // Android USB tethering shows up as an RNDIS/"Internet Sharing"
        // device. Match on both the description and the friendly name since
        // OEM drivers word these differently.
        info.likelyTethering =
            ContainsNoCase(info.description, "rndis") ||
            ContainsNoCase(info.description, "internet sharing") ||
            ContainsNoCase(info.description, "usb") ||
            ContainsNoCase(info.name, "rndis") ||
            ContainsNoCase(info.name, "usb");

        MIB_IPINTERFACE_ROW row{};
        row.Family = AF_INET;
        row.InterfaceIndex = info.ifIndex;
        if (GetIpInterfaceEntry(&row) == NO_ERROR) {
            info.usesAutomaticMetric = (row.UseAutomaticMetric != FALSE);
            info.currentMetric = row.Metric;
        }

        adapters.push_back(info);
    }

    // Best candidates first: tethering adapters, then anything with a gateway.
    std::stable_sort(adapters.begin(), adapters.end(),
        [](const AdapterInfo& a, const AdapterInfo& b) {
            if (a.likelyTethering != b.likelyTethering) return a.likelyTethering;
            return !a.gateway.empty() && b.gateway.empty();
        });

    return adapters;
}

bool DeprioritizeAdapterRoute(unsigned long ifIndex) {
    MIB_IPINTERFACE_ROW row{};
    row.Family = AF_INET;
    row.InterfaceIndex = ifIndex;
    if (GetIpInterfaceEntry(&row) != NO_ERROR) return false;
    row.UseAutomaticMetric = FALSE;
    row.Metric = 9999;
    return SetIpInterfaceEntry(&row) == NO_ERROR;
}

bool RestoreAdapterAutoMetric(unsigned long ifIndex) {
    MIB_IPINTERFACE_ROW row{};
    row.Family = AF_INET;
    row.InterfaceIndex = ifIndex;
    if (GetIpInterfaceEntry(&row) != NO_ERROR) return false;
    row.UseAutomaticMetric = TRUE;
    return SetIpInterfaceEntry(&row) == NO_ERROR;
}
