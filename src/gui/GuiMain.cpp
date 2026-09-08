#include "app/HostController.h"
#include "app/HostConfig.h"
#include "app/Resource.h"
#include "network/NetworkInfo.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <windows.h>

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
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ----- Theme -----
// Same palette as the Display Twin host-UI mockup (Main.dc.html), so the
// real app and the design concept agree.
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
        t.pageBg = Rgb(10, 14, 17);
        t.cardBg = Rgb(18, 23, 27);
        t.cardBorder = Rgb(30, 38, 43);
        t.frameBg = Rgb(13, 17, 20);
        t.frameBgHover = Rgb(20, 26, 30);
        t.text = Rgb(231, 236, 239);
        t.textMuted = Rgb(138, 151, 160);
        t.accent = Rgb(110, 231, 219);
        t.accentText = Rgb(10, 14, 17);
        t.bannerBg = Rgb(15, 42, 38);
        t.bannerBorder = Rgb(31, 74, 66);
        t.bannerLabel = Rgb(110, 231, 219);
        t.bannerValue = Rgb(234, 255, 249);
        return t;
    }
    Theme t;
    t.pageBg = Rgb(250, 249, 247);
    t.cardBg = Rgb(255, 255, 255);
    t.cardBorder = Rgb(238, 236, 231);
    t.frameBg = Rgb(251, 250, 248);
    t.frameBgHover = Rgb(245, 243, 239);
    t.text = Rgb(28, 27, 26);
    t.textMuted = Rgb(138, 132, 121);
    t.accent = Rgb(194, 99, 47);
    t.accentText = Rgb(255, 248, 242);
    t.bannerBg = Rgb(253, 240, 230);
    t.bannerBorder = Rgb(243, 217, 189);
    t.bannerLabel = Rgb(138, 90, 48);
    t.bannerValue = Rgb(164, 83, 31);
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

    style.WindowPadding = ImVec2(16, 10);
    style.FramePadding = ImVec2(8, 5);
    style.ItemSpacing = ImVec2(10, 7);
    style.ItemInnerSpacing = ImVec2(6, 6);
    style.ChildRounding = 10.0f;
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
    return state->theme.text;
}

// One small labeled number, e.g. the mockup's "Encode  8.7 ms" tile.
void StatTile(const AppState* state, const char* label, const std::string& value, float width) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, state->theme.frameBg);
    // Tighter than the page-level WindowPadding (18,18) - a small tile can't
    // afford that much padding on all four sides and still fit label+value.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::BeginChild(label, ImVec2(width, 84), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextColored(state->theme.textMuted, "%s", label);
    ImGui::PushFont(state->monoBoldFont, 26.0f);
    ImGui::Text("%s", value.c_str());
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// Value font for the Connection card's Android IP / Ports / Target rows -
// a size bump over body text, but not as heavy/large as monoBoldFont (which
// is bold and meant for the banner/stat tiles).
constexpr float kValueFontSize = 21.0f;

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

void RenderUI(AppState* state, HWND hwnd) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    // Only the log (its own child below) should ever scroll - everything
    // above it is sized to fit outright, so the outer window never needs to
    // (accidental mouse-wheel scroll over it is also disabled).
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const Theme& t = state->theme;
    const float fullWidth = ImGui::GetContentRegionAvail().x;

    // ---- Header: title, status, theme toggle ----
    ImGui::PushFont(state->sansFont, 24.0f);  // Manrope, not mono - this is a title, not a value.
    ImGui::Text("Display Twin");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6);
    ImGui::TextColored(t.textMuted, "PC Host");

    ImGui::SameLine(fullWidth - 70);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 6);
    if (ImGui::Button(state->darkMode ? "Light" : "Dark", ImVec2(70, 28))) {
        state->darkMode = !state->darkMode;
        state->theme = MakeTheme(state->darkMode);
        ApplyTheme(state->theme, hwnd, state->darkMode);
    }

    if (state->isRunning) {
        const HostStats& stats = state->controller.GetStats();
        ImGui::TextColored(ImVec4(0.30f, 0.72f, 0.42f, 1.0f), "\xe2\x97\x8f  Streaming - %u fps", stats.fps.load());
    } else {
        ImGui::TextColored(ImVec4(0.82f, 0.62f, 0.22f, 1.0f), "\xe2\x97\x8b  Waiting");
    }

    ImGui::Dummy(ImVec2(0, 2));

    // ---- Connection info: flat label/value rows, not visible input boxes ----
    // Still real, editable ImGui widgets underneath - FrameBg/Border are
    // pushed transparent so they read as plain text until clicked, and each
    // is right-aligned by pre-computing its width and moving the cursor.
    ImGui::BeginChild("##connection", ImVec2(0, 150), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    {
        const float rowWidth = ImGui::GetContentRegionAvail().x;

        // A plain Text element has no FramePadding around it, but an
        // InputText/InputInt does - so a row ending in plain text (Target's
        // "kbps") needs this much extra reserved width to visually match the
        // trailing inset that rows ending in an input field get for free.
        // Skipping this was why Target/Ports didn't line up with Android IP.
        const float trailingTextInset = ImGui::GetStyle().FramePadding.x;

        // Row: Android IP. The field's width is sized to its own text (plus
        // room for the frame padding and caret) rather than a fixed box, so
        // with the cursor placed at rowWidth-w the value sits flush against
        // the row's right edge - ImGui's InputText has no built-in
        // right-text-alignment, this is the practical way to get that look.
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(t.textMuted, "Android IP");
        ImGui::SameLine();
        {
            const float w = DynFieldWidth(state, state->androidIp, 90.0f);
            ImGui::SetCursorPosX(rowWidth - w);
            ImGui::PushFont(state->monoFont, kValueFontSize);
            ImGui::SetNextItemWidth(w);
            ImGui::InputText("##ip", state->androidIp, sizeof(state->androidIp));
            ImGui::PopFont();
        }
        ImGui::Separator();

        // Row: Ports (video / input) - ends in an InputInt, same as the IP
        // row, so no trailing-inset compensation needed here.
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
            ImGui::SetCursorPosX(rowWidth - (wv + 4 + sepW + 4 + wi));
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
        ImGui::Separator();

        // Row: Target (fps + bitrate) - folds what used to be a separate
        // "Stream" card into this one, matching the mockup. Ends in plain
        // text ("kbps"), hence + trailingTextInset.
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
            ImGui::SetCursorPosX(rowWidth - (wf + 4 + unit1W + 4 + wb + 4 + unit2W + trailingTextInset));
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
    }
    ImGui::PopStyleColor(3);
    ImGui::EndChild();

    if (ImGui::Button("Detect tethering", ImVec2(-1, 0))) DetectNetwork(state, true);
    ImGui::BeginDisabled(!state->tetherHasAdapter);
    if (ImGui::Button(state->tetherAutoMetric ? "Fix network priority" : "Restore automatic priority", ImVec2(-1, 0))) {
        FixNetworkPriority(state);
    }
    ImGui::EndDisabled();

    // ---- Banner: the address to type on the tablet ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.bannerBg);
    ImGui::PushStyleColor(ImGuiCol_Border, t.bannerBorder);
    ImGui::BeginChild("##banner", ImVec2(0, 76), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (state->bannerLabel.empty()) {
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::PushFont(state->sansFont, 14.0f);
        ImGui::TextColored(t.bannerLabel, "%s", state->bannerValue.c_str());
        ImGui::PopFont();
    } else {
        ImGui::Dummy(ImVec2(0, 2));
        // Half the usual ItemSpacing between just this label and the value
        // below it - the mockup's caption sits close to its value, not a
        // full row-gap away.
        const ImVec2 normalSpacing = ImGui::GetStyle().ItemSpacing;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(normalSpacing.x, normalSpacing.y * 0.5f));
        ImGui::PushFont(state->sansFont, 14.0f);  // Slightly smaller than body text.
        ImGui::TextColored(t.bannerLabel, "%s", state->bannerLabel.c_str());
        ImGui::PopFont();
        ImGui::PushFont(state->monoBoldFont, 26.0f);
        ImGui::TextColored(t.bannerValue, "%s", state->bannerValue.c_str());
        ImGui::PopFont();
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    // ---- Start/Stop ----
    {
        const float half = (fullWidth - 12.0f) / 2.0f;
        ImGui::BeginDisabled(state->isRunning);
        if (ImGui::Button("Start", ImVec2(half, 34))) StartHost(state);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!state->isRunning);
        if (ImGui::Button("Stop", ImVec2(half, 34))) StopHost(state);
        ImGui::EndDisabled();
    }

    // ---- Live diagnostics: real numbers from HostController, not parsed log text ----
    ImGui::TextColored(t.textMuted, "LIVE DIAGNOSTICS");
    ImGui::Spacing();
    {
        const HostStats& stats = state->controller.GetStats();
        char buf[32];
        const float tileW = (fullWidth - 12.0f * 3) / 4.0f;

        std::snprintf(buf, sizeof(buf), "%u fps", stats.fps.load());
        StatTile(state, "FPS", buf, tileW);
        ImGui::SameLine();
        std::snprintf(buf, sizeof(buf), "%.1f ms", stats.captureMs.load());
        StatTile(state, "Capture", buf, tileW);
        ImGui::SameLine();
        std::snprintf(buf, sizeof(buf), "%.1f ms", stats.encodeMs.load());
        StatTile(state, "Encode", buf, tileW);
        ImGui::SameLine();
        std::snprintf(buf, sizeof(buf), "%u KB", stats.frameKB.load());
        StatTile(state, "Frame", buf, tileW);

        if (stats.hasPenData.load()) {
            std::snprintf(buf, sizeof(buf), "%u..%u", stats.penMin.load(), stats.penMax.load());
        } else {
            std::snprintf(buf, sizeof(buf), "-");
        }
        StatTile(state, "Pen", buf, tileW);
        ImGui::SameLine();
        std::snprintf(buf, sizeof(buf), "%.2f ms", stats.sendMs.load());
        StatTile(state, "Send", buf, tileW);
        ImGui::SameLine();
        std::snprintf(buf, sizeof(buf), "%u", stats.idleCount.load());
        StatTile(state, "Idle", buf, tileW);
        ImGui::SameLine();
        std::snprintf(buf, sizeof(buf), "%u", stats.cappedCount.load());
        StatTile(state, "Capped", buf, tileW);
    }

    // ---- Log ----
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextColored(t.textMuted, "LOG");
    ImGui::BeginChild("##log", ImVec2(0, 150), ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);  // ~5 lines - the one scrollable area.
    {
        std::lock_guard<std::mutex> lock(state->logMutex);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(state->logLines.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                ImGui::PushStyleColor(ImGuiCol_Text, LogLineColor(state, state->logLines[i]));
                ImGui::PushFont(state->monoFont, 0.0f);
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
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 600, 920,
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
    state.sansFont = AddEmbeddedFont(io, IDR_FONT_SANS, 18.0f);
    state.monoFont = AddEmbeddedFont(io, IDR_FONT_MONO, 18.0f);
    state.monoBoldFont = AddEmbeddedFont(io, IDR_FONT_MONO_BOLD, 18.0f);
    if (!state.sansFont) {
        state.sansFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    }
    if (!state.monoFont) {
        state.monoFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 18.0f);
    }
    if (!state.monoBoldFont) {
        state.monoBoldFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consolab.ttf", 18.0f);
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
