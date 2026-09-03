// File: PenDisplayPC/include/network/NetworkInfo.h
#pragma once

#include <string>
#include <vector>

// One active IPv4 network adapter on this PC.
//
// For a USB-tethered Android device, Windows creates an RNDIS adapter whose
// unicast address is the PC and whose gateway is the tablet itself, so
// `gateway` is exactly the address the host should stream to.
struct AdapterInfo {
    std::string name;         // Friendly name, e.g. "Ethernet 2"
    std::string description;  // e.g. "Remote NDIS based Internet Sharing Device"
    std::string ipv4;         // This PC's address on the adapter
    std::string gateway;      // Gateway address (the tablet, when tethering)
    bool likelyTethering = false;
};

// Active, non-loopback IPv4 adapters. Likely-tethering adapters are listed
// first so callers can just take the front entry as the best guess.
std::vector<AdapterInfo> EnumerateIPv4Adapters();
