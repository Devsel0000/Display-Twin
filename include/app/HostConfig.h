#pragma once

#define DISPLAY_TWIN_TABLET_IP "192.168.0.19"
#define DISPLAY_TWIN_VIDEO_PORT 5000
#define DISPLAY_TWIN_INPUT_PORT 5001

// String-literal variants for GUI code (Win32's TEXT() macro token-pastes
// an L prefix onto these, so they must stay plain/narrow here - do NOT
// pre-prefix them with L, and do NOT wrap an already-wide literal in TEXT()).
#define DISPLAY_TWIN_VIDEO_PORT_STR "5000"
#define DISPLAY_TWIN_INPUT_PORT_STR "5001"

constexpr int kTargetFramerate = 60;
constexpr int kVideoBitrateKbps = 15000;
