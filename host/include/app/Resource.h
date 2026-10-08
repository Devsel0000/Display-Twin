#pragma once

// Win32 resource IDs. The icon must keep the lowest ID of any icon resource -
// Explorer shows the lowest-numbered one as the executable's icon.
#define IDI_APPICON 1

// The UI fonts are linked into the binary as RCDATA so the host ships as a
// single portable .exe with no sidecar font files to lose.
#define IDR_FONT_SANS      101
#define IDR_FONT_MONO      102
#define IDR_FONT_MONO_BOLD 103
