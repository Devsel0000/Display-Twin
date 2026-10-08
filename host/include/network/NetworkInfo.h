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
    unsigned long ifIndex = 0;       // For DeprioritizeAdapterRoute()/RestoreAdapterMetric()
    unsigned long currentMetric = 0; // Windows' current route metric for this adapter
    bool usesAutomaticMetric = true;
};

// Active, non-loopback IPv4 adapters. Likely-tethering adapters are listed
// first so callers can just take the front entry as the best guess.
std::vector<AdapterInfo> EnumerateIPv4Adapters();

// USB tethering presents the phone as a router, and Windows' automatic
// metric calculation often ranks that fresh link ahead of the PC's real
// internet connection (Wi-Fi/Ethernet) for the DEFAULT route - even though
// the phone usually isn't actually forwarding internet traffic (mobile data
// off, tethering used purely as a private link to run this app over). The
// PC then tries general internet traffic through a link that goes nowhere,
// which looks like "the internet broke" until Windows' route health check
// falls back. Setting a very low-priority (high-number) fixed metric on
// this adapter stops it from ever winning that contest; traffic to the
// adapter's own subnet (the tablet, which is all this app needs) is
// unaffected since metric only decides ties between multiple usable routes.
// Requires admin (the app already runs elevated). Returns false if the
// adapter can't be reached or the change is rejected.
bool DeprioritizeAdapterRoute(unsigned long ifIndex);

// Reverts to Windows' automatic metric calculation for this adapter.
bool RestoreAdapterAutoMetric(unsigned long ifIndex);
