#include "app/HostController.h"
#include "app/HostConfig.h"
#include "app/Resource.h"
#include "network/NetworkInfo.h"

#include "imgui.h"
#include "qrcodegen.hpp"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <windows.h>

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// imgui_impl_win32.h intentionally omits this declaration (wrapped in #if 0)
// to avoid dragging <windows.h> into the header; it asks callers to copy it
// in directly.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

// ----- D3D11 device/swapchain bound to the main window -----
// Separate from ScreenCapture's D3D11 device (that one exists only while
// streaming, on the worker thread, bound to the desktop duplication
// adapter). This one exists for the lifetime of the GUI window and is what
// Dear ImGui renders into.
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_renderTargetView = nullptr;

void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTargetView);
        backBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_renderTargetView) { g_renderTargetView->Release(); g_renderTargetView = nullptr; }
}

bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL obtained;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &obtained, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &obtained, &g_context);
    }
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;

    switch (msg) {
    case WM_SIZE:
        if (g_device && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // Disable ALT opening the (nonexistent) window menu.
        break;
    case WM_GETMINMAXINFO: {
        // Below this the connection card and controls no longer fit, and
        // the log - the one area meant to flex - would be squeezed out.
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 480;
        info->ptMinTrackSize.y = 600;
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ----- Theme -----
// One blue family for both themes, built on #E3F2FD / #90CAF9 / #2196F3 /
// #0D47A1. Neutrals (page, card, borders, muted text) are tints and shades
// of those four, so light and dark read as the same product.
struct Theme {
    ImVec4 pageBg, cardBg, cardBorder, frameBg, frameBgHover;
    ImVec4 text, textMuted, accent, accentText;
    ImVec4 bannerBg, bannerBorder, bannerLabel, bannerValue;
};

ImVec4 Rgb(int r, int g, int b, float a = 1.0f) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

Theme MakeTheme(bool dark) {
    if (dark) {
        Theme t;
        t.pageBg = Rgb(8, 17, 29);           // #0D47A1 taken almost to black
        t.cardBg = Rgb(14, 27, 44);
        t.cardBorder = Rgb(26, 47, 74);
        t.frameBg = Rgb(11, 22, 36);
        t.frameBgHover = Rgb(19, 36, 58);
        t.text = Rgb(227, 242, 253);         // #E3F2FD
        t.textMuted = Rgb(143, 170, 198);
        t.accent = Rgb(144, 202, 249);       // #90CAF9 - the light blue carries on a dark ground
        t.accentText = Rgb(13, 71, 161);     // #0D47A1 on #90CAF9, ~5.5:1
        t.bannerBg = Rgb(13, 71, 161);       // #0D47A1
        t.bannerBorder = Rgb(25, 98, 196);
        t.bannerLabel = Rgb(144, 202, 249);  // #90CAF9
        t.bannerValue = Rgb(227, 242, 253);  // #E3F2FD
        return t;
    }
    Theme t;
    t.pageBg = Rgb(244, 249, 254);           // #E3F2FD lifted toward white
    t.cardBg = Rgb(255, 255, 255);
    t.cardBorder = Rgb(214, 233, 251);
    t.frameBg = Rgb(244, 249, 254);
    t.frameBgHover = Rgb(227, 242, 253);     // #E3F2FD
    t.text = Rgb(11, 31, 58);                // #0D47A1 taken toward black
    t.textMuted = Rgb(91, 120, 152);
    t.accent = Rgb(33, 150, 243);            // #2196F3
    t.accentText = Rgb(255, 255, 255);
    t.bannerBg = Rgb(227, 242, 253);         // #E3F2FD
    t.bannerBorder = Rgb(144, 202, 249);     // #90CAF9
    t.bannerLabel = Rgb(13, 71, 161);        // #0D47A1
    t.bannerValue = Rgb(13, 71, 161);        // #0D47A1
    return t;
}

ImVec4 Scale(ImVec4 c, float f) {
    return ImVec4(c.x * f, c.y * f, c.z * f, c.w);
}

// Applies the theme to Dear ImGui's style and the DWM title bar, so the
// native window chrome doesn't clash with a dark client area.
void ApplyTheme(const Theme& t, HWND hwnd, bool dark) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;

    c[ImGuiCol_WindowBg] = t.pageBg;
    c[ImGuiCol_ChildBg] = t.cardBg;
    c[ImGuiCol_Border] = t.cardBorder;
    c[ImGuiCol_Text] = t.text;
    c[ImGuiCol_TextDisabled] = t.textMuted;
    c[ImGuiCol_FrameBg] = t.frameBg;
    c[ImGuiCol_FrameBgHovered] = t.frameBgHover;
    c[ImGuiCol_FrameBgActive] = t.frameBgHover;
    c[ImGuiCol_Button] = t.accent;
    c[ImGuiCol_ButtonHovered] = Scale(t.accent, 1.08f);
    c[ImGuiCol_ButtonActive] = Scale(t.accent, 0.92f);
    c[ImGuiCol_Header] = t.accent;
    c[ImGuiCol_HeaderHovered] = Scale(t.accent, 1.08f);
    c[ImGuiCol_HeaderActive] = Scale(t.accent, 0.92f);
    c[ImGuiCol_CheckMark] = t.accent;
    c[ImGuiCol_SliderGrab] = t.accent;
    c[ImGuiCol_SliderGrabActive] = Scale(t.accent, 0.92f);
    c[ImGuiCol_Separator] = t.cardBorder;
    c[ImGuiCol_SeparatorHovered] = t.accent;
    c[ImGuiCol_ScrollbarBg] = t.pageBg;

    style.WindowPadding = ImVec2(12, 8);
    style.FramePadding = ImVec2(7, 3);
    style.ItemSpacing = ImVec2(8, 5);
    style.ItemInnerSpacing = ImVec2(5, 4);
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.ChildBorderSize = 1.0f;
    style.WindowRounding = 0.0f;  // Fills the whole client area - no floating-window corners.
    style.ScrollbarSize = 12.0f;

    BOOL darkAttr = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkAttr, sizeof(darkAttr));
}

// ----- App state -----

struct AppState {
    HostController controller;
    bool isRunning = false;
    bool darkMode = false;
    Theme theme = MakeTheme(false);
    // Narrow window: the step-by-step layout. Once the user drags the
    // window wide enough, the two-pane layout (settings left, diagnostics and
    // log right). See kWideEnter / kWideExit.
    bool wideLayout = false;

    char androidIp[64] = "";
    int videoPort = DISPLAY_TWIN_VIDEO_PORT;
    int inputPort = DISPLAY_TWIN_INPUT_PORT;
    int fps = kTargetFramerate;
    int bitrateKbps = kVideoBitrateKbps;

    std::string bannerLabel;   // Small caption line; empty when bannerValue is a single free-form message.
    std::string bannerValue;

    unsigned long tetherIfIndex = 0;
    bool tetherHasAdapter = false;
    bool tetherAutoMetric = true;

    // PC addresses shown as QR codes for the tablet to scan (see DrawQrModal).
    std::string qrUsbIp, qrWifiIp;

    std::mutex logMutex;
    std::vector<std::string> logLines;

    // Matches the fonts used in the Display Twin host-UI mockup
    // (display-twin-host-concepts.html): Manrope for everything, JetBrains
    // Mono for anything that's actually a value (IP, ports, numbers, log).
    ImFont* sansFont = nullptr;      // Manrope - the default/body font, also used (bigger) for the title.
    ImFont* monoFont = nullptr;      // JetBrains Mono Regular - IP/port/target field values, log text.
    ImFont* monoBoldFont = nullptr;  // JetBrains Mono Bold - banner value, stat tile numbers.
};

void CopyToBuffer(char* dst, size_t dstSize, const std::string& src) {
    const size_t n = (src.size() < dstSize - 1) ? src.size() : dstSize - 1;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

// Points at an RCDATA blob linked into the executable. Resource memory is
// owned by the loaded image and stays mapped for the life of the process,
// which is exactly what ImGui's atlas needs (since 1.92 the TTF bytes must
// outlive the atlas), so it is handed over with FontDataOwnedByAtlas=false.
bool FindResourceBytes(int id, void** data, int* size) {
    const HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!found) {
        return false;
    }
    const HGLOBAL loaded = LoadResource(nullptr, found);
    if (!loaded) {
        return false;
    }
    void* bytes = LockResource(loaded);
    const DWORD len = SizeofResource(nullptr, found);
    if (!bytes || len == 0) {
        return false;
    }
    *data = bytes;
    *size = static_cast<int>(len);
    return true;
}

// Adds one of the embedded UI fonts to the atlas. Returns nullptr if the
// resource is missing so the caller can fall back to a system font.
ImFont* AddEmbeddedFont(ImGuiIO& io, int resourceId, float sizePixels) {
    void* data = nullptr;
    int size = 0;
    if (!FindResourceBytes(resourceId, &data, &size)) {
        return nullptr;
    }
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // Resource memory - the atlas must not free it.
    return io.Fonts->AddFontFromMemoryTTF(data, size, sizePixels, &cfg);
}

void AppendLog(AppState* state, const std::string& line) {
    std::lock_guard<std::mutex> lock(state->logMutex);
    state->logLines.push_back(line);
    // Keep the panel from growing without bound over a long session.
    if (state->logLines.size() > 1000) {
        state->logLines.erase(state->logLines.begin(), state->logLines.begin() + (state->logLines.size() - 1000));
    }
}

// Lists the PC's active IPv4 adapters in the log and, when a USB tethering
// adapter is found, fills the Android IP field with its gateway. With USB
// tethering the tablet acts as the gateway for the RNDIS link, so the
// gateway address IS the tablet - which saves the user from reading
// ipconfig and guessing which of the two addresses is which.
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
        line += (adapter.name.empty() ? std::string("(unnamed adapter)") : adapter.name) + "  PC=" + adapter.ipv4;
        if (!adapter.gateway.empty()) {
            line += "  gateway(tablet?)=" + adapter.gateway;
        }
        line += adapter.usesAutomaticMetric
            ? "  metric=auto(" + std::to_string(adapter.currentMetric) + ")"
            : "  metric=fixed(" + std::to_string(adapter.currentMetric) + ")";
        AppendLog(state, line);
        if (!best && adapter.likelyTethering && !adapter.gateway.empty()) {
            best = &adapters[i];
        }
    }

    if (!best) {
        AppendLog(state, "[Net] No USB tethering adapter detected. Connect the cable, turn on "
                         "USB tethering on the tablet, then press Detect again.");
        state->bannerLabel.clear();
        state->bannerValue = "No USB tethering found - enable it on the tablet, then press Detect";
        state->tetherHasAdapter = false;
        return;
    }

    AppendLog(state, "[Net] Tablet address is most likely " + best->gateway +
                     " (via adapter '" + best->name + "').");
    AppendLog(state, "[Net] On the tablet, set PC IP to " + best->ipv4 + ".");
    if (autoFill) {
        CopyToBuffer(state->androidIp, sizeof(state->androidIp), best->gateway);
        AppendLog(state, "[Net] Android IP field set to " + best->gateway + ".");
    }

    // If a stream is already running (e.g. the user is pressing Detect again
    // after a tethering reconnect), follow the tablet to its new address
    // live instead of requiring a Stop/Start.
    if (state->isRunning) {
        state->controller.UpdateTabletIp(best->gateway);
    }

    state->tetherIfIndex = best->ifIndex;
    state->tetherHasAdapter = true;
    state->tetherAutoMetric = best->usesAutomaticMetric;

    // The one value the user has to type by hand, called out big in the UI.
    state->bannerLabel = "ON THE TABLET, SET PC IP TO";
    state->bannerValue = best->ipv4;
}

void FixNetworkPriority(AppState* state) {
    if (!state->tetherHasAdapter) return;
    const bool goingAuto = !state->tetherAutoMetric;
    const bool ok = goingAuto
        ? RestoreAdapterAutoMetric(state->tetherIfIndex)
        : DeprioritizeAdapterRoute(state->tetherIfIndex);
    if (ok) {
        state->tetherAutoMetric = goingAuto;
        AppendLog(state, goingAuto
            ? "[Net] Restored automatic route metric on the tethering adapter."
            : "[Net] Tethering adapter deprioritized (metric=9999) so it won't compete "
              "with your real internet connection for the default route.");
    } else {
        AppendLog(state, "[ERROR] Failed to change the adapter's route metric.");
    }
}

void StartHost(AppState* state) {
    if (state->isRunning) return;

    HostSettings settings;
    settings.tabletIp = state->androidIp;
    settings.videoPort = state->videoPort;
    settings.inputPort = state->inputPort;
    settings.framerate = state->fps;
    settings.bitrateKbps = state->bitrateKbps;

    state->isRunning = true;
    state->controller.Start(settings, [state](const std::string& line) {
        AppendLog(state, line);  // Thread-safe - HostController invokes this on its worker thread.
    });
}

void StopHost(AppState* state) {
    if (!state->isRunning) return;
    state->controller.Stop();
    state->isRunning = false;
}

ImVec4 LogLineColor(const AppState* state, const std::string& line) {
    if (line.rfind("[ERROR]", 0) == 0) return ImVec4(0.85f, 0.30f, 0.30f, 1.0f);
    if (line.rfind("[WARN]", 0) == 0) return ImVec4(0.85f, 0.65f, 0.20f, 1.0f);
    if (line.rfind("[Stats]", 0) == 0) return state->theme.textMuted;
    if (line.rfind("[Net]", 0) == 0) return ImVec4(0.35f, 0.62f, 0.80f, 1.0f);
    if (line.rfind("[Pen]", 0) == 0) return ImVec4(0.45f, 0.72f, 0.48f, 1.0f);
    if (line.rfind("[Session]", 0) == 0) return ImVec4(0.35f, 0.72f, 0.75f, 1.0f);
    if (line.rfind("[Discovery]", 0) == 0) return ImVec4(0.35f, 0.62f, 0.80f, 1.0f);
    if (line.rfind("[QoS]", 0) == 0) return ImVec4(0.85f, 0.65f, 0.20f, 1.0f);
    return state->theme.text;
}

// Value font for the Connection card's Android IP / Ports / Target rows -
// a size bump over body text, but not as heavy/large as monoBoldFont (which
// is bold and meant for the banner/stat tiles).
constexpr float kValueFontSize = 18.0f;

// The rest of the type scale, in one place. Body text is what buttons,
// labels and step titles use; everything else is sized relative to it.
constexpr float kBodyFontSize = 17.0f;
constexpr float kTitleFontSize = 26.0f;        // "Display Twin", top-left
constexpr float kCaptionFontSize = 15.0f;      // banner caption, detection message
constexpr float kBannerValueFontSize = 22.0f;  // the PC IP to type on the tablet
constexpr float kMetricLabelFontSize = 14.0f;  // diagnostics labels above their values
constexpr float kLogFontSize = 16.0f;          // dense and scrolled, so a notch under body
constexpr float kBadgeFontSize = 13.0f;        // step number inside its circle
constexpr float kBadgeDiameter = 22.0f;
constexpr float kStartButtonHeight = 34.0f;

// Width just big enough for an EDITABLE field's current text at
// kValueFontSize plus room for the frame padding and caret, so a
// right-positioned InputText/InputInt sits flush against the row's right
// edge instead of floating in a wider fixed box. Measures in the mono font
// since that's what these fields actually render in - measuring in the
// wrong font is exactly what threw Ports/Target out of alignment with the
// Android IP row before.
float DynFieldWidth(const AppState* state, const char* text, float minWidth) {
    ImGui::PushFont(state->monoFont, kValueFontSize);
    const float w = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2.0f + 4.0f;
    ImGui::PopFont();
    return w < minWidth ? minWidth : w;
}

// Width of a plain (non-editable) separator/unit label in a value row, e.g.
// "/" or "kbps" - measured for real instead of guessed, and also used to add
// a matching trailing inset to rows that end in plain text rather than an
// input field (an InputText/InputInt's own FramePadding otherwise makes
// input-ending rows sit slightly left of text-ending rows at the same
// nominal width).
float DynTextWidth(const AppState* state, const char* text) {
    ImGui::PushFont(state->monoFont, kValueFontSize);
    const float w = ImGui::CalcTextSize(text).x;
    ImGui::PopFont();
    return w;
}

// The narrow window shows the step-by-step layout; dragging it wider than
// kWideEnter switches to the two-pane layout, and it only switches back
// below kWideExit. The gap between the two is hysteresis: a window resting
// right on a single threshold would flip layouts every time a resize nudged
// it by a pixel.
constexpr float kWideEnter = 820.0f;
constexpr float kWideExit = 780.0f;

const ImVec4 kStatusStreaming(0.30f, 0.72f, 0.42f, 1.0f);
const ImVec4 kStatusWaiting(0.82f, 0.62f, 0.22f, 1.0f);

// Accent-filled button with the theme's on-accent text colour. ImGui draws
// button labels in ImGuiCol_Text by default, which on the dark theme's light
// accent left light-on-light labels.
bool AccentButton(const AppState* state, const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Text, state->theme.accentText);
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor();
    return pressed;
}

void ToggleTheme(AppState* state, HWND hwnd) {
    state->darkMode = !state->darkMode;
    state->theme = MakeTheme(state->darkMode);
    ApplyTheme(state->theme, hwnd, state->darkMode);
}

// Right-aligned theme toggle on the current line. rowStartX/rowWidth are the
// line's start and usable width, captured before anything was drawn on it.
void ThemeToggleAtRight(AppState* state, HWND hwnd, float rowStartX, float rowWidth) {
    const char* label = state->darkMode ? "Light" : "Dark";
    const float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(rowStartX + rowWidth - w);
    if (AccentButton(state, label)) ToggleTheme(state, hwnd);
}

void DrawStatus(AppState* state) {
    if (state->isRunning) {
        const HostStats& stats = state->controller.GetStats();
        ImGui::TextColored(kStatusStreaming, "\xe2\x97\x8f Streaming \xc2\xb7 %u fps", stats.fps.load());
    } else {
        ImGui::TextColored(kStatusWaiting, "\xe2\x97\x8b Waiting");
    }
}

void DrawTitle(AppState* state) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(state->sansFont, kTitleFontSize);  // Manrope, not mono - this is a title, not a value.
    ImGui::TextUnformatted("Display Twin");
    ImGui::PopFont();
}

// Android IP / Ports / Target as flat label-value rows. Still real, editable
// ImGui widgets underneath - FrameBg is pushed transparent so they read as
// plain text until clicked, and each is right-aligned by pre-computing its
// width and moving the cursor. Works inside a card or inside a step group:
// positions are taken relative to where the rows start.
void DrawConnectionRows(AppState* state, bool separators) {
    const Theme& t = state->theme;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));

    const float rowRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    // A plain Text element has no FramePadding around it, but an
    // InputText/InputInt does - so a row ending in plain text (Target's
    // "kbps") needs this much extra reserved width to visually match the
    // trailing inset that rows ending in an input field get for free.
    const float trailingTextInset = ImGui::GetStyle().FramePadding.x;

    // Row: Android IP. InputText has no right-text-alignment, so the field is
    // sized to its own text and placed against the right edge.
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(t.textMuted, "Android IP");
    ImGui::SameLine();
    {
        const float w = DynFieldWidth(state, state->androidIp, 90.0f);
        ImGui::SetCursorPosX(rowRight - w);
        ImGui::PushFont(state->monoFont, kValueFontSize);
        ImGui::SetNextItemWidth(w);
        ImGui::InputText("##ip", state->androidIp, sizeof(state->androidIp));
        ImGui::PopFont();
    }
    if (separators) ImGui::Separator();

    // Row: Ports (video / input).
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(t.textMuted, "Ports");
    ImGui::SameLine();
    {
        char vbuf[16], ibuf[16];
        std::snprintf(vbuf, sizeof(vbuf), "%d", state->videoPort);
        std::snprintf(ibuf, sizeof(ibuf), "%d", state->inputPort);
        const float wv = DynFieldWidth(state, vbuf, 44.0f);
        const float wi = DynFieldWidth(state, ibuf, 44.0f);
        const float sepW = DynTextWidth(state, "/");
        ImGui::SetCursorPosX(rowRight - (wv + 4 + sepW + 4 + wi));
        ImGui::PushFont(state->monoFont, kValueFontSize);
        ImGui::SetNextItemWidth(wv);
        ImGui::InputInt("##videoport", &state->videoPort, 0);
        ImGui::SameLine(0, 4);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("/");
        ImGui::SameLine(0, 4);
        ImGui::SetNextItemWidth(wi);
        ImGui::InputInt("##inputport", &state->inputPort, 0);
        ImGui::PopFont();
    }
    if (separators) ImGui::Separator();

    // Row: Target (fps + bitrate). Ends in plain text, hence the inset.
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(t.textMuted, "Target");
    ImGui::SameLine();
    {
        char fbuf[16], bbuf[16];
        std::snprintf(fbuf, sizeof(fbuf), "%d", state->fps);
        std::snprintf(bbuf, sizeof(bbuf), "%d", state->bitrateKbps);
        const float wf = DynFieldWidth(state, fbuf, 40.0f);
        const float wb = DynFieldWidth(state, bbuf, 56.0f);
        const float unit1W = DynTextWidth(state, "fps \xc2\xb7");
        const float unit2W = DynTextWidth(state, "kbps");
        ImGui::SetCursorPosX(rowRight - (wf + 4 + unit1W + 4 + wb + 4 + unit2W + trailingTextInset));
        ImGui::PushFont(state->monoFont, kValueFontSize);
        ImGui::SetNextItemWidth(wf);
        ImGui::InputInt("##fps", &state->fps, 0);
        ImGui::SameLine(0, 4);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("fps \xc2\xb7");  // middle dot (U+00B7), raw UTF-8 bytes only - no literal non-ASCII in this file, see the CP949-misparse incident in git history
        ImGui::SameLine(0, 4);
        ImGui::SetNextItemWidth(wb);
        ImGui::InputInt("##bitrate", &state->bitrateKbps, 0);
        ImGui::SameLine(0, 4);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("kbps");
        ImGui::PopFont();
    }

    ImGui::PopStyleColor(3);
}

// Black-on-white QR (always light, whatever the theme - scanners need the
// contrast) with the 4-module quiet zone the spec asks for. Empty ip draws a
// placeholder instead.
void DrawQr(const std::string& ip, const char* mode, float size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), IM_COL32(255, 255, 255, 255));
    if (!ip.empty()) {
        const auto qr = qrcodegen::QrCode::encodeText(
            ("dtwin://" + ip + "?mode=" + mode).c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const float cell = size / (qr.getSize() + 8);
        for (int y = 0; y < qr.getSize(); ++y) {
            for (int x = 0; x < qr.getSize(); ++x) {
                if (!qr.getModule(x, y)) continue;
                const ImVec2 a(p.x + (x + 4) * cell, p.y + (y + 4) * cell);
                dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), IM_COL32(0, 0, 0, 255));
            }
        }
    }
    ImGui::Dummy(ImVec2(size, size));
}

// Two codes, one per way the tablet can reach this PC. Scanning one in the
// tablet app sets the PC IP and connects, replacing typing it in.
void DrawQrModal(AppState* state) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Connect by QR", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const float size = 220.0f;
    struct Col { const char* title; const std::string* ip; const char* mode; };
    const Col cols[] = {
        {"USB tethering", &state->qrUsbIp, "usb"},
        {"Wi-Fi", &state->qrWifiIp, "wifi"},
    };
    for (int i = 0; i < 2; ++i) {
        if (i) ImGui::SameLine(0, 16.0f);
        ImGui::BeginGroup();
        ImGui::TextUnformatted(cols[i].title);
        DrawQr(*cols[i].ip, cols[i].mode, size);
        ImGui::TextColored(state->theme.textMuted, "%s",
            cols[i].ip->empty() ? "not connected" : cols[i].ip->c_str());
        ImGui::EndGroup();
    }
    ImGui::TextColored(state->theme.textMuted, "Scan from the tablet app: Change IP > Scan QR");
    if (ImGui::Button("Close", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawNetworkButtons(AppState* state, bool stacked) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float w = stacked ? avail : (avail - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
    if (AccentButton(state, "Detect tethering", ImVec2(w, 0))) DetectNetwork(state, true);
    if (!stacked) ImGui::SameLine();
    ImGui::BeginDisabled(!state->tetherHasAdapter);
    if (AccentButton(state, state->tetherAutoMetric ? "Fix network priority" : "Restore auto priority", ImVec2(w, 0))) {
        FixNetworkPriority(state);
    }
    ImGui::EndDisabled();
    if (AccentButton(state, "Connect by QR", ImVec2(avail, 0))) {
        state->qrUsbIp.clear();
        state->qrWifiIp.clear();
        for (const AdapterInfo& a : EnumerateIPv4Adapters()) {
            std::string& slot = a.likelyTethering ? state->qrUsbIp : state->qrWifiIp;
            if (slot.empty()) slot = a.ipv4;
        }
        ImGui::OpenPopup("Connect by QR");
    }
    DrawQrModal(state);
}

// The one value the user has to type by hand. withCaption=false is for the
// step layout, where the step title already says what the address is for.
void DrawBanner(AppState* state, bool withCaption) {
    const Theme& t = state->theme;
    if (state->bannerLabel.empty()) {
        // No address yet: the detection result is a sentence, not a value.
        if (state->bannerValue.empty()) return;
        ImGui::PushFont(state->sansFont, kCaptionFontSize);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(t.textMuted, "%s", state->bannerValue.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.bannerBg);
    ImGui::PushStyleColor(ImGuiCol_Border, t.bannerBorder);
    ImGui::BeginChild("##banner", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 normalSpacing = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(normalSpacing.x, normalSpacing.y * 0.5f));
    if (withCaption) {
        ImGui::PushFont(state->sansFont, kCaptionFontSize);
        ImGui::TextColored(t.bannerLabel, "%s", state->bannerLabel.c_str());
        ImGui::PopFont();
    }
    ImGui::PushFont(state->monoBoldFont, kBannerValueFontSize);
    ImGui::TextColored(t.bannerValue, "%s", state->bannerValue.c_str());
    ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

struct Metric {
    const char* label;
    char value[32];
};

void FillMetrics(AppState* state, Metric (&m)[16]) {
    const HostStats& stats = state->controller.GetStats();
    auto set = [](Metric& metric, const char* label) -> char* {
        metric.label = label;
        return metric.value;
    };
    std::snprintf(set(m[0], "FPS"), 32, "%u", stats.fps.load());
    std::snprintf(set(m[1], "Capture"), 32, "%.1f ms", stats.captureMs.load());
    std::snprintf(set(m[2], "Encode"), 32, "%.1f ms", stats.encodeMs.load());
    std::snprintf(set(m[3], "Send"), 32, "%.2f ms", stats.sendMs.load());
    std::snprintf(set(m[4], "Frame"), 32, "%u KB", stats.frameKB.load());
    std::snprintf(set(m[5], "Idle"), 32, "%u", stats.idleCount.load());
    std::snprintf(set(m[6], "Capped"), 32, "%u", stats.cappedCount.load());
    if (stats.hasPenData.load()) {
        std::snprintf(set(m[7], "Pen"), 32, "%u..%u", stats.penMin.load(), stats.penMax.load());
    } else {
        std::snprintf(set(m[7], "Pen"), 32, "-");
    }
    // Tablet-reported numbers stay zero until the tablet completes the
    // handshake - "no session" says so directly.
    if (stats.sessionActive.load()) {
        std::snprintf(set(m[8], "Tablet"), 32, "%u fps", stats.clientFps.load());
    } else {
        std::snprintf(set(m[8], "Tablet"), 32, "no session");
    }
    std::snprintf(set(m[9], "Loss"), 32, "%.1f %%", stats.lossPermille.load() / 10.0f);
    std::snprintf(set(m[10], "RTT"), 32, "%u ms", stats.rttMs.load());
    std::snprintf(set(m[11], "Decode"), 32, "%.1f ms", stats.clientDecodeMs.load());
    std::snprintf(set(m[12], "PLI/IDR"), 32, "%u / %u", stats.pliCount.load(), stats.idrCount.load());
    std::snprintf(set(m[13], "QoS fps"), 32, "%u", stats.qosFps.load());
    std::snprintf(set(m[14], "QoS kbps"), 32, "%u", stats.qosBitrateKbps.load());
    std::snprintf(set(m[15], "QoS steps"), 32, "%u", stats.qosDowngrades.load());
}

// Diagnostics: a collapsing header whose label carries the handful of
// numbers that say whether the stream is healthy, over a 4-column table of
// all sixteen. `id` keeps each layout's open/closed state separate, so the
// wide layout can open by default without forcing the narrow one open.
// stackedCells puts each label above its value (the wide layout's narrower
// columns); otherwise label and value share a line.
void DrawDiagnostics(AppState* state, const char* id, bool defaultOpen, bool stackedCells) {
    const Theme& t = state->theme;
    const HostStats& stats = state->controller.GetStats();
    char header[192];
    if (state->isRunning) {
        std::snprintf(header, sizeof(header),
            "Diagnostics   %u fps \xc2\xb7 encode %.1f ms \xc2\xb7 loss %.1f%% \xc2\xb7 PLI %u###%s",
            stats.fps.load(), stats.encodeMs.load(), stats.lossPermille.load() / 10.0f,
            stats.pliCount.load(), id);
    } else {
        std::snprintf(header, sizeof(header), "Diagnostics###%s", id);
    }

    // The theme's Header colour is the accent (used for selectable
    // highlights); a full accent bar here would shout. Card colours.
    ImGui::PushStyleColor(ImGuiCol_Header, t.cardBg);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, t.frameBgHover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, t.frameBgHover);
    const bool open = ImGui::CollapsingHeader(header, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    ImGui::PopStyleColor(3);
    if (!open) return;

    Metric m[16];
    FillMetrics(state, m);
    char tableId[48];
    std::snprintf(tableId, sizeof(tableId), "##table_%s", id);
    if (ImGui::BeginTable(tableId, 4, ImGuiTableFlags_SizingStretchSame)) {
        for (int i = 0; i < 16; ++i) {
            ImGui::TableNextColumn();
            if (stackedCells) {
                ImGui::PushFont(state->sansFont, kMetricLabelFontSize);
                ImGui::TextColored(t.textMuted, "%s", m[i].label);
                ImGui::PopFont();
            } else {
                ImGui::TextColored(t.textMuted, "%s", m[i].label);
                ImGui::SameLine();
            }
            ImGui::PushFont(state->monoFont, 0.0f);
            ImGui::TextUnformatted(m[i].value);
            ImGui::PopFont();
        }
        ImGui::EndTable();
    }
}

// Log: takes whatever height is left, never less than 5 lines.
void DrawLog(AppState* state) {
    ImGui::PushFont(state->monoFont, kLogFontSize);
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    ImGui::PopFont();
    const float minH = lineH * 5.0f + ImGui::GetStyle().WindowPadding.y * 2.0f;
    const float logH = ImGui::GetContentRegionAvail().y > minH ? ImGui::GetContentRegionAvail().y : minH;
    ImGui::BeginChild("##log", ImVec2(0, logH), ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    {
        std::lock_guard<std::mutex> lock(state->logMutex);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(state->logLines.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                ImGui::PushStyleColor(ImGuiCol_Text, LogLineColor(state, state->logLines[i]));
                ImGui::PushFont(state->monoFont, kLogFontSize);
                ImGui::TextUnformatted(state->logLines[i].c_str());
                ImGui::PopFont();
                ImGui::PopStyleColor();
            }
        }
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

// ---- Narrow layout: numbered setup steps ----

enum class StepState { Pending, Current, Done };

// Draws the step's number badge at the start of the current line and leaves
// the cursor just right of it. rowH is the height of the first line of the
// step's content, so the badge centres on it (a title line, or the tall
// Start/Stop button on the last step).
void StepBadge(AppState* state, int number, StepState s, float rowH) {
    const Theme& t = state->theme;
    const float d = kBadgeDiameter;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 c(p.x + d * 0.5f, p.y + rowH * 0.5f);
    if (s == StepState::Done) {
        dl->AddCircleFilled(c, d * 0.5f, ImGui::GetColorU32(kStatusStreaming));
        const ImVec2 tick[3] = { ImVec2(c.x - 4.5f, c.y + 0.5f), ImVec2(c.x - 1.5f, c.y + 3.5f),
                                 ImVec2(c.x + 4.5f, c.y - 3.5f) };
        dl->AddPolyline(tick, 3, IM_COL32_WHITE, ImDrawFlags_None, 2.0f);
    } else {
        if (s == StepState::Current) {
            dl->AddCircleFilled(c, d * 0.5f, ImGui::GetColorU32(t.accent));
        } else {
            dl->AddCircle(c, d * 0.5f - 0.75f, ImGui::GetColorU32(t.cardBorder), 0, 1.5f);
        }
        char num[4];
        std::snprintf(num, sizeof(num), "%d", number);
        const float fontSize = kBadgeFontSize;
        const ImVec2 size = state->monoBoldFont->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, num);
        dl->AddText(state->monoBoldFont, fontSize, ImVec2(c.x - size.x * 0.5f, c.y - size.y * 0.5f),
                    ImGui::GetColorU32(s == StepState::Current ? t.accentText : t.textMuted), num);
    }
    ImGui::Dummy(ImVec2(d, rowH));
    ImGui::SameLine(0, 10);
}

void StepTitle(AppState* state, StepState s, const char* title) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(s == StepState::Done ? state->theme.textMuted : state->theme.text, "%s", title);
}

void RenderNarrow(AppState* state, HWND hwnd) {
    const float rowStartX = ImGui::GetCursorPosX();
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    DrawTitle(state);
    ImGui::SameLine();
    DrawStatus(state);
    ThemeToggleAtRight(state, hwnd, rowStartX, rowWidth);

    // What can actually be verified, and nothing more: a tethering adapter
    // (or a live session, which proves the network path whichever way it
    // runs - Wi-Fi users never see an adapter) completes step 1; only the
    // tablet's HELLO proves step 2, which is why it can stay current while
    // streaming. Step 3 is a review, done once its settings were used.
    const HostStats& stats = state->controller.GetStats();
    const bool session = state->isRunning && stats.sessionActive.load();
    const bool linkFound = state->tetherHasAdapter || session;
    const StepState s1 = linkFound ? StepState::Done : StepState::Current;
    const StepState s2 = session ? StepState::Done : (linkFound ? StepState::Current : StepState::Pending);
    const StepState s3 = state->isRunning ? StepState::Done : StepState::Pending;
    const StepState s4 = state->isRunning ? StepState::Done : StepState::Pending;
    const float frameH = ImGui::GetFrameHeight();
    const float stepGap = 6.0f;

    ImGui::BeginChild("##steps", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    StepBadge(state, 1, s1, frameH);
    ImGui::BeginGroup();
    StepTitle(state, s1, "Connect the tablet (USB tethering or Wi-Fi)");
    DrawNetworkButtons(state, false);
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, stepGap));

    StepBadge(state, 2, s2, frameH);
    ImGui::BeginGroup();
    StepTitle(state, s2, "On the tablet, set PC IP to");
    DrawBanner(state, false);
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, stepGap));

    StepBadge(state, 3, s3, frameH);
    ImGui::BeginGroup();
    StepTitle(state, s3, "Check the connection");
    DrawConnectionRows(state, false);
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, stepGap));

    // One button that is Start or Stop, rather than a pair where one half is
    // always disabled.
    const float startH = kStartButtonHeight;
    StepBadge(state, 4, s4, startH);
    if (AccentButton(state, state->isRunning ? "Stop" : "Start", ImVec2(ImGui::GetContentRegionAvail().x, startH))) {
        if (state->isRunning) StopHost(state); else StartHost(state);
    }

    ImGui::EndChild();

    DrawDiagnostics(state, "diag_narrow", false, true);
    DrawLog(state);
}

// ---- Wide layout: settings left, diagnostics and log right ----

void RenderWide(AppState* state, HWND hwnd) {
    const float leftW = 330.0f;
    const float gap = 12.0f;
    const float h = ImGui::GetContentRegionAvail().y;
    const ImGuiWindowFlags columnFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    // Layout-only containers: no border, no fill (and therefore no padding),
    // so the cards inside them sit exactly where they would in one column.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##left", ImVec2(leftW, h), ImGuiChildFlags_None, columnFlags);
    ImGui::PopStyleColor();
    {
        const float rowStartX = ImGui::GetCursorPosX();
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        DrawTitle(state);
        ThemeToggleAtRight(state, hwnd, rowStartX, rowWidth);
        DrawStatus(state);

        ImGui::BeginChild("##connection", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawConnectionRows(state, true);
        ImGui::EndChild();

        // Stacked: side by side, "Fix network priority" doesn't fit a half
        // of this column.
        DrawNetworkButtons(state, true);
        DrawBanner(state, true);

        // Start/Stop pinned to the bottom of the column.
        const float startH = kStartButtonHeight;
        const float spare = ImGui::GetContentRegionAvail().y - startH - ImGui::GetStyle().ItemSpacing.y;
        if (spare > 0.0f) ImGui::Dummy(ImVec2(0, spare));
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
        ImGui::BeginDisabled(state->isRunning);
        if (AccentButton(state, "Start", ImVec2(half, startH))) StartHost(state);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!state->isRunning);
        if (AccentButton(state, "Stop", ImVec2(half, startH))) StopHost(state);
        ImGui::EndDisabled();
    }
    ImGui::EndChild();

    ImGui::SameLine(0, gap);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##right", ImVec2(0, h), ImGuiChildFlags_None, columnFlags);
    ImGui::PopStyleColor();
    {
        // There is room for the full table here, so it starts open.
        DrawDiagnostics(state, "diag_wide", true, true);
        DrawLog(state);
    }
    ImGui::EndChild();
}

void RenderUI(AppState* state, HWND hwnd) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    // Only the log should ever scroll - everything else is sized to fit
    // outright, so the outer window never needs to (accidental mouse-wheel
    // scroll over it is also disabled).
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const float width = viewport->WorkSize.x;
    if (!state->wideLayout && width >= kWideEnter) {
        state->wideLayout = true;
    } else if (state->wideLayout && width < kWideExit) {
        state->wideLayout = false;
    }

    if (state->wideLayout) {
        RenderWide(state, hwnd);
    } else {
        RenderNarrow(state, hwnd);
    }

    ImGui::End();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    // Must run before any GetSystemMetrics()/window call. Without this, an
    // unaware process gets a DPI-virtualized (scaled-down) screen size from
    // GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN), while DXGI Desktop
    // Duplication still captures at full physical resolution. VirtualPen
    // uses GetSystemMetrics to scale normalized pen coordinates to screen
    // pixels, so that mismatch compresses every injected point into the
    // top-left fraction of the real screen (e.g. only the top ~65% of the
    // screen is reachable at 150% scaling). Dear ImGui's own DPI helper is
    // NOT used - it would fight this.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const wchar_t* className = L"DisplayTwinHostWindow";
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // Large icon for Alt-Tab/the taskbar, small one for the title bar. Asking
    // for each at its own metric keeps Windows from stretching one to both.
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, className, L"Display Twin PC Host",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 540, 720,
        nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!CreateDeviceD3D(hwnd)) {
        MessageBoxW(nullptr, L"Failed to initialize Direct3D for the UI", L"Error", MB_OK | MB_ICONERROR);
        DestroyWindow(hwnd);
        UnregisterClassW(className, instance);
        return 1;
    }

    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // No layout to persist - this is a single fixed panel.

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    AppState state;

    // The default ImGui bitmap font (~13px) reads as a debug overlay, not an
    // app. Load the same fonts as the host-UI mockup - Manrope for general
    // text, JetBrains Mono for anything that's actually a value - at a
    // moderate base size; call sites PushFont(font, size) to size them per
    // use (this ImGui version rasterizes on demand, so that's cheap).
    // Whichever font is added FIRST becomes the atlas default used
    // everywhere with no PushFont needed, so add the sans body font first.
    // These come out of the exe's own resources, so the host stays a single
    // portable file; the system-font paths below are only a safety net.
    ImGuiIO& io = ImGui::GetIO();
    state.sansFont = AddEmbeddedFont(io, IDR_FONT_SANS, kBodyFontSize);
    state.monoFont = AddEmbeddedFont(io, IDR_FONT_MONO, kBodyFontSize);
    state.monoBoldFont = AddEmbeddedFont(io, IDR_FONT_MONO_BOLD, kBodyFontSize);
    if (!state.sansFont) {
        state.sansFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", kBodyFontSize);
    }
    if (!state.monoFont) {
        state.monoFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", kBodyFontSize);
    }
    if (!state.monoBoldFont) {
        state.monoBoldFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consolab.ttf", kBodyFontSize);
    }
    if (!state.sansFont) {
        state.sansFont = io.Fonts->AddFontDefault();  // Guaranteed fallback if nothing above is found.
    }
    if (!state.monoFont) {
        state.monoFont = state.sansFont;
    }
    if (!state.monoBoldFont) {
        state.monoBoldFont = state.monoFont;
    }

    std::memcpy(state.androidIp, DISPLAY_TWIN_TABLET_IP, sizeof(DISPLAY_TWIN_TABLET_IP));
    ApplyTheme(state.theme, hwnd, state.darkMode);

    // Show the adapter list right away, and pre-fill the tablet address if a
    // USB tethering link is already up. Does nothing to the IP box otherwise.
    DetectNetwork(&state, true);

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderUI(&state, hwnd);

        ImGui::Render();
        const float* clear = reinterpret_cast<const float*>(&state.theme.pageBg);
        g_context->OMSetRenderTargets(1, &g_renderTargetView, nullptr);
        g_context->ClearRenderTargetView(g_renderTargetView, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_swapChain->Present(1, 0);  // vsync on - paces the loop, keeps CPU idle between frames.
    }

    if (state.isRunning) {
        state.controller.Stop();
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(className, instance);
    return 0;
}
