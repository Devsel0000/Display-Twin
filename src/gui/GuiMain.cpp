#include "app/HostController.h"
#include "app/HostConfig.h"
#include "network/NetworkInfo.h"

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <codecvt>
#include <locale>

// Belt-and-suspenders: the project's UACExecutionLevel setting makes the
// linker generate its own manifest, which can drop the comctl32 v6
// side-by-side dependency declared in the .manifest / vcxproj. Embedding it
// here via a linker directive guarantees InitCommonControlsEx() below can
// find the v6 common-controls assembly regardless of how the project's
// manifest settings end up being merged.
#pragma comment(linker, \
    "\"/manifestdependency:type='win32' " \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
    constexpr int IDC_IP = 1001;
    constexpr int IDC_VIDEO_PORT = 1002;
    constexpr int IDC_INPUT_PORT = 1003;
    constexpr int IDC_FPS = 1004;
    constexpr int IDC_BITRATE = 1005;
    constexpr int IDC_START = 1006;
    constexpr int IDC_STOP = 1007;
    constexpr int IDC_LOG = 1008;
    constexpr int IDC_DETECT = 1009;
    constexpr int IDC_BANNER = 1010;
    constexpr UINT WM_HOST_LOG = WM_APP + 1;

    struct AppState {
        HWND window = nullptr;
        HWND ip = nullptr;
        HWND videoPort = nullptr;
        HWND inputPort = nullptr;
        HWND fps = nullptr;
        HWND bitrate = nullptr;
        HWND log = nullptr;
        HWND banner = nullptr;   // Large, bold "type this on the tablet" notice
        HostController controller;
        bool isRunning = false;  // Tracks whether the host loop is active
    };

    std::wstring ReadText(HWND control) {
        const int length = GetWindowTextLengthW(control);
        if (length <= 0) return L"";
        std::wstring value(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(control, &value[0], length + 1);
        value.resize(static_cast<size_t>(length));
        return value;
    }

    int ReadInt(HWND control, int fallback) {
        try {
            return std::stoi(ReadText(control));
        }
        catch (...) {
            return fallback;
        }
    }

    // Safe UTF-8 -> UTF-16 conversion
    std::wstring StringToWString(const std::string& str) {
        if (str.empty()) return L"";
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], static_cast<int>(str.size()), NULL, 0);
        if (size_needed <= 0) return L"";
        std::wstring wstr(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, &str[0], static_cast<int>(str.size()), &wstr[0], size_needed);
        return wstr;
    }

    void AppendLog(AppState* state, const std::string& message) {
        if (!state || !state->log) return;

        std::wstring text = StringToWString(message);
        int length = GetWindowTextLengthW(state->log);
        std::wstring current(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(state->log, &current[0], length + 1);
        current.resize(static_cast<size_t>(length));
        current += text + L"\r\n";
        SetWindowTextW(state->log, current.c_str());

        // Auto-scroll to the bottom
        SendMessageW(state->log, EM_SETSEL, current.size(), current.size());
        SendMessageW(state->log, EM_SCROLLCARET, 0, 0);
    }

    // Lists the PC's active IPv4 adapters in the log and, when a USB
    // tethering adapter is found, fills the Android IP box with its gateway.
    // With USB tethering the tablet acts as the gateway for the RNDIS link,
    // so the gateway address IS the tablet - which saves the user from
    // reading ipconfig and guessing which of the two addresses is which.
    void DetectNetwork(AppState* state, bool autoFill) {
        const std::vector<AdapterInfo> adapters = EnumerateIPv4Adapters();
        if (adapters.empty()) {
            AppendLog(state, "[Net] No active IPv4 adapter found.");
            return;
        }

        const AdapterInfo* best = nullptr;
        for (size_t i = 0; i < adapters.size(); ++i) {
            const AdapterInfo& adapter = adapters[i];
            std::string line = "[Net] ";
            line += adapter.likelyTethering ? "[USB tethering?] " : "[network] ";
            line += adapter.name + "  PC=" + adapter.ipv4;
            if (!adapter.gateway.empty()) {
                line += "  gateway(tablet?)=" + adapter.gateway;
            }
            AppendLog(state, line);
            if (!best && adapter.likelyTethering && !adapter.gateway.empty()) {
                best = &adapters[i];
            }
        }

        if (!best) {
            AppendLog(state, "[Net] No USB tethering adapter detected. Connect the cable, turn on "
                             "USB tethering on the tablet, then press Detect again.");
            if (state->banner) {
                SetWindowTextW(state->banner,
                    L"No USB tethering found - enable it on the tablet, then press Detect");
            }
            return;
        }

        AppendLog(state, "[Net] Tablet address is most likely " + best->gateway +
                         " (via adapter '" + best->name + "').");
        AppendLog(state, "[Net] On the tablet, set PC IP to " + best->ipv4 + ".");
        if (autoFill && state->ip) {
            SetWindowTextW(state->ip, StringToWString(best->gateway).c_str());
            AppendLog(state, "[Net] Android IP field set to " + best->gateway + ".");
        }

        // The one value the user has to type by hand, called out in large
        // bold text so it doesn't get lost in the log.
        if (state->banner) {
            const std::wstring notice = L"On the tablet, set PC IP to  " + StringToWString(best->ipv4);
            SetWindowTextW(state->banner, notice.c_str());
        }
    }

    HWND AddControl(HWND parent, const wchar_t* type, const wchar_t* text,
        DWORD style, int x, int y, int width, int height, int id) {
        return CreateWindowExW(0, type, text, style, x, y, width, height, parent,
            reinterpret_cast<HMENU>(id), GetModuleHandleW(nullptr), nullptr);
    }

    void StartHost(AppState* state) {
        if (!state || state->isRunning) return;

        HostSettings settings;
        const std::wstring ip = ReadText(state->ip);
        settings.tabletIp.assign(ip.begin(), ip.end());
        settings.videoPort = ReadInt(state->videoPort, 5000);
        settings.inputPort = ReadInt(state->inputPort, 5001);
        settings.framerate = ReadInt(state->fps, 60);
        settings.bitrateKbps = ReadInt(state->bitrate, 15000);

        state->isRunning = true;
        state->controller.Start(settings, [state](const std::string& line) {
            if (state && state->window) {
                auto* copy = new std::string(line);
                if (!PostMessageW(state->window, WM_HOST_LOG, 0, reinterpret_cast<LPARAM>(copy))) {
                    delete copy;  // Free if PostMessage failed to queue it
                }
            }
            });
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = static_cast<AppState*>(create->lpCreateParams);
            state->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        if (!state) return DefWindowProcW(hwnd, message, wParam, lParam);

        switch (message) {
        case WM_CTLCOLORSTATIC: {
            // Paint the notice banner in a strong accent color so the address
            // the user has to copy to the tablet is impossible to miss.
            if (state->banner && reinterpret_cast<HWND>(lParam) == state->banner) {
                auto dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, RGB(0, 90, 180));
                SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        case WM_HOST_LOG: {
            auto* line = reinterpret_cast<std::string*>(lParam);
            if (line) {
                AppendLog(state, *line);
                delete line;
            }
            return 0;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == IDC_START && !state->isRunning) {
                // Disable the Start button while the host is running
                EnableWindow(GetDlgItem(hwnd, IDC_START), FALSE);
                StartHost(state);
            }
            if (LOWORD(wParam) == IDC_DETECT) {
                DetectNetwork(state, true);
            }
            if (LOWORD(wParam) == IDC_STOP && state->isRunning) {
                state->controller.Stop();
                state->isRunning = false;
                EnableWindow(GetDlgItem(hwnd, IDC_START), TRUE);
            }
            return 0;
        }
        case WM_CLOSE: {
            if (state->isRunning) {
                state->controller.Stop();
                state->isRunning = false;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    // Must run before any GetSystemMetrics()/window call. Without this, an
    // unaware process gets a DPI-virtualized (scaled-down) screen size from
    // GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN), while DXGI Desktop
    // Duplication still captures at full physical resolution. VirtualPen
    // uses GetSystemMetrics to scale normalized pen coordinates to screen
    // pixels, so that mismatch compresses every injected point into the
    // top-left fraction of the real screen (e.g. only the top ~65% of the
    // screen is reachable at 150% scaling).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Initialize common controls
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES };
    if (!InitCommonControlsEx(&controls)) {
        MessageBoxW(nullptr, L"Failed to initialize common controls", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Register the window class
    const wchar_t* className = L"DisplayTwinHostWindow";
    WNDCLASSW windowClass{};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassW(&windowClass)) {
        MessageBoxW(nullptr, L"Failed to register window class", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    AppState state;
    HWND window = CreateWindowExW(
        0, className, L"Display Twin PC Host",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 620, 460,
        nullptr, nullptr, instance, &state);

    if (!window) {
        MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Create UI controls
    AddControl(window, L"STATIC", L"Android IP", WS_CHILD | WS_VISIBLE, 20, 20, 100, 24, 0);
    state.ip = AddControl(window, L"EDIT", TEXT(DISPLAY_TWIN_TABLET_IP),
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        130, 18, 180, 24, IDC_IP);
    AddControl(window, L"BUTTON", L"Detect tethering", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        320, 17, 140, 26, IDC_DETECT);

    AddControl(window, L"STATIC", L"Video port", WS_CHILD | WS_VISIBLE, 20, 55, 100, 24, 0);
    state.videoPort = AddControl(window, L"EDIT", TEXT(DISPLAY_TWIN_VIDEO_PORT_STR),
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        130, 53, 80, 24, IDC_VIDEO_PORT);

    AddControl(window, L"STATIC", L"Input port", WS_CHILD | WS_VISIBLE, 230, 55, 80, 24, 0);
    state.inputPort = AddControl(window, L"EDIT", TEXT(DISPLAY_TWIN_INPUT_PORT_STR),
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        320, 53, 80, 24, IDC_INPUT_PORT);

    AddControl(window, L"STATIC", L"FPS", WS_CHILD | WS_VISIBLE, 20, 90, 100, 24, 0);
    state.fps = AddControl(window, L"EDIT", L"60",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
        130, 88, 80, 24, IDC_FPS);

    AddControl(window, L"STATIC", L"Bitrate kbps", WS_CHILD | WS_VISIBLE, 230, 90, 90, 24, 0);
    state.bitrate = AddControl(window, L"EDIT", L"15000",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
        320, 88, 80, 24, IDC_BITRATE);

    AddControl(window, L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        20, 130, 100, 30, IDC_START);
    AddControl(window, L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        130, 130, 100, 30, IDC_STOP);

    // The address the user must type into the tablet app, in large bold text
    // so it stands out from the scrolling log below it.
    state.banner = AddControl(window, L"STATIC", L"Press Detect tethering to find the tablet",
        WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
        20, 168, 560, 34, IDC_BANNER);

    state.log = AddControl(window, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
        20, 210, 560, 190, IDC_LOG);

    // Font setup (optional)
    HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (hFont) {
        SendMessageW(state.ip, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        SendMessageW(state.videoPort, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        SendMessageW(state.inputPort, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        SendMessageW(state.fps, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        SendMessageW(state.bitrate, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        SendMessageW(state.log, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        // The font should be released with DeleteObject on shutdown; omitted here
    }

    HFONT bannerFont = CreateFontW(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (bannerFont) {
        SendMessageW(state.banner, WM_SETFONT, reinterpret_cast<WPARAM>(bannerFont), TRUE);
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    // Show the adapter list right away, and pre-fill the tablet address if a
    // USB tethering link is already up. Does nothing to the IP box otherwise.
    DetectNetwork(&state, true);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    // Clean up resources
    if (state.isRunning) {
        state.controller.Stop();
    }

    return static_cast<int>(message.wParam);
}