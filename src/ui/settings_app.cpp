#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "settings_app.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <commdlg.h>
#include <oleidl.h>
#include <string>
#include <vector>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <thread>
#include <cmath>
#include <unordered_map>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include "core/config.h"
#include "core/ipc_server.h"
#include "core/engine_state.h"
#include "core/video_optimizer.h"
#include "platform/win32/hardware_info.h"
#include "platform/win32/lockscreen_manager.h"
#include "thumbnail_manager.h"
#include "icons_fontawesome6.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace miniwp {

static HWND                     g_hWnd = nullptr;
static HINSTANCE                g_hInstance = nullptr;
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static bool                     g_isOpen = false;

// Hardware telemetry cache
static SystemHardwareInfo       g_hardwareInfo;
static bool                     g_hardwareInfoInit = false;

// Performance metrics cache
static bool   g_daemonConnected = false;
static bool   g_daemonPlaying = false;
static bool   g_daemonPaused = false;
static std::string g_daemonCurrentVideo = "";
static int    g_daemonFps = 0;
static double g_daemonVideoFps = 0.0;
static int    g_daemonWidth = 0;
static int    g_daemonHeight = 0;
static double g_daemonDuration = 0.0;
static double g_daemonCurrentTimeSec = 0.0;
static std::string g_daemonCodec = "";
static size_t g_daemonRamMB = 0;
static size_t g_daemonVramMB = 0;
static double g_daemonCpuPercent = 0.0;
static bool   g_daemonInjected = false;
static bool   g_daemonHwDecode = false;
static int    g_daemonActiveGpuIndex = 0;
static uint64_t g_daemonFramesRendered = 0;
static std::string g_daemonLastError = "";

static std::vector<float> g_cpuHistory(60, 0.0f);
static std::vector<float> g_ramHistory(60, 0.0f);
static std::vector<float> g_vramHistory(60, 0.0f);

// Modal state for video optimization prompt
static bool g_showOptimizeModal = false;
static std::string g_pendingOptimizePath;
static std::string g_pendingOptimizeAction;
static int g_pendingSourceW = 0;
static int g_pendingSourceH = 0;
static int g_pendingTargetW = 1920;
static int g_pendingTargetH = 1080;
static bool g_rememberDownscaleChoice = false;

// Modal state for Static Frame Capture & Target Selector
static bool g_showCaptureModal = false;
static std::string g_captureModalPath;
static double g_captureModalDuration = 10.0;
static double g_captureModalFps = 30.0;
static int g_captureModalSourceW = 1920;
static int g_captureModalSourceH = 1080;
static float g_captureModalTimeSec = 1.0f;
static int g_captureModalFrameNum = 30;
static bool g_captureModalTargetDesktop = true;
static bool g_captureModalTargetLockscreen = true;
static std::string g_captureModalStatus;

// Preview state in capture modal
static ComPtr<ID3D11ShaderResourceView> g_captureModalPreviewSRV;
static float g_captureModalPreviewSec = -1.0f;
static std::atomic<bool> g_captureModalPreviewPending{false};
static std::vector<uint8_t> g_captureModalPreviewBgra;
static int g_captureModalPreviewW = 320;
static int g_captureModalPreviewH = 180;
static std::atomic<bool> g_captureModalPreviewWorkerActive{false};
static std::atomic<bool> g_captureModalPreviewQueued{false};
static std::atomic<float> g_captureModalQueuedSec{0.0f};
static bool g_captureModalAutoPreviewRequested = false;
static uint64_t g_captureModalLastChangeTick = 0;

// Modern shell state: floating dock navigation + collapsible detail panel
static int  g_activePage = 0; // 0 Gallery, 1 Lock&Static, 2 General, 3 Performance
static int  g_selectedGalleryIdx = -1;
static bool g_detailVisible = false;
static float g_dpiScale = 1.0f;
static ImFont* g_fontSemibold = nullptr;

static void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    style.WindowRounding    = 12.0f;
    style.ChildRounding     = 14.0f;
    style.FrameRounding     = 10.0f;
    style.PopupRounding     = 12.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding      = 8.0f;
    style.TabRounding       = 10.0f;

    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.ChildBorderSize   = 1.0f;

    style.WindowPadding     = ImVec2(18.0f, 14.0f);
    style.FramePadding      = ImVec2(10.0f, 7.0f);
    style.ItemSpacing       = ImVec2(12.0f, 10.0f);
    style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize     = 10.0f;

    colors[ImGuiCol_Text]                  = ImVec4(0.945f, 0.961f, 0.976f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.580f, 0.639f, 0.722f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.035f, 0.047f, 0.082f, 0.96f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.067f, 0.090f, 0.149f, 0.94f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.067f, 0.090f, 0.149f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.220f, 0.910f, 1.000f, 0.18f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.090f, 0.126f, 0.208f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.122f, 0.165f, 0.267f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.150f, 0.200f, 0.320f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.035f, 0.047f, 0.082f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.055f, 0.075f, 0.125f, 1.00f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.067f, 0.090f, 0.149f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.035f, 0.047f, 0.082f, 0.35f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.220f, 0.910f, 1.000f, 0.45f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.220f, 0.910f, 1.000f, 0.65f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.220f, 0.910f, 1.000f, 0.90f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.220f, 0.910f, 1.000f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.220f, 0.910f, 1.000f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.945f, 0.961f, 0.976f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.220f, 0.910f, 1.000f, 0.12f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.220f, 0.910f, 1.000f, 0.22f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.220f, 0.910f, 1.000f, 0.32f);
    colors[ImGuiCol_Header]                = ImVec4(0.220f, 0.910f, 1.000f, 0.10f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.220f, 0.910f, 1.000f, 0.16f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.220f, 0.910f, 1.000f, 0.26f);
    colors[ImGuiCol_Separator]             = ImVec4(0.220f, 0.910f, 1.000f, 0.14f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.220f, 0.910f, 1.000f, 0.55f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.220f, 0.910f, 1.000f, 0.90f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.067f, 0.090f, 0.149f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.122f, 0.165f, 0.267f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.100f, 0.140f, 0.230f, 1.00f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.045f, 0.060f, 0.100f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.067f, 0.090f, 0.149f, 1.00f);
    colors[ImGuiCol_PlotLines]             = ImVec4(0.220f, 0.910f, 1.000f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]      = ImVec4(1.000f, 0.820f, 0.400f, 1.00f);
    colors[ImGuiCol_PlotHistogram]         = ImVec4(0.220f, 0.910f, 1.000f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(1.000f, 0.820f, 0.400f, 1.00f);
    colors[ImGuiCol_NavHighlight]          = ImVec4(0.220f, 0.910f, 1.000f, 0.80f);
    colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.020f, 0.030f, 0.060f, 0.55f);
    colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.020f, 0.030f, 0.060f, 0.62f);
}

static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (g_pSwapChain && SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)))) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelArray,
        2,
        D3D11_SDK_VERSION,
        &sd,
        &g_pSwapChain,
        &g_pd3dDevice,
        &featureLevel,
        &g_pd3dDeviceContext
    );

    if (res != S_OK) {
        return false;
    }

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

static void SendIpcAsync(const std::string& request_json) {
    std::thread([request_json]() {
        IpcClient client;
        client.SendRequest(request_json);
    }).detach();
}

static void ApplyAction(std::string utf8_path, std::string action) {
    if (action == "stop") {
        SendIpcAsync("{\"cmd\":\"stop\"}");
        g_daemonPlaying = false;
        g_daemonPaused = false;
        g_daemonCurrentVideo.clear();
        auto& cfg = g_config.Get();
        if (!cfg.wallpapers.empty()) {
            cfg.wallpapers[0].video_path = "";
            g_config.Save();
        }
        return;
    }
    if (action == "resume") {
        SendIpcAsync("{\"cmd\":\"resume\"}");
        g_daemonPlaying = true;
        g_daemonPaused = false;
        return;
    }

    if (utf8_path.empty()) return;
    if (utf8_path.find("\\MiniWallpaper\\optimized\\") == std::string::npos &&
        utf8_path.find("/MiniWallpaper/optimized/") == std::string::npos &&
        utf8_path.find("\\optimized\\") == std::string::npos &&
        utf8_path.find("/optimized/") == std::string::npos) {
        g_config.Get().AddToGallery(utf8_path);
        g_config.Save();
    }

    if (action == "wallpaper") {
        nlohmann::json req{{"cmd", "set_wallpaper"}, {"path", utf8_path}};
        SendIpcAsync(req.dump());
    }
}

static void StartVideoOptimization(const std::string& input_path, int target_w, int target_h, const std::string& action, int crop_mode = 0) {
    // Add source path to gallery immediately so user sees card right away
    g_config.Get().AddToGallery(input_path);
    g_config.Save();

    g_video_optimizer.StartOptimizeAsync(
        input_path,
        target_w,
        target_h,
        crop_mode,
        nullptr,
        [action](bool success, const std::string& output_path) {
            if (success && !output_path.empty()) {
                ApplyAction(output_path, action);
            }
        }
    );
}

static void RequestApplyVideo(std::string utf8_path, std::string action) {
    if (action == "stop" || action == "resume") {
        ApplyAction(utf8_path, action);
        return;
    }
    if (utf8_path.empty()) return;

    // Always ensure source path is in gallery
    g_config.Get().AddToGallery(utf8_path);
    g_config.Save();

    auto& cfg = g_config.Get();
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    if (screen_w <= 0) screen_w = 1920;
    if (screen_h <= 0) screen_h = 1080;

    auto probe = VideoOptimizer::Probe(utf8_path);
    auto [target_w, target_h] = VideoOptimizer::CalculateTargetDimensions(probe.width, probe.height, screen_w, screen_h, cfg.optimizer_crop_mode);

    // Check if optimized version already exists in cache
    if (VideoOptimizer::HasOptimizedCache(utf8_path, target_w, target_h)) {
        std::string opt_path = VideoOptimizer::GetOptimizedPath(utf8_path, target_w, target_h);
        ApplyAction(opt_path, action);
        return;
    }

    // Probe source video dimensions and framerate
    if (probe.valid && (probe.width > screen_w || probe.height > screen_h || probe.fps > 60.0)) {
        if (cfg.prompt_downscale) {
            g_pendingOptimizePath = utf8_path;
            g_pendingOptimizeAction = action;
            g_pendingSourceW = probe.width;
            g_pendingSourceH = probe.height;
            g_pendingTargetW = target_w;
            g_pendingTargetH = target_h;
            g_showOptimizeModal = true;
            return;
        } else if (cfg.auto_downscale_highres) {
            StartVideoOptimization(utf8_path, target_w, target_h, action, cfg.optimizer_crop_mode);
            return;
        }
    }

    ApplyAction(utf8_path, action);
}

static LRESULT WINAPI SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }

    switch (msg) {
        case WM_SIZE:
            if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;

        case WM_DROPFILES: {
            HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < fileCount; i++) {
                wchar_t filePath[MAX_PATH] = {};
                if (DragQueryFileW(hDrop, i, filePath, MAX_PATH)) {
                    int size_needed = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, NULL, 0, NULL, NULL);
                    std::string utf8_path(size_needed - 1, 0);
                    WideCharToMultiByte(CP_UTF8, 0, filePath, -1, &utf8_path[0], size_needed, NULL, NULL);

                    std::error_code ec;
                    auto ext = fs::path(filePath).extension().string();
                    for (auto& c : ext) c = (char)::tolower(c);
                    if (ext == ".mp4" || ext == ".webm" || ext == ".mkv" || ext == ".avi" || ext == ".mov") {
                        RequestApplyVideo(utf8_path, "wallpaper");
                        break;
                    }
                }
            }
            DragFinish(hDrop);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = (LONG)(860 * g_dpiScale);
            mmi->ptMinTrackSize.y = (LONG)(560 * g_dpiScale);
            return 0;
        }

        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;

        case WM_CLOSE:
            SettingsUI::Close();
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void FetchDaemonStatus() {
    g_daemonConnected = g_shared_engine_state.connected.load();
    g_daemonPlaying = g_shared_engine_state.playing.load();
    g_daemonPaused = g_shared_engine_state.paused.load();
    g_daemonCurrentVideo = g_shared_engine_state.GetCurrentVideo();
    g_daemonFps = g_shared_engine_state.fps.load();
    g_daemonVideoFps = g_shared_engine_state.video_fps.load();
    g_daemonWidth = g_shared_engine_state.width.load();
    g_daemonHeight = g_shared_engine_state.height.load();
    g_daemonDuration = g_shared_engine_state.duration.load();
    g_daemonCurrentTimeSec = g_shared_engine_state.current_time_sec.load();
    g_daemonCodec = g_shared_engine_state.GetCodec();
    g_daemonRamMB = g_shared_engine_state.ram_mb.load();
    g_daemonVramMB = g_shared_engine_state.vram_mb.load();
    g_daemonCpuPercent = g_shared_engine_state.cpu_percent.load();
    g_daemonInjected = g_shared_engine_state.injected.load();
    g_daemonHwDecode = g_shared_engine_state.hw_decode.load();
    g_daemonActiveGpuIndex = g_shared_engine_state.active_gpu_index.load();
    g_daemonFramesRendered = g_shared_engine_state.frames_rendered.load();
    g_daemonLastError = g_shared_engine_state.GetLastError();

    g_ramHistory.erase(g_ramHistory.begin());
    g_ramHistory.push_back(static_cast<float>(g_daemonRamMB));

    g_cpuHistory.erase(g_cpuHistory.begin());
    g_cpuHistory.push_back(static_cast<float>(g_daemonCpuPercent));

    g_vramHistory.erase(g_vramHistory.begin());
    g_vramHistory.push_back(static_cast<float>(g_daemonVramMB));
}

static void OpenCaptureModal(const std::string& video_path, int targetFilter = 0);
static void ApplyAction(std::string utf8_path, std::string action);
static void RequestApplyVideo(std::string utf8_path, std::string action);
static void StartVideoOptimization(const std::string& input_path, int target_w, int target_h, const std::string& action, int crop_mode);

// Bayangan lembut berlapis utk elemen melayang (efek z-index: terlihat ngambang, bukan ditempel).
static void DrawFloatingShadow(ImVec2 p0, ImVec2 p1, float rounding) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p0.x - 12.0f, p0.y - 6.0f), ImVec2(p1.x + 12.0f, p1.y + 14.0f), IM_COL32(0, 0, 0, 26), rounding + 12.0f);
    dl->AddRectFilled(ImVec2(p0.x - 7.0f, p0.y - 3.0f), ImVec2(p1.x + 7.0f, p1.y + 9.0f), IM_COL32(0, 0, 0, 34), rounding + 7.0f);
    dl->AddRectFilled(ImVec2(p0.x - 3.0f, p0.y - 1.0f), ImVec2(p1.x + 3.0f, p1.y + 5.0f), IM_COL32(0, 0, 0, 42), rounding + 3.0f);
}

// =========================================================================
// STARTORCH UI KIT (MiniWallpaper edition) — atomic widgets
// Palet: biru → putih (+ aksen gold, pink starlume, & merah). Tanpa emoji/glyph dekoratif.
// =========================================================================
#define ST_BG_BASE     IM_COL32(9, 12, 21, 255)
#define ST_PANEL       IM_COL32(17, 23, 38, 245)
#define ST_PANEL_ELEV  IM_COL32(23, 32, 53, 255)
#define ST_GLASS       IM_COL32(20, 28, 46, 235)
#define ST_GLASS_HOVER IM_COL32(29, 41, 66, 244)
#define ST_CYAN        IM_COL32(56, 232, 255, 255)
#define ST_WHITE       IM_COL32(241, 245, 249, 255)
#define ST_TEXT        IM_COL32(241, 245, 249, 255)
#define ST_TEXT_DIM    IM_COL32(148, 163, 184, 255)
#define ST_TEXT_MUTED  IM_COL32(100, 116, 139, 255)
#define ST_GOLD        IM_COL32(255, 209, 102, 255)
#define ST_RED         IM_COL32(255, 77, 109, 255)
#define ST_CYAN_LINE   IM_COL32(56, 232, 255, 46)
#define ST_CYAN_EDGE   IM_COL32(56, 232, 255, 107)
#define ST_PINK        IM_COL32(255, 148, 178, 255) // aksen starlume (kit)
#define ST_PINK_SOFT   IM_COL32(255, 196, 214, 255)
#define ST_GREEN       IM_COL32(46, 229, 157, 255)

static ImU32 StAlpha(ImU32 col, float a) {
    ImVec4 f = ImGui::ColorConvertU32ToFloat4(col);
    f.w = a;
    return ImGui::ColorConvertFloat4ToU32(f);
}

static ImU32 StMix(ImU32 a, ImU32 b, float t) {
    ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a);
    ImVec4 cb = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t,
        ca.z + (cb.z - ca.z) * t, ca.w + (cb.w - ca.w) * t));
}

// Animasi nilai per-widget (hover/toggle lerp) berbasis ID widget.
static std::unordered_map<ImGuiID, float> g_animFloats;
static float AnimTowards(ImGuiID key, float target, float speed) {
    float& v = g_animFloats[key];
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.2f) dt = 1.0f / 60.0f;
    float step = speed * dt;
    if (target > v) v = (target - v < step) ? target : v + step;
    else if (target < v) v = (v - target < step) ? target : v - step;
    return v;
}

// Bar gradien horizontal dgn ujung membulat (cap solid + body gradien, seamless).
static void StGradientBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, ImU32 leftCol, ImU32 rightCol) {
    float w = b.x - a.x;
    if (w <= 1.0f) return;
    float r = rounding;
    if (w < r * 3.0f) {
        dl->AddRectFilled(a, b, leftCol, r);
        return;
    }
    dl->AddRectFilled(a, ImVec2(a.x + r * 2.0f, b.y), leftCol, r, ImDrawFlags_RoundCornersLeft);
    dl->AddRectFilled(ImVec2(b.x - r * 2.0f, a.y), b, rightCol, r, ImDrawFlags_RoundCornersRight);
    dl->AddRectFilledMultiColor(ImVec2(a.x + r, a.y), ImVec2(b.x - r, b.y), leftCol, rightCol, rightCol, leftCol);
}

// Bar gradien starlume 3-stop (cyan → pink → gold, sesuai kit) dgn ujung membulat.
static void StGradientStarlume(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding) {
    float w = b.x - a.x;
    if (w <= 1.5f) return;
    float midX = a.x + w * 0.52f;
    dl->AddRectFilledMultiColor(a, ImVec2(midX, b.y), ST_CYAN, ST_PINK, ST_PINK, ST_CYAN);
    dl->AddRectFilledMultiColor(ImVec2(midX, a.y), b, ST_PINK, ST_GOLD, ST_GOLD, ST_PINK);
    float cap = (std::min)(rounding * 2.0f, w * 0.5f);
    dl->AddRectFilled(a, ImVec2(a.x + cap, b.y), ST_CYAN, rounding, ImDrawFlags_RoundCornersLeft);
    dl->AddRectFilled(ImVec2(b.x - cap, a.y), b, ST_GOLD, rounding, ImDrawFlags_RoundCornersRight);
}

// Glow lembut utk rounded-rect (digambar di belakang shape).
static void DrawGlowRect(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, ImU32 color, float strength) {
    if (strength <= 0.01f) return;
    for (int i = 3; i >= 1; --i) {
        float off = (float)i * 2.2f;
        dl->AddRectFilled(ImVec2(a.x - off, a.y - off), ImVec2(b.x + off, b.y + off),
                          StAlpha(color, 0.06f * strength / (float)i), rounding + off);
    }
}

// Sweep shine (Startorch Holo): band menyala melintas sekali saat hover.
static void StDrawSweep(ImDrawList* dl, ImVec2 a, ImVec2 b, float progress, ImU32 color) {
    float w = b.x - a.x;
    if (w <= 2.0f || progress < 0.0f || progress > 1.0f) return;
    float bw = w * 0.42f;
    float bx = a.x - bw + (w + 2.0f * bw) * progress;
    dl->PushClipRect(a, b, true);
    ImU32 peak = StAlpha(color, 0.32f);
    ImU32 none = StAlpha(color, 0.0f);
    dl->AddRectFilledMultiColor(ImVec2(bx, a.y), ImVec2(bx + bw * 0.5f, b.y), none, peak, peak, none);
    dl->AddRectFilledMultiColor(ImVec2(bx + bw * 0.5f, a.y), ImVec2(bx + bw, b.y), peak, none, none, peak);
    dl->PopClipRect();
}

// ---- Tombol Holo (Startorch Variant 2: Secondary Holo + Sweep) ----
static bool HoloButtonEx(const char* label, ImVec2 size, bool danger, bool compact) {
    std::string txt(label ? label : "");
    for (auto& c : txt) if (c >= 'a' && c <= 'z') c = (char)(c - 32); // uppercase ASCII; byte UTF-8 (ikon) aman

    ImFont* font = ImGui::GetFont();
    float fs = ImGui::GetFontSize() + (compact ? 0.0f : 0.5f);
    ImVec2 tsz = font->CalcTextSizeA(fs, 10000.0f, 0.0f, txt.c_str());
    float h = compact ? 27.0f : 36.0f;
    float padX = compact ? 11.0f : 17.0f;
    if (size.x > 0.0f) size.x = (std::max)(size.x, tsz.x + padX * 2.0f);
    else size.x = tsz.x + padX * 2.0f;
    if (size.y > 0.0f) h = size.y;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGuiID id = ImGui::GetID(label ? label : "");
    ImGui::InvisibleButton(label ? label : "##btn", ImVec2(size.x, h));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    float ht = AnimTowards(id ^ 0x4857u, hovered ? 1.0f : 0.0f, 10.0f);

    float& sw = g_animFloats[id ^ 0x5357u];
    if (!hovered) sw = -1.0f;
    else if (sw < 0.0f) sw = 0.0f;
    if (sw >= 0.0f && sw < 1.0f) {
        sw += ImGui::GetIO().DeltaTime / 0.55f;
        if (sw > 1.0f) sw = 1.0f;
    }

    float lift = ht * 2.0f;
    ImVec2 a = ImVec2(pos.x, pos.y - lift);
    ImVec2 b = ImVec2(pos.x + size.x, pos.y + h - lift);
    ImU32 accent = danger ? ST_RED : ST_CYAN;
    ImU32 fill = StAlpha(accent, (danger ? 0.15f : 0.09f) + (danger ? 0.22f : 0.10f) * ht);
    ImU32 border = StAlpha(accent, 0.42f + 0.58f * ht);
    ImU32 txtCol = StMix(accent, ST_WHITE, 0.35f + 0.45f * ht);
    float round = compact ? 8.0f : 10.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ht > 0.01f) DrawGlowRect(dl, a, b, round, accent, ht);
    dl->AddRectFilled(a, b, fill, round);
    if (sw >= 0.0f && sw <= 1.0f) StDrawSweep(dl, a, b, sw, accent);
    dl->AddRect(a, b, border, round, 0, ht > 0.45f ? 1.5f : 1.1f);

    ImVec2 tp = ImVec2(a.x + (size.x - tsz.x) * 0.5f, a.y + (h - tsz.y) * 0.5f);
    dl->AddText(font, fs, tp, txtCol, txt.c_str());
    return clicked;
}

static bool HoloButton(const char* label, ImVec2 size = ImVec2(0, 0)) { return HoloButtonEx(label, size, false, false); }
static bool HoloDangerButton(const char* label, ImVec2 size = ImVec2(0, 0)) { return HoloButtonEx(label, size, true, false); }
static bool HoloButtonSmall(const char* label) { return HoloButtonEx(label, ImVec2(0, 0), false, true); }
static bool HoloDangerButtonSmall(const char* label) { return HoloButtonEx(label, ImVec2(0, 0), true, true); }

// ---- Slot ikon (dock & pill): gaya segmented-pill, tanpa teks ----
static bool HoloIconButton(const char* id, const char* icon, float w, float h, float rounding, bool danger, bool active, const char* tooltip) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGuiID uid = ImGui::GetID(id);
    ImGui::InvisibleButton(id, ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    float ht = AnimTowards(uid ^ 0x1C07u, hovered ? 1.0f : 0.0f, 12.0f);

    ImU32 accent = danger ? ST_RED : ST_CYAN;
    ImVec2 a = pos, b = ImVec2(pos.x + w, pos.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (active) {
        DrawGlowRect(dl, a, b, rounding, accent, 0.9f);
        dl->AddRectFilled(a, b, StAlpha(accent, 0.22f), rounding);
        dl->AddRect(a, b, StAlpha(accent, 0.60f), rounding, 0, 1.2f);
        StGradientBar(dl, ImVec2(a.x + w * 0.28f, b.y - 4.0f), ImVec2(b.x - w * 0.28f, b.y - 1.5f), 1.25f, StAlpha(accent, 0.55f), ST_WHITE);
    } else if (ht > 0.01f) {
        dl->AddRectFilled(a, b, StAlpha(accent, 0.10f * ht), rounding);
        dl->AddRect(a, b, StAlpha(accent, 0.35f * ht), rounding, 0, 1.0f);
    }

    ImU32 fg = active ? ST_WHITE : StMix(ST_TEXT_DIM, accent, ht);
    ImVec2 tsz = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(a.x + (w - tsz.x) * 0.5f, a.y + (h - tsz.y) * 0.5f), fg, icon);
    if (hovered && tooltip) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

// ---- Toggle switch (pengganti checkbox) ----
static bool ToggleSwitch(const char* label, bool* v) {
    const float w = 48.0f, h = 26.0f, gap = 10.0f;
    bool hasLabel = (label && label[0] != '\0');
    ImVec2 tsz = hasLabel ? ImGui::CalcTextSize(label) : ImVec2(0, 0);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float totalW = w + (hasLabel ? gap + tsz.x : 0.0f);
    ImGuiID id = ImGui::GetID(hasLabel ? label : "##tgl");
    ImGui::InvisibleButton(hasLabel ? label : "##tgl", ImVec2(totalW, h));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    if (clicked) *v = !(*v);
    float t = AnimTowards(id ^ 0x70B5u, *v ? 1.0f : 0.0f, 13.0f);
    float hov = AnimTowards(id ^ 0x70B6u, hovered ? 1.0f : 0.0f, 10.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = pos, b = ImVec2(pos.x + w, pos.y + h);
    if (t > 0.02f) DrawGlowRect(dl, a, b, h * 0.5f, ST_CYAN, t * 0.7f);
    dl->AddRectFilled(a, b, StMix(ST_PANEL_ELEV, StAlpha(ST_CYAN, 0.20f), t), h * 0.5f);
    ImU32 border = StMix(StAlpha(ST_CYAN, 0.16f + 0.24f * hov), StAlpha(ST_CYAN, 0.85f), t);
    dl->AddRect(a, b, border, h * 0.5f, 0, 1.1f);

    float thumbX = a.x + 12.0f + t * (w - 24.0f);
    float cy = a.y + h * 0.5f;
    if (t > 0.02f) dl->AddCircleFilled(ImVec2(thumbX, cy), 11.0f, StAlpha(ST_CYAN, 0.16f * t));
    dl->AddCircleFilled(ImVec2(thumbX, cy), 9.0f, StMix(ST_TEXT_MUTED, ST_CYAN, t));
    dl->AddCircle(ImVec2(thumbX, cy), 9.0f, IM_COL32(9, 12, 21, 170), 0, 1.2f);

    if (hasLabel) {
        dl->AddText(ImVec2(a.x + w + gap, pos.y + (h - tsz.y) * 0.5f), ST_TEXT, label);
    }
    return clicked;
}

// ---- Slider Starlume (Variant 6): track gradien biru→putih, thumb diamond gold ----
static bool StarlumeSliderEx(const char* label, float* v, float vmin, float vmax, int decimals, const char* suffix, float width) {
    if (vmax <= vmin) return false;
    ImGui::PushID(label);
    float availW = ImGui::GetContentRegionAvail().x;
    float w = (width > 0.0f) ? (std::min)(width, availW) : availW;
    if (w < 40.0f) w = 40.0f;

    char valBuf[64];
    snprintf(valBuf, sizeof(valBuf), "%.*f%s", decimals, (double)*v, suffix ? suffix : "");

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(pos, ST_TEXT_DIM, label);
    ImVec2 vtsz = ImGui::CalcTextSize(valBuf);
    dl->AddText(ImVec2(pos.x + w - vtsz.x, pos.y), ST_GOLD, valBuf);
    ImGui::Dummy(ImVec2(w, ImGui::GetFontSize() + 5.0f));

    float trackH = 24.0f;
    ImVec2 tpos = ImGui::GetCursorScreenPos();
    ImGuiID id = ImGui::GetID("##slider");
    ImGui::InvisibleButton("##slider", ImVec2(w, trackH));
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    bool changed = false;
    if (active) {
        float nx = (ImGui::GetIO().MousePos.x - tpos.x) / w;
        nx = (std::min)(1.0f, (std::max)(0.0f, nx));
        float nv = vmin + nx * (vmax - vmin);
        if (nv != *v) { *v = nv; changed = true; }
    }
    float frac = (*v - vmin) / (vmax - vmin);
    frac = (std::min)(1.0f, (std::max)(0.0f, frac));
    float ht = AnimTowards(id ^ 0x5A17u, (hovered || active) ? 1.0f : 0.0f, 12.0f);

    ImVec2 a = ImVec2(tpos.x, tpos.y + (trackH - 7.0f) * 0.5f);
    ImVec2 b = ImVec2(tpos.x + w, a.y + 7.0f);
    dl->AddRectFilled(a, b, ST_BG_BASE, 3.5f);
    float fx = a.x + (b.x - a.x) * frac;
    if (fx > a.x + 1.5f) StGradientBar(dl, a, ImVec2(fx, b.y), 3.5f, ST_CYAN, ST_WHITE);
    dl->AddRect(a, b, ST_CYAN_LINE, 3.5f, 0, 1.0f);

    float cx = (std::min)(b.x - 3.0f, (std::max)(a.x + 3.0f, fx));
    float cyy = (a.y + b.y) * 0.5f;
    float r = 8.0f + 2.0f * ht;
    if (ht > 0.01f) {
        for (int i = 3; i >= 1; --i) {
            float gr = r + (float)i * 2.0f;
            ImVec2 gp[4] = { ImVec2(cx, cyy - gr), ImVec2(cx + gr, cyy), ImVec2(cx, cyy + gr), ImVec2(cx - gr, cyy) };
            dl->AddConvexPolyFilled(gp, 4, StAlpha(ST_GOLD, 0.06f * ht / (float)i));
        }
    }
    ImVec2 dp[4] = { ImVec2(cx, cyy - r), ImVec2(cx + r, cyy), ImVec2(cx, cyy + r), ImVec2(cx - r, cyy) };
    dl->AddConvexPolyFilled(dp, 4, ST_GOLD);
    dl->AddPolyline(dp, 4, ST_BG_BASE, 2.0f, ImDrawFlags_Closed);
    ImGui::PopID();
    return changed;
}

static bool StarlumeSliderInt(const char* label, int* v, int vmin, int vmax, const char* suffix = "", float width = -1.0f) {
    float fv = (float)*v;
    if (StarlumeSliderEx(label, &fv, (float)vmin, (float)vmax, 0, suffix, width)) {
        int nv = (int)std::lround(fv);
        nv = (std::min)(vmax, (std::max)(vmin, nv));
        if (nv != *v) { *v = nv; return true; }
    }
    return false;
}

static bool StarlumeSliderFloat(const char* label, float* v, float vmin, float vmax, int decimals = 1, const char* suffix = "", float width = -1.0f) {
    return StarlumeSliderEx(label, v, vmin, vmax, decimals, suffix, width);
}

// ---- Resource usage bars (Startorch Variant 1: STARLUME HARMONY) ----
static void StBarHeader(const char* label, const char* valText, ImU32 valCol, ImU32 labelCol) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(pos, labelCol, label);
    ImVec2 vtsz = ImGui::CalcTextSize(valText);
    dl->AddText(ImVec2(pos.x + w - vtsz.x, pos.y), valCol, valText);
    ImGui::Dummy(ImVec2(w, ImGui::GetFontSize() + 4.0f));
}

static void StarlumeBarInternal(const char* valText, float pct01, bool withHeader, const char* label, ImU32 valCol, ImU32 labelCol, float marker01) {
    pct01 = (std::min)(1.0f, (std::max)(0.0f, pct01));
    float w = ImGui::GetContentRegionAvail().x;
    if (withHeader) StBarHeader(label, valText, valCol, labelCol);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 a = pos, b = ImVec2(pos.x + w, pos.y + 10.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, ST_BG_BASE, 5.0f);
    float fx = a.x + (b.x - a.x) * pct01;
    if (fx > a.x + 2.0f) {
        StGradientStarlume(dl, a, ImVec2(fx, b.y), 5.0f);
        if (fx > a.x + 8.0f) {
            dl->PushClipRect(a, ImVec2(fx - 1.0f, b.y), true);
            float bw = (b.x - a.x) * 0.30f;
            float phase = fmodf((float)ImGui::GetTime() * 0.55f, 1.0f);
            float bx = a.x - bw + ((b.x - a.x) + 2.0f * bw) * phase;
            ImU32 peak = StAlpha(ST_WHITE, 0.30f);
            ImU32 none = StAlpha(ST_WHITE, 0.0f);
            dl->AddRectFilledMultiColor(ImVec2(bx, a.y), ImVec2(bx + bw * 0.5f, b.y), none, peak, peak, none);
            dl->AddRectFilledMultiColor(ImVec2(bx + bw * 0.5f, a.y), ImVec2(bx + bw, b.y), peak, none, none, peak);
            dl->PopClipRect();
        }
    }
    dl->AddRect(a, b, StAlpha(ST_CYAN, 0.25f), 5.0f, 0, 1.0f);
    if (marker01 >= 0.0f) {
        float mx = a.x + (b.x - a.x) * (std::min)(1.0f, (std::max)(0.0f, marker01));
        dl->AddLine(ImVec2(mx, a.y - 2.0f), ImVec2(mx, b.y + 2.0f), StAlpha(ST_GOLD, 0.85f), 1.6f);
    }
    ImGui::Dummy(ImVec2(w, 10.0f));
}

static void StarlumeBar(const char* label, float pct01, const char* valText, ImU32 valCol = ST_GOLD, ImU32 labelCol = ST_PINK_SOFT, float marker01 = -1.0f) {
    StarlumeBarInternal(valText, pct01, true, label, valCol, labelCol, marker01);
}

// ---- Progress bar Starlume (utk progress optimisasi) ----
static void StarlumeProgress(float pct01, float h, const char* text) {
    pct01 = (std::min)(1.0f, (std::max)(0.0f, pct01));
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 a = pos, b = ImVec2(pos.x + w, pos.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = h * 0.5f;
    dl->AddRectFilled(a, b, ST_BG_BASE, r);
    float fx = a.x + (b.x - a.x) * pct01;
    if (fx > a.x + 1.5f) {
        StGradientBar(dl, a, ImVec2(fx, b.y), r, ST_CYAN, ST_WHITE);
        dl->PushClipRect(a, ImVec2(fx, b.y), true);
        float bw = (b.x - a.x) * 0.25f;
        float phase = fmodf((float)ImGui::GetTime() * 0.7f, 1.0f);
        float bx = a.x - bw + ((b.x - a.x) + 2.0f * bw) * phase;
        ImU32 peak = StAlpha(ST_WHITE, 0.35f);
        ImU32 none = StAlpha(ST_WHITE, 0.0f);
        dl->AddRectFilledMultiColor(ImVec2(bx, a.y), ImVec2(bx + bw * 0.5f, b.y), none, peak, peak, none);
        dl->AddRectFilledMultiColor(ImVec2(bx + bw * 0.5f, a.y), ImVec2(bx + bw, b.y), peak, none, none, peak);
        dl->PopClipRect();
    }
    dl->AddRect(a, b, ST_CYAN_LINE, r, 0, 1.0f);
    if (text && text[0]) {
        ImVec2 tsz = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(a.x + (w - tsz.x) * 0.5f, a.y + (h - tsz.y) * 0.5f), ST_TEXT, text);
    }
    ImGui::Dummy(ImVec2(w, h));
}

// ---- Module card (Startorch Variant 4: Chamfered Module) ----
enum StCardState { ST_CARD_NORMAL = 0, ST_CARD_SELECTED = 1, ST_CARD_ACTIVE = 2 };

static void StChamferPath(ImVec2 a, ImVec2 b, float chamfer, float inflate, ImVec2* p) {
    p[0] = ImVec2(a.x - inflate, a.y - inflate);
    p[1] = ImVec2(b.x - chamfer - inflate, a.y - inflate);
    p[2] = ImVec2(b.x + inflate, a.y + chamfer + inflate);
    p[3] = ImVec2(b.x + inflate, b.y + inflate);
    p[4] = ImVec2(a.x + chamfer + inflate, b.y + inflate);
    p[5] = ImVec2(a.x - inflate, b.y - chamfer - inflate);
}

static bool BeginModuleCard(const char* id, ImVec2 size, float chamfer = 18.0f, int state = ST_CARD_NORMAL, ImGuiWindowFlags flags = 0, float padX = 16.0f, float padY = 14.0f) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 sz = size;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (sz.x <= 0.0f) sz.x = avail.x;
    if (sz.y <= 0.0f) sz.y = avail.y;
    ImVec2 a = pos, b = ImVec2(pos.x + sz.x, pos.y + sz.y);

    ImGuiID uid = ImGui::GetID(id);
    bool hovered = ImGui::IsMouseHoveringRect(a, b);
    float ht = AnimTowards(uid ^ 0x0C0Du, (hovered && state == ST_CARD_NORMAL) ? 1.0f : 0.0f, 9.0f);

    ImU32 accent = (state == ST_CARD_ACTIVE) ? ST_GOLD : ST_CYAN;
    float glow = (state == ST_CARD_SELECTED) ? 0.55f : (state == ST_CARD_ACTIVE ? 0.5f : ht * 0.45f);
    ImU32 fill = ST_GLASS;
    if (state == ST_CARD_NORMAL && hovered) fill = ST_GLASS_HOVER;
    else if (state == ST_CARD_SELECTED) fill = IM_COL32(24, 38, 60, 240);
    else if (state == ST_CARD_ACTIVE) fill = IM_COL32(38, 34, 22, 240);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p[6];
    if (glow > 0.02f) {
        for (int i = 3; i >= 1; --i) {
            float off = (float)i * 2.4f;
            StChamferPath(a, b, chamfer, off, p);
            dl->AddConvexPolyFilled(p, 6, StAlpha(accent, 0.045f * glow / (float)i));
        }
    }
    StChamferPath(a, b, chamfer, 0.0f, p);
    dl->AddConvexPolyFilled(p, 6, fill);
    ImU32 border = (state != ST_CARD_NORMAL) ? StAlpha(accent, 0.85f) : StMix(ST_CYAN_LINE, ST_CYAN_EDGE, ht);
    dl->AddPolyline(p, 6, border, (state == ST_CARD_NORMAL) ? 1.2f : 1.6f, ImDrawFlags_Closed);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild(id, sz, ImGuiChildFlags_None, flags);
    return hovered;
}

static void EndModuleCard() {
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

// ---- Glass card (Startorch Variant 1: Exo-Genesis Standard Glass + L-brackets) ----
static void StDrawLBrackets(ImDrawList* dl, ImVec2 a, ImVec2 b, float t) {
    float L = 12.0f + 10.0f * t; // melebar saat hover (12 → 22 px)
    ImU32 cyan = StMix(ST_CYAN, ST_WHITE, 0.35f * t);
    ImU32 pink = StMix(ST_PINK, ST_WHITE, 0.35f * t);
    if (t > 0.05f) {
        ImU32 gcy = StAlpha(ST_CYAN, 0.16f * t);
        ImU32 gpk = StAlpha(ST_PINK, 0.16f * t);
        dl->AddLine(ImVec2(a.x + 2.0f, a.y + 2.0f + L), ImVec2(a.x + 2.0f, a.y + 2.0f), gcy, 5.0f);
        dl->AddLine(ImVec2(a.x + 2.0f, a.y + 2.0f), ImVec2(a.x + 2.0f + L, a.y + 2.0f), gcy, 5.0f);
        dl->AddLine(ImVec2(b.x - 2.0f, b.y - 2.0f - L), ImVec2(b.x - 2.0f, b.y - 2.0f), gpk, 5.0f);
        dl->AddLine(ImVec2(b.x - 2.0f, b.y - 2.0f), ImVec2(b.x - 2.0f - L, b.y - 2.0f), gpk, 5.0f);
    }
    dl->AddLine(ImVec2(a.x + 2.0f, a.y + 2.0f + L), ImVec2(a.x + 2.0f, a.y + 2.0f), cyan, 2.0f);
    dl->AddLine(ImVec2(a.x + 2.0f, a.y + 2.0f), ImVec2(a.x + 2.0f + L, a.y + 2.0f), cyan, 2.0f);
    dl->AddLine(ImVec2(b.x - 2.0f, b.y - 2.0f - L), ImVec2(b.x - 2.0f, b.y - 2.0f), pink, 2.0f);
    dl->AddLine(ImVec2(b.x - 2.0f, b.y - 2.0f), ImVec2(b.x - 2.0f - L, b.y - 2.0f), pink, 2.0f);
}

struct StGlassCardPending {
    ImGuiID uid;
    bool brackets;
};
static StGlassCardPending g_stGlassPending[8];
static int g_stGlassPendingN = 0;

// size.y <= 0 → tinggi kartu mengikuti konten (auto-fit child). Latar + bracket
// digambar di EndGlassCard setelah ukuran child final, jadi auto-height akurat.
static void BeginGlassCard(const char* id, ImVec2 size, float padX = 18.0f, float padY = 16.0f, ImGuiWindowFlags flags = 0, bool brackets = true) {
    ImVec2 sz = size;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (sz.x <= 0.0f) sz.x = avail.x;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild(id, ImVec2(sz.x, (sz.y > 0.0f) ? sz.y : 0.0f), ImGuiChildFlags_None, flags);

    if (g_stGlassPendingN < IM_ARRAYSIZE(g_stGlassPending)) {
        g_stGlassPending[g_stGlassPendingN].uid = ImGui::GetID(id);
        g_stGlassPending[g_stGlassPendingN].brackets = brackets;
        g_stGlassPendingN++;
    }
}

// Return: tinggi kartu aktual (utk menyusun kartu berikutnya).
static float EndGlassCard() {
    ImGui::EndChild();
    ImVec2 a = ImGui::GetItemRectMin();
    ImVec2 b = ImGui::GetItemRectMax();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    ImGuiID uid = 0;
    bool brackets = true;
    if (g_stGlassPendingN > 0) {
        g_stGlassPendingN--;
        uid = g_stGlassPending[g_stGlassPendingN].uid;
        brackets = g_stGlassPending[g_stGlassPendingN].brackets;
    }

    bool hovered = ImGui::IsMouseHoveringRect(a, b);
    float ht = AnimTowards(uid ^ 0x61A5u, hovered ? 1.0f : 0.0f, 8.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, hovered ? ST_GLASS_HOVER : ST_GLASS, 14.0f);
    dl->AddRect(a, b, StMix(ST_CYAN_LINE, ST_CYAN_EDGE, ht), 14.0f, 0, 1.0f);
    if (brackets) StDrawLBrackets(dl, a, b, ht);
    return b.y - a.y;
}

// Header kartu: pill badge cyan (kit) + kode kanan yg redup.
static void GlassCardHeader(const char* pillText, const char* rightText) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 ts = ImGui::CalcTextSize(pillText);
    float ph = ts.y + 6.0f;
    float pw = ts.x + 18.0f;
    ImVec2 pa = pos;
    ImVec2 pb = ImVec2(pos.x + pw, pos.y + ph);

    // Pill / Segmented Switcher (Variant 2): latar pill aktif pink gradient
    // dengan glow, seperti .st-navbar-pill pada kit Startorch.
    dl->AddRectFilled(ImVec2(pa.x - 2.0f, pa.y - 2.0f), ImVec2(pb.x + 2.0f, pb.y + 2.0f),
                      StAlpha(ST_PINK, 0.10f), ph * 0.5f + 2.0f);
    dl->AddRectFilled(pa, pb, StAlpha(ST_PINK, 0.18f), ph * 0.5f);
    dl->AddRect(pa, pb, StAlpha(ST_PINK, 0.45f), ph * 0.5f, 0, 1.0f);
    dl->AddText(ImVec2(pa.x + 9.0f, pa.y + 3.0f), ST_WHITE, pillText);

    if (rightText && rightText[0]) {
        ImVec2 rs = ImGui::CalcTextSize(rightText);
        dl->AddText(ImVec2(pos.x + w - rs.x, pos.y + 3.0f), ST_TEXT_DIM, rightText);
    }
    ImGui::Dummy(ImVec2(w, ph + 10.0f));
}

static void TextDim(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ST_TEXT_MUTED));
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

static std::string StEllipsize(const std::string& s, size_t maxChars) {
    if (s.size() <= maxChars || maxChars < 4) return s;
    return s.substr(0, maxChars - 3) + "...";
}

// ---- Telemetry metric tile (Startorch Variant 3: Telemetry Metric) ----
static void TelemetryTile(const char* id, const char* label, float pct01, const char* valueText, const char* unitText,
                          const char* statusText, ImU32 statusCol, const float* hist5, const char* subText, ImVec2 size) {
    pct01 = (std::min)(1.0f, (std::max)(0.0f, pct01));
    BeginGlassCard(id, size, 16.0f, 14.0f, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x;

    // Baris 1: label + status pill (kanan)
    {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        dl->AddText(pos, ST_TEXT_DIM, label);
        ImVec2 sts = ImGui::CalcTextSize(statusText);
        float ph = sts.y + 4.0f;
        ImVec2 pa = ImVec2(pos.x + w - (sts.x + 16.0f), pos.y - 1.0f);
        ImVec2 pb = ImVec2(pa.x + sts.x + 16.0f, pa.y + ph);
        dl->AddRectFilled(pa, pb, StAlpha(statusCol, 0.15f), ph * 0.5f);
        dl->AddRect(pa, pb, StAlpha(statusCol, 0.40f), ph * 0.5f, 0, 1.0f);
        dl->AddText(ImVec2(pa.x + 8.0f, pa.y + 2.0f), statusCol, statusText);
        ImGui::Dummy(ImVec2(w, ph + 4.0f));
    }

    // Baris 2: nilai besar + unit + mini sparkbar (riwayat 5 sampel)
    {
        ImVec2 vpos = ImGui::GetCursorScreenPos();
        ImFont* bigFont = g_fontSemibold ? g_fontSemibold : ImGui::GetFont();
        float bigSize = ImGui::GetFontSize() * 1.6f;
        dl->AddText(bigFont, bigSize, vpos, ST_WHITE, valueText);
        ImVec2 vsz = bigFont->CalcTextSizeA(bigSize, 10000.0f, 0.0f, valueText);
        if (unitText && unitText[0]) {
            float uSize = ImGui::GetFontSize() * 0.85f;
            ImVec2 usz = bigFont->CalcTextSizeA(uSize, 10000.0f, 0.0f, unitText);
            dl->AddText(bigFont, uSize, ImVec2(vpos.x + vsz.x + 4.0f, vpos.y + vsz.y - usz.y - 2.0f), ST_CYAN, unitText);
        }

        const float barW = 6.0f, barGap = 4.0f;
        float areaH = (std::max)(20.0f, vsz.y);
        float ox = vpos.x + w - (barW * 5.0f + barGap * 4.0f);
        for (int i = 0; i < 5; ++i) {
            float v01 = (std::min)(1.0f, (std::max)(0.0f, hist5 ? hist5[i] * 0.01f : 0.0f));
            float hh = 7.0f + v01 * (areaH - 8.0f);
            ImVec2 ba = ImVec2(ox + (float)i * (barW + barGap), vpos.y + areaH - hh);
            ImVec2 bb = ImVec2(ba.x + barW, vpos.y + areaH);
            ImU32 col = (i >= 3) ? ((i == 4) ? ST_PINK : StAlpha(ST_PINK, 0.80f))
                                 : StAlpha(ST_CYAN, (i == 1) ? 0.60f : 0.42f);
            if (i == 4) dl->AddRectFilled(ImVec2(ba.x - 2.5f, ba.y - 2.5f), ImVec2(bb.x + 2.5f, bb.y + 2.5f), StAlpha(ST_PINK, 0.18f), 4.0f);
            dl->AddRectFilled(ba, bb, col, 2.5f);
        }
        ImGui::Dummy(ImVec2(w, areaH + 2.0f));
    }

    // Baris 3: sub-info + bar starlume mini
    if (subText && subText[0]) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ST_TEXT_MUTED), "%s", subText);
    }
    {
        ImVec2 a = ImGui::GetCursorScreenPos();
        ImVec2 b = ImVec2(a.x + w, a.y + 6.0f);
        dl->AddRectFilled(a, b, ST_BG_BASE, 3.0f);
        float fx = a.x + (b.x - a.x) * pct01;
        if (fx > a.x + 2.0f) {
            StGradientStarlume(dl, a, ImVec2(fx, b.y), 3.0f);
            if (fx > a.x + 8.0f) {
                dl->PushClipRect(a, ImVec2(fx - 1.0f, b.y), true);
                float bw = (b.x - a.x) * 0.35f;
                float phase = fmodf((float)ImGui::GetTime() * 0.7f, 1.0f);
                float bx = a.x - bw + ((b.x - a.x) + 2.0f * bw) * phase;
                ImU32 peak = StAlpha(ST_WHITE, 0.28f);
                ImU32 none = StAlpha(ST_WHITE, 0.0f);
                dl->AddRectFilledMultiColor(ImVec2(bx, a.y), ImVec2(bx + bw * 0.5f, b.y), none, peak, peak, none);
                dl->AddRectFilledMultiColor(ImVec2(bx + bw * 0.5f, a.y), ImVec2(bx + bw, b.y), peak, none, none, peak);
                dl->PopClipRect();
            }
        }
        dl->AddRect(a, b, StAlpha(ST_CYAN, 0.22f), 3.0f, 0, 1.0f);
        ImGui::Dummy(ImVec2(w, 6.0f));
    }
    EndGlassCard();
}

static void RenderFloatingDock() {
    const float dockW = 320.0f;
    const float dockH = 56.0f;
    // Overlay absolut di atas konten + bayangan (seperti z-index di web).
    ImVec2 winPos = ImGui::GetWindowPos();
    ImVec2 winSize = ImGui::GetWindowSize();
    ImVec2 dockPos = ImVec2(winPos.x + (winSize.x - dockW) * 0.5f, winPos.y + winSize.y - dockH - 14.0f);
    DrawFloatingShadow(dockPos, ImVec2(dockPos.x + dockW, dockPos.y + dockH), 28.0f);
    ImGui::SetCursorScreenPos(dockPos);
    // Reserve bottom space so dock floats above content edge
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 28.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035f, 0.047f, 0.082f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.220f, 0.910f, 1.000f, 0.25f));
    ImGui::BeginChild("FloatingDock", ImVec2(dockW, dockH), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        static const char* icons[4] = { ICON_FA_IMAGES, ICON_FA_LOCK, ICON_FA_SLIDERS, ICON_FA_GAUGE_HIGH };
        static const char* tips[4] = { "Wallpaper Gallery", "Lock Screen & Static", "General Settings", "Performance & Advanced" };
        ImVec2 dockOrigin = ImGui::GetCursorScreenPos();
        float slotW = (dockW - 20.0f) / 4.0f;
        for (int i = 0; i < 4; ++i) {
            ImVec2 bp0 = ImVec2(dockOrigin.x + i * slotW + (slotW - 52.0f) * 0.5f, dockOrigin.y);
            bool active = (g_activePage == i);
            ImGui::SetCursorScreenPos(bp0);
            char btnId[32]; snprintf(btnId, sizeof(btnId), "##dock%d", i);
            if (HoloIconButton(btnId, icons[i], 52.0f, 40.0f, 16.0f, false, active, tips[i])) g_activePage = i;
        }
    }
    ImGui::EndChild();
    // Glass edge highlight di bibir atas dock
    {
        ImDrawList* gdl = ImGui::GetWindowDrawList();
        ImVec2 dMin = ImGui::GetItemRectMin(), dMax = ImGui::GetItemRectMax();
        gdl->AddLine(ImVec2(dMin.x + 26.0f, dMin.y + 1.5f), ImVec2(dMax.x - 26.0f, dMin.y + 1.5f), IM_COL32(255, 255, 255, 42), 1.0f);
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

static void RenderOriginalTooltip(const VideoProbeResult& probe, const char* size_str, const std::string& /*path*/) {
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
        ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_CIRCLE_INFO "  Original Video Details");
        if (g_fontSemibold) ImGui::PopFont();
        ImGui::Separator();
        if (probe.valid) {
            const char* resCategory = (probe.width >= 3840) ? "4K Ultra HD" :
                                      (probe.width >= 2560) ? "1440p Quad HD" :
                                      (probe.width >= 1920) ? "1080p Full HD" : "Standard HD";
            ImGui::Text("Resolution : %d x %d (%s)", probe.width, probe.height, resCategory);
            ImGui::Text("Frame Rate : %.1f FPS", probe.fps);
            ImGui::Text("Codec      : %s", probe.codec_name.empty() ? "H.264 / AVC" : probe.codec_name.c_str());
            if (probe.duration > 0.0) {
                int totalSec = static_cast<int>(probe.duration);
                int mm = totalSec / 60;
                int ss = totalSec % 60;
                ImGui::Text("Duration   : %02d:%02d", mm, ss);
            }
        } else {
            ImGui::Text("Resolution : Probing on playback...");
        }
        if (size_str && size_str[0] != '\0') {
            ImGui::Text("File Size  : %s", size_str);
        }
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.80f, 1.00f), "Direct playback of original source with hardware acceleration.");
        ImGui::EndTooltip();
    }
}

static void RenderGalleryDetailPanel(const std::string& path, const std::string& filename, const VideoProbeResult& probe, const char* size_str, int target_w, int target_h, const std::string& opt_path, bool has_opt, bool is_already_optimal, bool is_playing_opt, bool is_playing_ori, bool is_current, float detailH) {
    auto& cfg = g_config.Get();
    BeginModuleCard("GalleryDetail", ImVec2(312, detailH), 18.0f, ST_CARD_NORMAL, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    {
        // Header row: title + close
        if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
        ImGui::TextColored(ImVec4(0.945f, 0.961f, 0.976f, 1.0f), ICON_FA_CIRCLE_INFO "  Detail Video");
        if (g_fontSemibold) ImGui::PopFont();
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 38.0f);
        if (HoloButtonSmall(ICON_FA_XMARK)) { g_detailVisible = false; }
        ImGui::Separator();
        ImGui::Spacing();

        // Large preview 16:9
        float pvW = ImGui::GetContentRegionAvail().x;
        float pvH = pvW * 9.0f / 16.0f;
        ImVec2 pp0 = ImGui::GetCursorScreenPos();
        ImVec2 pp1 = ImVec2(pp0.x + pvW, pp0.y + pvH);
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        ID3D11ShaderResourceView* big_srv = ThumbnailManager::Instance().GetThumbnailSRV(g_pd3dDevice, path);
        if (big_srv) {
            pdl->AddImageRounded((ImTextureID)big_srv, pp0, pp1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 12.0f);
        } else {
            pdl->AddRectFilled(pp0, pp1, IM_COL32(20, 22, 28, 255), 12.0f);
            pdl->AddRect(pp0, pp1, IM_COL32(45, 50, 62, 255), 12.0f);
            const char* lt = "Loading preview...";
            ImVec2 lts = ImGui::CalcTextSize(lt);
            pdl->AddText(ImVec2(pp0.x + (pvW - lts.x) * 0.5f, pp0.y + (pvH - lts.y) * 0.5f), IM_COL32(140, 145, 160, 255), lt);
        }
        pdl->AddRect(pp0, pp1, IM_COL32(56, 232, 255, 46), 12.0f);
        if (is_current) {
            // Active glow border (gold = sedang dipakai)
            pdl->AddRect(pp0, pp1, IM_COL32(255, 209, 102, 220), 12.0f, 0, 2.0f);
        }
        ImGui::Dummy(ImVec2(pvW, pvH + 4.0f));

        // Badge
        std::string badgeText; ImU32 bBg, bFg;
        if (is_current) {
            if (is_playing_opt) { badgeText = ICON_FA_BOLT "  ACTIVE (OPTIMIZED)"; bBg = IM_COL32(255, 209, 102, 38); bFg = IM_COL32(255, 209, 102, 255); }
            else { badgeText = ICON_FA_CIRCLE_PLAY "  ACTIVE (ORIGINAL)"; bBg = IM_COL32(56, 232, 255, 34); bFg = IM_COL32(56, 232, 255, 255); }
        } else if (has_opt) { badgeText = ICON_FA_CHECK "  Optimized Ready"; bBg = IM_COL32(56, 232, 255, 26); bFg = IM_COL32(140, 220, 245, 255); }
        else if (is_already_optimal) { badgeText = "1080p Optimal"; bBg = IM_COL32(255, 209, 102, 26); bFg = IM_COL32(255, 209, 102, 235); }
        else { badgeText = ICON_FA_FILM "  Video File"; bBg = IM_COL32(255, 255, 255, 16); bFg = IM_COL32(148, 163, 184, 255); }
        ImVec2 btsz = ImGui::CalcTextSize(badgeText.c_str());
        ImVec2 bcur = ImGui::GetCursorScreenPos();
        pdl->AddRectFilled(bcur, ImVec2(bcur.x + btsz.x + 18.0f, bcur.y + btsz.y + 8.0f), bBg, 8.0f);
        pdl->AddText(ImVec2(bcur.x + 9.0f, bcur.y + 4.0f), bFg, badgeText.c_str());
        ImGui::Dummy(ImVec2(btsz.x + 18.0f, btsz.y + 10.0f));

        // Filename (wrapped, 2 lines max feel)
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(filename.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nPath: %s", filename.c_str(), path.c_str());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(ImVec4(0.50f, 0.55f, 0.64f, 1.0f), "%s", path.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        // Metadata box
        int metaLines = probe.valid ? 3 : 1;
        if (size_str && size_str[0] != '\0') metaLines++;
        float metaH = metaLines * ImGui::GetTextLineHeightWithSpacing() + 12.0f * 2.0f;
        BeginModuleCard("DetailMeta", ImVec2(0, metaH), 10.0f, ST_CARD_NORMAL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, 12.0f, 12.0f);
        if (probe.valid) {
            ImGui::Text("Resolusi  : %d x %d", probe.width, probe.height);
            ImGui::Text("Framerate : %.0f FPS", probe.fps);
            ImGui::Text("Codec     : %s", probe.codec_name.empty() ? "H.264 / AVC" : probe.codec_name.c_str());
            if (size_str && size_str[0]) ImGui::Text("Ukuran    : %s", size_str);
        } else {
            ImGui::TextColored(ImVec4(0.58f, 0.64f, 0.72f, 1.0f), "Probing video info...");
            if (size_str && size_str[0]) ImGui::Text("Ukuran : %s", size_str);
        }
        EndModuleCard();
        ImGui::Spacing();

        float fullW = ImGui::GetContentRegionAvail().x;
        if (is_current) {
            if (HoloDangerButton(ICON_FA_STOP "  Stop Wallpaper", ImVec2(fullW, 0))) ApplyAction("", "stop");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop video wallpaper and return to Windows static desktop");
            ImGui::Spacing();
            if (is_playing_opt) {
                if (HoloButton(ICON_FA_PLAY "  Play Original", ImVec2(fullW, 0))) ApplyAction(path, "wallpaper");
                RenderOriginalTooltip(probe, size_str, path);
            } else {
                if (has_opt) {
                    if (HoloButton(ICON_FA_BOLT "  Switch to Optimized", ImVec2(fullW, 0))) ApplyAction(opt_path, "wallpaper");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Switch to pre-rendered optimized version to save ~70% GPU");
                } else if (!is_already_optimal) {
                    if (HoloButton(ICON_FA_DOWNLOAD "  Optimize Video", ImVec2(fullW, 0))) StartVideoOptimization(path, target_w, target_h, "wallpaper", cfg.optimizer_crop_mode);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pre-render hardware-optimized 1080p copy (Low GPU load)");
                }
            }
        } else {
            if (has_opt) {
                if (HoloButton(ICON_FA_BOLT "  Play Optimized", ImVec2(fullW, 0))) ApplyAction(opt_path, "wallpaper");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play pre-rendered optimized version (Low GPU usage)");
                if (HoloButton(ICON_FA_PLAY "  Play Original", ImVec2(fullW, 0))) ApplyAction(path, "wallpaper");
                RenderOriginalTooltip(probe, size_str, path);
            } else if (is_already_optimal) {
                if (HoloButton(ICON_FA_PLAY "  Play Wallpaper", ImVec2(fullW, 0))) RequestApplyVideo(path, "wallpaper");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play 1080p native video directly");
            } else {
                if (HoloButton(ICON_FA_PLAY "  Play Original", ImVec2(fullW, 0))) RequestApplyVideo(path, "wallpaper");
                RenderOriginalTooltip(probe, size_str, path);
                if (HoloButton(ICON_FA_DOWNLOAD "  Optimize Video", ImVec2(fullW, 0))) StartVideoOptimization(path, target_w, target_h, "wallpaper", cfg.optimizer_crop_mode);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pre-render hardware-optimized 1080p copy (Low GPU load)");
            }
        }
        ImGui::Spacing();
        if (HoloButton(ICON_FA_CAMERA "  Capture Frame", ImVec2(fullW, 0))) OpenCaptureModal(path);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
            ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_CAMERA "  Capture Frame (Visual Wallpaper)");
            if (g_fontSemibold) ImGui::PopFont();
            ImGui::Separator();
            ImGui::Text("Ambil gambar frame statis dari video ini untuk:");
            ImGui::BulletText("Desktop Wallpaper (0s instant boot visual)");
            ImGui::BulletText("Windows Lock Screen (Win+L seamless transition)");
            ImGui::EndTooltip();
        }
        if (HoloDangerButton(ICON_FA_TRASH_CAN "  Remove from Gallery", ImVec2(fullW, 0))) {
            bool wasCur = is_current;
            if (wasCur) ApplyAction("", "stop");
            ThumbnailManager::DeleteThumbnailCache(path);
            VideoOptimizer::DeleteOptimizedCache(path);
            cfg.RemoveFromGallery(path);
            g_config.Save();
            g_detailVisible = false;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove video from gallery");
        ImGui::Dummy(ImVec2(1.0f, 56.0f));
    }
    EndModuleCard();
}

// Pill aksi (tambah/refresh/hapus) melayang DI ATAS cards gallery — digambar terakhir biar paling atas.
static void RenderSidePillOverlay(ImVec2 rowTop, float rowH) {
    auto& cfg = g_config.Get();
    const float sideBtn = 40.0f;
    const float sideGap = 8.0f;
    const float sidePillH = sideBtn * 3.0f + sideGap * 2.0f + 16.0f;
    float sidePillY = (rowH - sidePillH) * 0.5f;
    if (sidePillY < 4.0f) sidePillY = 4.0f;
    ImVec2 pillPos = ImVec2(rowTop.x + 12.0f, rowTop.y + sidePillY);
    DrawFloatingShadow(pillPos, ImVec2(pillPos.x + 48.0f, pillPos.y + sidePillH), 24.0f);
    ImGui::SetCursorScreenPos(pillPos);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 24.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035f, 0.047f, 0.082f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.220f, 0.910f, 1.000f, 0.25f));
    ImGui::BeginChild("SideMenuPill", ImVec2(48, sidePillH), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImGui::PushID("SideMenu");
        ImGui::SetWindowFontScale(1.30f);
        if (HoloIconButton("##pill_add", ICON_FA_PLUS, sideBtn, sideBtn, 13.0f, false, false, "Tambah video")) {
            wchar_t filename[MAX_PATH] = L"";
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = g_hWnd;
            ofn.lpstrFilter = L"Video Files (*.mp4;*.webm;*.mkv;*.avi;*.mov)\0*.mp4;*.webm;*.mkv;*.avi;*.mov\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = filename;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

            if (GetOpenFileNameW(&ofn)) {
                int size_needed = WideCharToMultiByte(CP_UTF8, 0, filename, -1, NULL, 0, NULL, NULL);
                std::string utf8_path(size_needed - 1, 0);
                WideCharToMultiByte(CP_UTF8, 0, filename, -1, &utf8_path[0], size_needed, NULL, NULL);
                RequestApplyVideo(utf8_path, "wallpaper");
            }
        }
        if (HoloIconButton("##pill_refresh", ICON_FA_ROTATE, sideBtn, sideBtn, 13.0f, false, false, "Refresh galeri")) {
            g_config.Load();
        }
        if (HoloIconButton("##pill_clear", ICON_FA_TRASH_CAN, sideBtn, sideBtn, 13.0f, true, false, "Hapus semua video")) {
            for (const auto& pp2 : cfg.gallery_history) {
                ThumbnailManager::DeleteThumbnailCache(pp2);
                VideoOptimizer::DeleteOptimizedCache(pp2);
            }
            cfg.gallery_history.clear();
            g_config.Save();
        }
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopID();
    }
    ImGui::EndChild();
    // Glass edge highlight di bibir atas pill
    {
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        ImVec2 pmn = ImGui::GetItemRectMin(), pmx = ImGui::GetItemRectMax();
        pdl->AddLine(ImVec2(pmn.x + 14.0f, pmn.y + 1.5f), ImVec2(pmx.x - 14.0f, pmn.y + 1.5f), IM_COL32(56, 232, 255, 46), 1.0f);
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

static void RenderGalleryTab() {
    auto& cfg = g_config.Get();
    auto galleryCopy = cfg.gallery_history;

    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    if (screen_w <= 0) screen_w = 1920;
    if (screen_h <= 0) screen_h = 1080;

    // Galeri tanpa header (judul & status ada di title bar; hint drop muncul sbg overlay saat drag file).

    // Toolbar tambah/refresh/hapus ada di pill vertikal mengambang sisi kiri.

    // Multi-Monitor Target Checkboxes in Gallery
    if (!g_hardwareInfoInit) {
        g_hardwareInfo = HardwareDetector::QuerySystemInfo();
        g_hardwareInfoInit = true;
    }

    if (g_hardwareInfo.displays.size() > 1) {
        BeginModuleCard("MultiMonitorBanner", ImVec2(0, 54.0f), 14.0f, ST_CARD_NORMAL, ImGuiWindowFlags_NoScrollbar, 16.0f, 12.0f);

        if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.945f, 0.961f, 0.976f, 1.0f), ICON_FA_TV " Target Desktops:");
        if (g_fontSemibold) ImGui::PopFont();
        ImGui::SameLine(0, 16.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);

        bool all_checked = cfg.target_displays.empty() || (cfg.target_displays.size() == g_hardwareInfo.displays.size());
        if (ToggleSwitch("All Monitors", &all_checked)) {
            if (all_checked) {
                cfg.target_displays.clear();
            } else {
                cfg.target_displays = { 0 };
            }
            g_config.Save();
            nlohmann::json req{{"cmd", "set_target_displays"}, {"displays", cfg.target_displays}};
            SendIpcAsync(req.dump());
        }

        for (size_t d = 0; d < g_hardwareInfo.displays.size(); ++d) {
            ImGui::SameLine(0, 16.0f);
            bool is_d_checked = cfg.IsDisplayEnabled(static_cast<int>(d));
            std::string disp_label = "Display " + std::to_string(d + 1) + (g_hardwareInfo.displays[d].is_primary ? " (Primary)" : "");
            if (ToggleSwitch(disp_label.c_str(), &is_d_checked)) {
                cfg.SetDisplayEnabled(static_cast<int>(d), is_d_checked);
                g_config.Save();
                nlohmann::json req{{"cmd", "set_target_displays"}, {"displays", cfg.target_displays}};
                SendIpcAsync(req.dump());
            }
        }

        EndModuleCard();
        ImGui::Spacing();
    }

    bool isGalleryEmpty = galleryCopy.empty();

    // Clamp selection
    if (!isGalleryEmpty && (g_selectedGalleryIdx < 0 || g_selectedGalleryIdx >= (int)galleryCopy.size())) {
        // Auto-select currently playing video if any
        g_selectedGalleryIdx = -1;
        for (size_t k = 0; k < galleryCopy.size(); ++k) {
            const std::string& pp = galleryCopy[k];
            if (!g_daemonCurrentVideo.empty() && (g_daemonCurrentVideo == pp)) { g_selectedGalleryIdx = (int)k; break; }
        }
        if (g_selectedGalleryIdx < 0 && !cfg.wallpapers.empty()) {
            for (size_t k = 0; k < galleryCopy.size(); ++k) {
                if (cfg.wallpapers[0].video_path == galleryCopy[k]) { g_selectedGalleryIdx = (int)k; break; }
            }
        }
    }
    bool showDetail = !isGalleryEmpty && g_detailVisible && g_selectedGalleryIdx >= 0 && g_selectedGalleryIdx < (int)galleryCopy.size();

    float totalAvailW = ImGui::GetContentRegionAvail().x;
    float gridAvailW = totalAvailW - (showDetail ? (312.0f + 12.0f) : 0.0f);
    if (gridAvailW < 200.0f) gridAvailW = 200.0f;
    float gridH = ImGui::GetContentRegionAvail().y - 4.0f;
    if (gridH < 160.0f) gridH = 160.0f;

    ImVec2 gridRowTop = ImGui::GetCursorScreenPos();

    if (isGalleryEmpty) {
        ImGui::SetCursorScreenPos(ImVec2(gridRowTop.x, gridRowTop.y));
        BeginModuleCard("GalleryEmpty", ImVec2(gridAvailW, gridH), 18.0f);
        ImVec2 ec = ImGui::GetCursorScreenPos();
        ImVec2 esz = ImGui::GetContentRegionAvail();
        const char* et = "Belum ada video - klik + atau drag & drop ke sini";
        ImVec2 ets = ImGui::CalcTextSize(et);
        ImGui::SetCursorScreenPos(ImVec2(ec.x + (esz.x - ets.x) * 0.5f, ec.y + (esz.y - ets.y) * 0.5f));
        ImGui::TextColored(ImVec4(0.580f, 0.639f, 0.722f, 1.0f), "%s", et);
        EndModuleCard();
        RenderSidePillOverlay(gridRowTop, gridH);
        return;
    }

    ImGui::SetCursorScreenPos(gridRowTop);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("GalleryGrid", ImVec2(gridAvailW, gridH), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    float availW = ImGui::GetContentRegionAvail().x;
    float cardMinW = 230.0f;
    float spacingX = 14.0f;
    float spacingY = 14.0f;
    int numCols = (int)((availW + spacingX) / (cardMinW + spacingX));
    if (numCols < 1) numCols = 1;
    if (numCols > 4) numCols = 4;
    float cardWidth = (availW - spacingX * (numCols - 1)) / numCols;
    if (cardWidth < 180.0f) cardWidth = availW;
    float thumbH = cardWidth * 9.0f / 16.0f;
    float footerH = 62.0f;
    float cardHeight = thumbH + footerH + 20.0f;

    for (size_t i = 0; i < galleryCopy.size(); ++i) {
        const auto& path = galleryCopy[i];
        fs::path p(path);
        std::string filename = p.filename().string();
        if (filename.empty()) filename = path;

        int target_w = screen_w;
        int target_h = screen_h;
        auto probe = VideoOptimizer::Probe(path);
        if (probe.valid) {
            auto [pw, ph] = VideoOptimizer::CalculateTargetDimensions(probe.width, probe.height, screen_w, screen_h, cfg.optimizer_crop_mode);
            target_w = pw;
            target_h = ph;
        }

        std::string opt_path = VideoOptimizer::GetOptimizedPath(path, target_w, target_h);
        bool has_opt = VideoOptimizer::HasOptimizedCache(path, target_w, target_h);
        bool is_already_optimal = (probe.valid && probe.width <= screen_w && probe.height <= screen_h && probe.fps <= 60.5);

        bool is_playing_opt = (!g_daemonCurrentVideo.empty() && g_daemonCurrentVideo == opt_path);
        bool is_playing_ori = (!g_daemonCurrentVideo.empty() && g_daemonCurrentVideo == path);
        bool is_current = is_playing_opt || is_playing_ori ||
                          (!cfg.wallpapers.empty() && (cfg.wallpapers[0].video_path == path || cfg.wallpapers[0].video_path == opt_path));

        // Format file size and metadata summary line
        char size_str[32] = {};
        std::error_code fsec;
        if (fs::exists(path, fsec)) {
            uintmax_t bytes = fs::file_size(path, fsec);
            if (bytes >= 1024 * 1024 * 1024) {
                snprintf(size_str, sizeof(size_str), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
            } else if (bytes >= 1024 * 1024) {
                snprintf(size_str, sizeof(size_str), "%.1f MB", bytes / (1024.0 * 1024.0));
            } else if (bytes > 0) {
                snprintf(size_str, sizeof(size_str), "%.0f KB", bytes / 1024.0);
            }
        }

        std::string meta_line;
        if (probe.valid) {
            char meta_buf[128];
            snprintf(meta_buf, sizeof(meta_buf), "%dx%d • %.0f FPS • %s",
                     probe.width, probe.height, probe.fps,
                     probe.codec_name.empty() ? "video" : probe.codec_name.c_str());
            meta_line = meta_buf;
            if (size_str[0] != '\0') {
                meta_line += " • ";
                meta_line += size_str;
            }
        }

        ImGui::PushID(static_cast<int>(i));
        bool is_selected = (g_selectedGalleryIdx == (int)i);
        int cardState = is_current ? ST_CARD_ACTIVE : ((is_selected && showDetail) ? ST_CARD_SELECTED : ST_CARD_NORMAL);

        BeginModuleCard("VCard", ImVec2(cardWidth, cardHeight), 14.0f, cardState, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, 0.0f, 0.0f);
        {
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 cardPos = ImGui::GetCursorScreenPos();
            float innerW = cardWidth;
            // Thumbnail top (full-bleed)
            ImVec2 t0 = ImVec2(cardPos.x + 6.0f, cardPos.y + 6.0f);
            ImVec2 t1 = ImVec2(cardPos.x + innerW - 10.0f, cardPos.y + 6.0f + thumbH);
            ID3D11ShaderResourceView* thumb_srv = ThumbnailManager::Instance().GetThumbnailSRV(g_pd3dDevice, path);
            if (thumb_srv) {
                draw_list->AddImageRounded((ImTextureID)thumb_srv, t0, t1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 10.0f);
            } else {
                draw_list->AddRectFilled(t0, t1, IM_COL32(17, 23, 38, 255), 10.0f);
                draw_list->AddRect(t0, t1, IM_COL32(56, 232, 255, 40), 10.0f);
                const char* lt = "Loading...";
                ImVec2 lts = ImGui::CalcTextSize(lt);
                draw_list->AddText(ImVec2(t0.x + (innerW - 16.0f - lts.x) * 0.5f, t0.y + (thumbH - lts.y) * 0.5f), IM_COL32(140, 152, 168, 255), lt);
            }
            draw_list->AddRect(t0, t1, IM_COL32(56, 232, 255, 40), 10.0f);
            if (is_current) draw_list->AddRect(t0, t1, IM_COL32(255, 209, 102, 210), 10.0f, 0, 1.6f);
            // Play overlay icon on hover/current
            ImVec2 hovMin = t0, hovMax = t1;
            bool thumbHovered = ImGui::IsMouseHoveringRect(hovMin, hovMax);
            if (is_current || thumbHovered) {
                ImVec2 cc = ImVec2((t0.x + t1.x) * 0.5f, (t0.y + t1.y) * 0.5f);
                draw_list->AddCircleFilled(cc, 20.0f, IM_COL32(9, 12, 21, 178));
                draw_list->AddCircle(cc, 20.0f, IM_COL32(56, 232, 255, 90), 0, 1.2f);
                const char* pic = is_current ? ICON_FA_PAUSE : ICON_FA_PLAY;
                ImVec2 psz = ImGui::CalcTextSize(pic);
                draw_list->AddText(ImVec2(cc.x - psz.x * 0.5f, cc.y - psz.y * 0.5f), is_current ? IM_COL32(255, 209, 102, 255) : IM_COL32(241, 245, 249, 255), pic);
            }
            ImGui::Dummy(ImVec2(innerW, thumbH + 12.0f));

            // Footer: name + version badge
            ImGui::SetCursorPosX(12.0f);
            ImGui::BeginGroup();
            float footW = innerW - 24.0f;
            std::string verBadge;
            ImU32 verBg, verFg;
            if (is_playing_opt || (is_current && has_opt)) { verBadge = "OPTIMIZED"; verBg = IM_COL32(56, 232, 255, 30); verFg = IM_COL32(56, 232, 255, 255); }
            else if (has_opt) { verBadge = "ORI + OPT"; verBg = IM_COL32(255, 255, 255, 20); verFg = IM_COL32(241, 245, 249, 220); }
            else if (is_already_optimal) { verBadge = "ORI - Optimal"; verBg = IM_COL32(255, 209, 102, 28); verFg = IM_COL32(255, 209, 102, 235); }
            else { verBadge = "ORIGINAL"; verBg = IM_COL32(255, 255, 255, 14); verFg = IM_COL32(148, 163, 184, 255); }
            if (is_current) { verBadge = std::string(ICON_FA_CIRCLE_PLAY "  ") + verBadge; }

            std::string titleStr = filename;
            ImVec2 verSz = ImGui::CalcTextSize(verBadge.c_str());
            float verW = verSz.x + 16.0f;
            float maxTitleW = footW - verW - 10.0f;
            if (maxTitleW < 60.0f) maxTitleW = 60.0f;
            while (titleStr.length() > 6 && ImGui::CalcTextSize((titleStr + "...").c_str()).x > maxTitleW) titleStr.pop_back();
            if (titleStr != filename) titleStr += "...";

            ImVec2 fpos = ImGui::GetCursorScreenPos();
            ImGui::TextUnformatted(titleStr.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nPath: %s", filename.c_str(), path.c_str());
            ImVec2 bp0 = ImVec2(fpos.x + footW - verW, fpos.y);
            ImVec2 bp1 = ImVec2(bp0.x + verW, bp0.y + verSz.y + 6.0f);
            draw_list->AddRectFilled(bp0, bp1, verBg, 6.0f);
            draw_list->AddText(ImVec2(bp0.x + 8.0f, bp0.y + 3.0f), verFg, verBadge.c_str());

            if (!meta_line.empty()) ImGui::TextColored(ImVec4(0.580f, 0.639f, 0.722f, 1.0f), "%s", meta_line.c_str());
            else ImGui::TextColored(ImVec4(0.520f, 0.575f, 0.660f, 1.0f), "Video source ready");
            ImGui::EndGroup();
        }
        EndModuleCard();
        // Click handling (select + toggle detail)
        bool cardHovered = ImGui::IsItemHovered();
        if (cardHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (g_selectedGalleryIdx == (int)i && g_detailVisible) g_detailVisible = false;
            else { g_selectedGalleryIdx = (int)i; g_detailVisible = true; }
        }
        if (cardHovered) ImGui::SetTooltip("Klik untuk %s detail", (g_detailVisible && is_selected) ? "tutup" : "buka");
        ImGui::PopID();

        // Responsive vertical-card grid
        bool isLastInRow = ((i + 1) % numCols == 0) || (i + 1 >= galleryCopy.size());
        if (!isLastInRow) {
            ImGui::SameLine(0, spacingX);
        } else {
            ImGui::Dummy(ImVec2(1.0f, spacingY));
        }
    }

    // Ruang bawah agar baris terakhir bisa scroll ke atas dock yg melayang
    ImGui::Dummy(ImVec2(1.0f, 84.0f));

    ImGui::EndChild();
    ImGui::PopStyleColor();

    if (showDetail) {
        // Sejajarkan top panel detail dgn top grid (SameLine menempelkannya di bawah grid sehingga kepotong)
        ImGui::SetCursorScreenPos(ImVec2(gridRowTop.x + gridAvailW + 12.0f, gridRowTop.y));
        // Resolve selected video data for detail panel (same logic as cards)
        const std::string& selPath = galleryCopy[(size_t)g_selectedGalleryIdx];
        fs::path selFs(selPath);
        std::string selName = selFs.filename().string();
        if (selName.empty()) selName = selPath;
        auto selProbe = VideoOptimizer::Probe(selPath);
        int selTW = screen_w, selTH = screen_h;
        if (selProbe.valid) {
            auto dims = VideoOptimizer::CalculateTargetDimensions(selProbe.width, selProbe.height, screen_w, screen_h, cfg.optimizer_crop_mode);
            selTW = dims.first; selTH = dims.second;
        }
        std::string selOpt = VideoOptimizer::GetOptimizedPath(selPath, selTW, selTH);
        bool selHasOpt = VideoOptimizer::HasOptimizedCache(selPath, selTW, selTH);
        bool selOptimal = (selProbe.valid && selProbe.width <= screen_w && selProbe.height <= screen_h && selProbe.fps <= 60.5);
        bool selPlayOpt = (!g_daemonCurrentVideo.empty() && g_daemonCurrentVideo == selOpt);
        bool selPlayOri = (!g_daemonCurrentVideo.empty() && g_daemonCurrentVideo == selPath);
        bool selCur = selPlayOpt || selPlayOri ||
                      (!cfg.wallpapers.empty() && (cfg.wallpapers[0].video_path == selPath || cfg.wallpapers[0].video_path == selOpt));
        char selSize[32] = {};
        std::error_code selEc;
        if (fs::exists(selPath, selEc)) {
            uintmax_t b = fs::file_size(selPath, selEc);
            if (b >= 1024ull * 1024ull * 1024ull) snprintf(selSize, sizeof(selSize), "%.1f GB", b / (1024.0 * 1024.0 * 1024.0));
            else if (b >= 1024ull * 1024ull) snprintf(selSize, sizeof(selSize), "%.1f MB", b / (1024.0 * 1024.0));
            else if (b > 0) snprintf(selSize, sizeof(selSize), "%.0f KB", b / 1024.0);
        }
        RenderGalleryDetailPanel(selPath, selName, selProbe, selSize, selTW, selTH, selOpt, selHasOpt, selOptimal, selPlayOpt, selPlayOri, selCur, gridH);
    }
    // Pill aksi overlay paling atas (di atas cards).
    RenderSidePillOverlay(gridRowTop, gridH);
}

// Judul halaman gaya LuaTools: tebal besar + deskripsi abu-abu.
static void RenderPageTitle(const char* icon, const char* title, const char* desc) {
    if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
    ImGui::SetWindowFontScale(1.4f);
    ImGui::TextColored(ImVec4(0.96f, 0.97f, 0.99f, 1.0f), "%s  %s", icon, title);
    ImGui::SetWindowFontScale(1.0f);
    if (g_fontSemibold) ImGui::PopFont();
    if (desc && desc[0] != '\0')
        ImGui::TextColored(ImVec4(0.63f, 0.67f, 0.75f, 1.0f), "%s", desc);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}

static void RenderLockscreenTab() {
    auto& cfg = g_config.Get();

    RenderPageTitle(ICON_FA_LOCK, "Lock Screen & Static", "Configure instant 0s Windows boot visuals and lock screen transitions with zero continuous CPU load.");

    // CARD 1: Windows Desktop Static Wallpaper (0s Instant Boot Visual)
    BeginModuleCard("DesktopStaticCard", ImVec2(0, 155), 18.0f, ST_CARD_NORMAL, ImGuiWindowFlags_NoScrollbar);

    if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
    ImGui::TextColored(ImVec4(0.945f, 0.961f, 0.976f, 1.0f), ICON_FA_DESKTOP "  1. Windows Desktop Static Wallpaper (0s Instant Boot Visual)");
    if (g_fontSemibold) ImGui::PopFont();
    ImGui::Spacing();

    if (ToggleSwitch("Enable Windows Desktop Static Wallpaper", &cfg.update_desktop_wallpaper)) {
        g_config.Save();
        if (cfg.update_desktop_wallpaper) {
            if (cfg.desktop_static_source_mode == 1 && !cfg.desktop_static_video_path.empty()) {
                nlohmann::json req{
                    {"cmd", "sync_desktop_wallpaper"},
                    {"path", cfg.desktop_static_video_path},
                    {"desktop", true},
                    {"lockscreen", false},
                    {"timestamp", cfg.desktop_static_timestamp}
                };
                SendIpcAsync(req.dump());
            } else {
                SendIpcAsync("{\"cmd\":\"sync_desktop_wallpaper\",\"desktop\":true,\"lockscreen\":false}");
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Injects a high-definition static frame directly into Windows Desktop Wallpaper.\nWhen Windows boots, your desktop appears in 0.0s with no black screen or loading delay.");
    }

    if (cfg.update_desktop_wallpaper) {
        ImGui::Indent(18.0f);
        if (cfg.desktop_static_source_mode == 0) {
            ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.0f), ICON_FA_ROTATE " Active Mode: Auto-Sync (Dynamically follows active live wallpaper)");
        } else {
            fs::path p(cfg.desktop_static_video_path);
            std::string vidName = p.filename().string();
            if (vidName.empty()) vidName = cfg.desktop_static_video_path;
            int curMin = static_cast<int>(cfg.desktop_static_timestamp) / 60;
            float curSec = static_cast<float>(cfg.desktop_static_timestamp) - curMin * 60.0f;
            ImGui::TextColored(ImVec4(1.00f, 0.82f, 0.40f, 1.0f), ICON_FA_IMAGE " Active Mode: Custom Frame [%s @ %02d:%05.2f]", vidName.c_str(), curMin, curSec);

            ImGui::SameLine();
            if (HoloButtonSmall("Reset to Auto-Sync")) {
                cfg.desktop_static_source_mode = 0;
                g_config.Save();
                SendIpcAsync("{\"cmd\":\"sync_desktop_wallpaper\",\"reset_desktop_auto\":true}");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Switch back to automatically updating desktop wallpaper with whichever live video is currently playing");
            }
        }

        std::string cur_vid = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : g_daemonCurrentVideo;
        if (cfg.desktop_static_source_mode == 1 && !cfg.desktop_static_video_path.empty()) {
            cur_vid = cfg.desktop_static_video_path;
        }
        if (cur_vid.empty() && !cfg.gallery_history.empty()) {
            cur_vid = cfg.gallery_history[0];
        }

        if (HoloButton(ICON_FA_CAMERA "  Choose Custom Frame for Desktop...", ImVec2(320, 0))) {
            if (!cur_vid.empty()) {
                OpenCaptureModal(cur_vid, 1);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Select a specific video and exact timestamp/frame specifically for the Desktop Wallpaper");
        }
        ImGui::Unindent(18.0f);
    } else {
        ImGui::TextDisabled("  Desktop static wallpaper injection is currently disabled.");
    }

    EndModuleCard();
    ImGui::Spacing();

    // CARD 2: Windows Lock Screen Wallpaper (Win + L Seamless Transition)
    BeginModuleCard("LockscreenCard", ImVec2(0, 155), 18.0f, ST_CARD_NORMAL, ImGuiWindowFlags_NoScrollbar);

    if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
    ImGui::TextColored(ImVec4(0.945f, 0.961f, 0.976f, 1.0f), ICON_FA_LOCK "  2. Windows Lock Screen Wallpaper (Win + L Seamless Transition)");
    if (g_fontSemibold) ImGui::PopFont();
    ImGui::Spacing();

    if (ToggleSwitch("Enable Windows Lock Screen Wallpaper", &cfg.update_lockscreen)) {
        g_config.Save();
        if (cfg.update_lockscreen) {
            if (cfg.lockscreen_source_mode == 1 && !cfg.lockscreen_video_path.empty()) {
                nlohmann::json req{
                    {"cmd", "sync_desktop_wallpaper"},
                    {"path", cfg.lockscreen_video_path},
                    {"desktop", false},
                    {"lockscreen", true},
                    {"timestamp", cfg.lockscreen_timestamp}
                };
                SendIpcAsync(req.dump());
            } else {
                SendIpcAsync("{\"cmd\":\"sync_desktop_wallpaper\",\"desktop\":false,\"lockscreen\":true}");
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pre-caches a pristine high-definition frame for the Windows Lock Screen.\nWhen pressing Win+L, it displays immediately with 0ms visual delay and 0% background load.");
    }

    if (cfg.update_lockscreen) {
        ImGui::Indent(18.0f);
        if (cfg.lockscreen_source_mode == 0) {
            ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.0f), ICON_FA_ROTATE " Active Mode: Auto-Sync (Dynamically follows active live wallpaper)");
        } else {
            fs::path p(cfg.lockscreen_video_path);
            std::string vidName = p.filename().string();
            if (vidName.empty()) vidName = cfg.lockscreen_video_path;
            int curMin = static_cast<int>(cfg.lockscreen_timestamp) / 60;
            float curSec = static_cast<float>(cfg.lockscreen_timestamp) - curMin * 60.0f;
            ImGui::TextColored(ImVec4(1.00f, 0.82f, 0.40f, 1.0f), ICON_FA_IMAGE " Active Mode: Custom Frame [%s @ %02d:%05.2f]", vidName.c_str(), curMin, curSec);

            ImGui::SameLine();
            if (HoloButtonSmall("Reset to Auto-Sync")) {
                cfg.lockscreen_source_mode = 0;
                g_config.Save();
                SendIpcAsync("{\"cmd\":\"sync_desktop_wallpaper\",\"reset_lockscreen_auto\":true}");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Switch back to automatically updating lock screen wallpaper with whichever live video is currently playing");
            }
        }

        std::string cur_vid = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : g_daemonCurrentVideo;
        if (cfg.lockscreen_source_mode == 1 && !cfg.lockscreen_video_path.empty()) {
            cur_vid = cfg.lockscreen_video_path;
        }
        if (cur_vid.empty() && !cfg.gallery_history.empty()) {
            cur_vid = cfg.gallery_history[0];
        }

        if (HoloButton(ICON_FA_CAMERA "  Choose Custom Frame for Lock Screen...", ImVec2(320, 0))) {
            if (!cur_vid.empty()) {
                OpenCaptureModal(cur_vid, 2);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Select a specific video and exact timestamp/frame specifically for the Windows Lock Screen");
        }
        ImGui::Unindent(18.0f);
    } else {
        ImGui::TextDisabled("  Windows Lock Screen wallpaper integration is currently disabled.");
    }

    EndModuleCard();
    ImGui::Spacing();

    // CARD 3: Windows Screensaver Integration
    BeginModuleCard("ScreensaverCard", ImVec2(0, 115), 18.0f, ST_CARD_NORMAL, ImGuiWindowFlags_NoScrollbar);

    if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
    ImGui::TextColored(ImVec4(0.945f, 0.961f, 0.976f, 1.0f), ICON_FA_TV "  3. Windows Screensaver Integration");
    if (g_fontSemibold) ImGui::PopFont();
    ImGui::Spacing();

    static bool scr_checked = false;
    static bool scr_installed = false;
    if (!scr_checked) {
        scr_installed = LockScreenManager::IsScreensaverInstalled();
        scr_checked = true;
    }

    if (ToggleSwitch("Enable MiniWallpaper as Windows Screensaver", &scr_installed)) {
        if (scr_installed) {
            wchar_t exePathBuf[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
            std::wstring exePath(exePathBuf);
            std::wstring scrPath = exePath.substr(0, exePath.find_last_of(L'.')) + L".scr";
            if (!fs::exists(scrPath)) {
                CopyFileW(exePath.c_str(), scrPath.c_str(), FALSE);
            }
            LockScreenManager::InstallScreensaver(scrPath, 300, true);
        } else {
            LockScreenManager::UninstallScreensaver();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Plays your live video wallpaper at full 60 FPS when your computer is idle!\nWhen waking the PC, it transitions seamlessly back.");
    }

    ImGui::SameLine(0, 16.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 5.0f);
    if (HoloButton(ICON_FA_GEAR "  Configure Screensaver...", ImVec2(220, 0))) {
        LockScreenManager::OpenWindowsScreensaverSettings();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Opens official Windows Screen Saver Settings to adjust idle timeout and resume password.");
    }

    ImGui::TextColored(ImVec4(0.580f, 0.639f, 0.722f, 1.0f), "Smoothly plays your live wallpaper when idle, waking up immediately upon mouse movement.");

    EndModuleCard();

    // Ruang bawah agar konten tidak tertutup dock yg melayang
    ImGui::Dummy(ImVec2(1.0f, 72.0f));
}

static void RenderGeneralSettingsTab() {
    auto& cfg = g_config.Get();

    RenderPageTitle(ICON_FA_SLIDERS, "General Settings", "Configure display scaling, master volume, Windows autostart, and power-saving rules.");

    float availW = ImGui::GetContentRegionAvail().x;
    float comboW = (std::min)(480.0f, availW);
    float sliderW = (std::min)(320.0f, availW);

    // SECTION 1: Display Scaling Mode
    if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
    ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_EXPAND "  Display Scaling & Multi-Monitor");
    if (g_fontSemibold) ImGui::PopFont();
    ImGui::Spacing();

    static const char* scalingModes[] = {
        "Auto Aspect Fill (Cover - Smart Crop, No Black Bars)",
        "Aspect Fit (Letterbox - Full Frame with Black Bars)",
        "Stretch to Screen (Ignore Aspect Ratio)"
    };
    int currentMode = cfg.scaling_mode;
    ImGui::SetNextItemWidth(comboW);
    if (ImGui::Combo("Display Scaling Mode", &currentMode, scalingModes, IM_ARRAYSIZE(scalingModes))) {
        cfg.scaling_mode = currentMode;
        g_config.Save();
        nlohmann::json req{{"cmd", "set_scaling"}, {"mode", currentMode}};
        SendIpcAsync(req.dump());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Changes how video fits the screen.\n*Note: Effect is visible when video aspect ratio differs from display\n (e.g. 4:3, 21:9 ultrawide, or vertical video on 16:9 screen).");
    }

    if (!g_hardwareInfoInit) {
        g_hardwareInfo = HardwareDetector::QuerySystemInfo();
        g_hardwareInfoInit = true;
    }

    if (g_hardwareInfo.displays.size() > 1) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.75f, 1.0f), "Target Desktop Screen(s):");

        bool all_checked = cfg.target_displays.empty() || (cfg.target_displays.size() == g_hardwareInfo.displays.size());
        if (ToggleSwitch("All Monitors (Apply to All Desktops)", &all_checked)) {
            if (all_checked) {
                cfg.target_displays.clear();
            } else {
                cfg.target_displays = { 0 };
            }
            g_config.Save();
            nlohmann::json req{{"cmd", "set_target_displays"}, {"displays", cfg.target_displays}};
            SendIpcAsync(req.dump());
        }

        for (size_t d = 0; d < g_hardwareInfo.displays.size(); ++d) {
            const auto& disp = g_hardwareInfo.displays[d];
            bool is_d_checked = cfg.IsDisplayEnabled(static_cast<int>(d));
            std::string label = "Display " + std::to_string(d + 1) + ": " + disp.name + " (" +
                                std::to_string(disp.width) + "x" + std::to_string(disp.height) + ")" +
                                (disp.is_primary ? " [Primary]" : " [Secondary]");
            if (ToggleSwitch(label.c_str(), &is_d_checked)) {
                cfg.SetDisplayEnabled(static_cast<int>(d), is_d_checked);
                g_config.Save();
                nlohmann::json req{{"cmd", "set_target_displays"}, {"displays", cfg.target_displays}};
                SendIpcAsync(req.dump());
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // SECTION 2: Audio Configuration
    if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
    ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_VOLUME_HIGH "  Audio Configuration");
    if (g_fontSemibold) ImGui::PopFont();
    ImGui::Spacing();

    static float volume = 0.0f;
    static bool volume_init = false;
    if (!volume_init && !cfg.wallpapers.empty()) {
        volume = cfg.wallpapers[0].volume;
        volume_init = true;
    }

    if (StarlumeSliderFloat("Master Volume", &volume, 0.0f, 1.0f, 2, "", sliderW)) {
        if (!cfg.wallpapers.empty()) {
            cfg.wallpapers[0].volume = volume;
        }
        g_config.Save();
        nlohmann::json req{{"cmd", "set_volume"}, {"volume", volume}};
        SendIpcAsync(req.dump());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // SECTION 3: Windows Startup & Automation
    if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
    ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_DESKTOP "  Windows Startup & Automation");
    if (g_fontSemibold) ImGui::PopFont();
    ImGui::Spacing();

    static bool startup_inited = false;
    static bool startup_enabled = false;
    if (!startup_inited) {
        startup_enabled = WindowsAutostart::IsEnabled();
        startup_inited = true;
    }

    if (ToggleSwitch("Start MiniWallpaper automatically on Windows Boot", &startup_enabled)) {
        WindowsAutostart::SetEnabled(startup_enabled, 0);
        cfg.run_on_startup = startup_enabled;
        g_config.Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Launches MiniWallpaper silently in the background when Windows starts, seamlessly resuming your wallpaper.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // SECTION 4: Power & Occlusion Auto-Pause Rules
    if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
    ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_BATTERY_HALF "  Power & Occlusion Auto-Pause");
    if (g_fontSemibold) ImGui::PopFont();
    ImGui::Spacing();

    if (ToggleSwitch("Auto-Pause when Fullscreen App/Game is active", &cfg.pause_on_fullscreen)) {
        g_config.Save();
    }
    if (ToggleSwitch("Auto-Pause when Window is Maximized / Desktop Covered", &cfg.pause_on_maximized)) {
        g_config.Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Automatically pauses wallpaper playback when an application (browser, code editor, etc.) is maximized and covers the desktop, reducing CPU and GPU usage to 0%.");
    }
    if (ToggleSwitch("Auto-Pause on Battery Power", &cfg.pause_on_battery)) {
        g_config.Save();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // SECTION 5: Auto Smooth Looping
    if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
    ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_ROTATE "  Auto Smooth & Seamless Looping");
    if (g_fontSemibold) ImGui::PopFont();
    ImGui::Spacing();

    if (ToggleSwitch("Enable Auto Smooth Looping (Seamless Crossfade)", &cfg.auto_smooth_loop)) {
        g_config.Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Applies a smooth crossfade transition and time-warp deceleration at loop seams for a seamless continuous loop without hard cuts.");
    }

    if (cfg.auto_smooth_loop) {
        const char* loop_presets[] = {
            "Cinematic Speed Ramp (1.2s + 0.75x Slow-Mo Blend)",
            "Smoothstep S-Curve (0.8s Natural Easing)",
            "Gentle Flow (1.8s Ambient Scenery Blend)",
            "Instant Snap (0.4s Fast Seamless Snap)",
            "Custom Tuning..."
        };

        ImGui::SetNextItemWidth(comboW);
        if (ImGui::Combo("Looping Preset", &cfg.loop_preset, loop_presets, IM_ARRAYSIZE(loop_presets))) {
            g_config.Save();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Select curated looping behavior presets optimized for different types of wallpaper animations.");
        }

        if (cfg.loop_preset == 4) { // Custom Tuning
            if (StarlumeSliderFloat("Transition Duration", &cfg.smooth_loop_duration, 0.2f, 2.5f, 1, " seconds", sliderW)) {
                g_config.Save();
            }

            const char* easing_curves[] = {
                "Linear (Constant)",
                "Smoothstep (S-Curve Hermite)",
                "Sine Wave (Smooth Harmonic)",
                "Smootherstep (Perlin Ultra-Smooth)"
            };
            ImGui::SetNextItemWidth(sliderW);
            if (ImGui::Combo("Easing Curve", &cfg.loop_easing_curve, easing_curves, IM_ARRAYSIZE(easing_curves))) {
                g_config.Save();
            }

            if (ToggleSwitch("Enable Speed Ramping (Time Warp Deceleration)", &cfg.loop_speed_ramp)) {
                g_config.Save();
            }

            if (cfg.loop_speed_ramp) {
                if (StarlumeSliderFloat("Min Speed at Seam", &cfg.loop_min_speed, 0.50f, 0.95f, 2, "x", sliderW)) {
                    g_config.Save();
                }
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f), ICON_FA_CIRCLE_CHECK "  All settings are saved and applied automatically in real-time.");
    ImGui::Spacing();

    if (HoloButton(ICON_FA_EYE_SLASH "  Hide Window to Tray", ImVec2(200, 0))) {
        SettingsUI::Close();
    }

    // Ruang bawah agar konten tidak tertutup dock yg melayang
    ImGui::Dummy(ImVec2(1.0f, 72.0f));
}

static void RenderPerformancePanel() {
    auto& cfg = g_config.Get();

    if (!g_hardwareInfoInit) {
        g_hardwareInfo = HardwareDetector::QuerySystemInfo();
        g_hardwareInfoInit = true;
    }

    RenderPageTitle(ICON_FA_GAUGE_HIGH, "Performance & Advanced", "GPU hardware acceleration engine, frame pacing, gamer auto-sleep governor, and live telemetry.");

    float availW = ImGui::GetContentRegionAvail().x;
    ImVec2 pageStart = ImGui::GetCursorScreenPos();
    float colGap = 16.0f;
    float colW = (availW - colGap) * 0.5f;
    float contentW = colW - 36.0f; // konten dalam kartu (padX 18 x 2)

    // ---- Sampel telemetri (5 titik, kadens ~1 dtk) utk mini sparkbar ----
    static float telemCpu[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    static float telemRam[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    static float telemVram[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    static double telemLastT = 0.0;
    static bool telemInit = false;

    int cur_ram = g_shared_engine_state.system_ram_percent.load();
    int cur_vram = g_shared_engine_state.gpu_vram_percent.load();
    bool is_sleeping = g_shared_engine_state.resource_heavy_sleep.load();
    float curCpu = g_daemonConnected ? (float)g_daemonCpuPercent : 0.0f;

    double nowT = ImGui::GetTime();
    if (!telemInit) {
        telemInit = true;
        telemLastT = nowT;
        for (int i = 0; i < 5; ++i) {
            telemCpu[i] = curCpu;
            telemRam[i] = (float)cur_ram;
            telemVram[i] = (float)cur_vram;
        }
    } else if (nowT - telemLastT >= 1.0) {
        telemLastT = nowT;
        for (int i = 0; i < 4; ++i) {
            telemCpu[i] = telemCpu[i + 1];
            telemRam[i] = telemRam[i + 1];
            telemVram[i] = telemVram[i + 1];
        }
        telemCpu[4] = curCpu;
        telemRam[4] = (float)cur_ram;
        telemVram[4] = (float)cur_vram;
    }

    // ================= Baris telemetri (Variant 3: Startorch Telemetry Metric) =================
    const float tileH = 148.0f;
    {
        const float tileGap = 14.0f;
        float tileW = (availW - tileGap * 2.0f) / 3.0f;

        char cpuVal[32];
        char cpuSub[96];
        const char* cpuUnit = "%";
        const char* cpuStatus;
        ImU32 cpuCol;
        if (!g_daemonConnected) {
            snprintf(cpuVal, sizeof(cpuVal), "--");
            snprintf(cpuSub, sizeof(cpuSub), "Background engine connecting...");
            cpuUnit = "";
            cpuStatus = "OFFLINE";
            cpuCol = ST_TEXT_MUTED;
        } else {
            snprintf(cpuVal, sizeof(cpuVal), "%.1f", (double)curCpu);
            snprintf(cpuSub, sizeof(cpuSub), "Background engine process");
            if (curCpu < 40.0f) { cpuStatus = "OPTIMAL"; cpuCol = ST_GREEN; }
            else if (curCpu < 70.0f) { cpuStatus = "ELEVATED"; cpuCol = ST_GOLD; }
            else { cpuStatus = "CRITICAL"; cpuCol = ST_RED; }
        }

        int ramTrigger = cfg.pause_on_resource_heavy ? cfg.resource_ram_threshold_pct : 85;
        char ramVal[32];
        char ramSub[96];
        snprintf(ramVal, sizeof(ramVal), "%d", cur_ram);
        snprintf(ramSub, sizeof(ramSub), "Engine working set: %zu MB", g_daemonRamMB);
        const char* ramStatus;
        ImU32 ramCol;
        if (cur_ram >= ramTrigger) { ramStatus = is_sleeping ? "AUTO-SLEEP" : "HIGH"; ramCol = ST_RED; }
        else if (cur_ram >= ramTrigger - 15) { ramStatus = "ELEVATED"; ramCol = ST_GOLD; }
        else { ramStatus = "OPTIMAL"; ramCol = ST_GREEN; }

        char vramVal[32];
        char vramSub[128];
        const char* vramUnit = "%";
        float vramPct = 0.0f;
        const char* vramStatus;
        ImU32 vramCol;
        if (cur_vram <= 0) {
            snprintf(vramVal, sizeof(vramVal), "--");
            vramUnit = "";
            snprintf(vramSub, sizeof(vramSub), "No VRAM readout (software decode)");
            vramStatus = "SOFTWARE";
            vramCol = ST_CYAN;
        } else {
            int vramTrigger = cfg.pause_on_resource_heavy ? cfg.resource_vram_threshold_pct : 85;
            snprintf(vramVal, sizeof(vramVal), "%d", cur_vram);
            if (g_daemonActiveGpuIndex == -1 || !g_daemonHwDecode) {
                snprintf(vramSub, sizeof(vramSub), "Engine video memory: %zu MB (software)", g_daemonVramMB);
            } else {
                snprintf(vramSub, sizeof(vramSub), "Engine video memory: %zu MB (HW decode)", g_daemonVramMB);
            }
            vramPct = (float)cur_vram / 100.0f;
            if (cur_vram >= vramTrigger) { vramStatus = is_sleeping ? "AUTO-SLEEP" : "HIGH"; vramCol = ST_RED; }
            else if (cur_vram >= vramTrigger - 15) { vramStatus = "ELEVATED"; vramCol = ST_GOLD; }
            else { vramStatus = "OPTIMAL"; vramCol = ST_GREEN; }
        }

        TelemetryTile("TelemCPU", "ENGINE CPU LOAD", (std::min)(1.0f, (std::max)(0.0f, curCpu / 100.0f)), cpuVal, cpuUnit, cpuStatus, cpuCol, telemCpu, cpuSub, ImVec2(tileW, tileH));
        ImGui::SetCursorScreenPos(ImVec2(pageStart.x + tileW + tileGap, pageStart.y));
        TelemetryTile("TelemRAM", "SYSTEM RAM", (std::min)(1.0f, (std::max)(0.0f, (float)cur_ram / 100.0f)), ramVal, "%", ramStatus, ramCol, telemRam, ramSub, ImVec2(tileW, tileH));
        ImGui::SetCursorScreenPos(ImVec2(pageStart.x + (tileW + tileGap) * 2.0f, pageStart.y));
        TelemetryTile("TelemVRAM", "GPU VRAM", vramPct, vramVal, vramUnit, vramStatus, vramCol, telemVram, vramSub, ImVec2(tileW, tileH));
    }

    float colTopY = pageStart.y + tileH + 16.0f;
    float leftX = pageStart.x;
    float rightX = pageStart.x + colW + colGap;
    float colY = colTopY;

    // ================= Kolom kiri: kartu kontrol =================

    // Kartu 1: Rendering Engine & Frame Rate
    ImGui::SetCursorScreenPos(ImVec2(leftX, colY));
    BeginGlassCard("PerfEngineCard", ImVec2(colW, 0.0f), 18.0f, 16.0f, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    GlassCardHeader(ICON_FA_MICROCHIP "  RENDERING ENGINE", "MOD // 01");

    std::vector<std::string> deviceNames;
    std::vector<int> deviceValues;

    for (size_t g = 0; g < g_hardwareInfo.gpus.size(); ++g) {
        std::string label = "GPU " + std::to_string(g + 1) + ": " + g_hardwareInfo.gpus[g].name + " (DirectX 11 HW)";
        deviceNames.push_back(label);
        deviceValues.push_back(static_cast<int>(g));
    }
    deviceNames.push_back("CPU: Software Processor Decode (swscale)");
    deviceValues.push_back(-1);

    int currentDeviceCombo = 0;
    for (size_t i = 0; i < deviceValues.size(); ++i) {
        if (deviceValues[i] == cfg.gpu_device_index) {
            currentDeviceCombo = static_cast<int>(i);
            break;
        }
    }

    std::vector<const char*> comboItems;
    for (const auto& name : deviceNames) {
        comboItems.push_back(name.c_str());
    }

    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 lp = ImGui::GetCursorScreenPos();
        dl->AddText(lp, ST_TEXT_DIM, "Video Rendering Engine");
        ImGui::Dummy(ImVec2(contentW, ImGui::GetFontSize() + 5.0f));
    }
    ImGui::SetNextItemWidth(contentW);
    if (ImGui::Combo("##render_engine", &currentDeviceCombo, comboItems.data(), static_cast<int>(comboItems.size()))) {
        cfg.gpu_device_index = deviceValues[currentDeviceCombo];
        g_config.Save();
        nlohmann::json req{{"cmd", "set_render_device"}, {"gpu_index", cfg.gpu_device_index}};
        SendIpcAsync(req.dump());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Choose whether video decoding & rendering runs on GPU 1, GPU 2, or CPU Software.");
    }

    if (StarlumeSliderInt("Target Render FPS", &cfg.target_fps, 15, 60, "", contentW)) {
        g_config.Save();
        nlohmann::json req{{"cmd", "set_fps"}, {"fps", cfg.target_fps}};
        SendIpcAsync(req.dump());
    }
    ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f), ICON_FA_CIRCLE_CHECK "  Active: %d FPS", g_daemonFps);

    if (StarlumeSliderInt("Battery Saver FPS", &cfg.battery_fps, 10, 30, "", contentW)) {
        g_config.Save();
    }
    colY += EndGlassCard() + colGap;

    // Kartu 2: Smart Resource Governor (Gaming & Heavy Load Auto-Sleep)
    ImGui::SetCursorScreenPos(ImVec2(leftX, colY));
    BeginGlassCard("PerfGovernorCard", ImVec2(colW, 0.0f), 18.0f, 16.0f, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    GlassCardHeader(ICON_FA_GAUGE_HIGH "  RESOURCE GOVERNOR", "MOD // 02");

    if (ToggleSwitch("Auto-Sleep on heavy RAM / VRAM load", &cfg.pause_on_resource_heavy)) {
        g_config.Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Essential for windowed gaming! When a heavy game or app pushes RAM or VRAM above the threshold, wallpaper enters Deep Sleep (0% CPU, 0 MB VRAM) to yield maximum performance to your game.");
    }

    if (cfg.pause_on_resource_heavy) {
        if (StarlumeSliderInt("System RAM Threshold", &cfg.resource_ram_threshold_pct, 60, 95, "%", contentW)) {
            g_config.Save();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Triggers auto-sleep when overall PC system RAM usage hits this percentage (default 80%).");
        }

        if (StarlumeSliderInt("GPU VRAM Threshold", &cfg.resource_vram_threshold_pct, 60, 95, "%", contentW)) {
            g_config.Save();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Triggers auto-sleep when GPU dedicated video memory usage hits this percentage (default 80%).");
        }

        bool ram_high = (cur_ram >= cfg.resource_ram_threshold_pct);
        bool vram_high = (cur_vram >= cfg.resource_vram_threshold_pct);

        char ram_buf[64], vram_buf[64];
        snprintf(ram_buf, sizeof(ram_buf), "%d%%  (limit %d%%)", cur_ram, cfg.resource_ram_threshold_pct);
        snprintf(vram_buf, sizeof(vram_buf), "%d%%  (limit %d%%)", cur_vram, cfg.resource_vram_threshold_pct);

        StarlumeBar("System RAM Usage", cur_ram / 100.0f, ram_buf, ram_high ? ST_RED : ST_GOLD,
                    ram_high ? ST_RED : ST_PINK_SOFT, cfg.resource_ram_threshold_pct / 100.0f);

        if (cur_vram > 0) {
            StarlumeBar("GPU VRAM Usage", cur_vram / 100.0f, vram_buf, vram_high ? ST_RED : ST_GOLD,
                        vram_high ? ST_RED : ST_PINK_SOFT, cfg.resource_vram_threshold_pct / 100.0f);
        } else {
            TextDim("GPU VRAM readout unavailable (software decode).");
        }

        if (is_sleeping) {
            ImGui::TextColored(ImVec4(1.00f, 0.82f, 0.40f, 1.00f), ICON_FA_EYE_SLASH "  Auto-Sleep Active: Releasing 100% VRAM & pausing rendering for active game");
        }
    }
    colY += EndGlassCard() + colGap;

    // Kartu 3: Video Optimization Transcoding Mode
    ImGui::SetCursorScreenPos(ImVec2(leftX, colY));
    BeginGlassCard("PerfOptimizerCard", ImVec2(colW, 0.0f), 18.0f, 16.0f, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    GlassCardHeader(ICON_FA_EXPAND "  TRANSCODING MODE", "MOD // 03");

    if (ImGui::RadioButton("Aspect Fit (Proportional - keeps entire frame)", &cfg.optimizer_crop_mode, 0)) {
        g_config.Save();
    }
    TextDim("Letterboxes the frame if its ratio differs from your screen.");
    if (ImGui::RadioButton("Aspect Fill / Center Crop (zero black bars)", &cfg.optimizer_crop_mode, 1)) {
        g_config.Save();
    }
    TextDim("Fills the screen exactly, cropping overflow edges.");
    colY += EndGlassCard() + colGap;

    // ================= Kolom kanan: status & informasi sistem =================

    // Kartu 4: Real-Time Engine Status & Telemetry
    float colY2 = colTopY;
    ImGui::SetCursorScreenPos(ImVec2(rightX, colY2));
    BeginGlassCard("PerfStatusCard", ImVec2(colW, 0.0f), 18.0f, 16.0f, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    GlassCardHeader(ICON_FA_CHART_LINE "  ENGINE STATUS", "SYS // 01");

    if (!g_daemonConnected) {
        ImGui::TextColored(ImVec4(1.00f, 0.30f, 0.43f, 1.0f), "Background Engine Initializing...");
    } else {
        if (g_daemonPlaying && !g_daemonPaused) {
            ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f), ICON_FA_CIRCLE_CHECK "  Playing (Active)");
        } else if (g_daemonPaused) {
            ImGui::TextColored(ImVec4(1.00f, 0.82f, 0.40f, 1.00f), ICON_FA_PAUSE "  Paused");
        } else {
            ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.75f, 1.00f), ICON_FA_STOP "  Idle");
        }

        std::string vidName = g_daemonCurrentVideo.empty() ? std::string("(None)") : fs::path(g_daemonCurrentVideo).filename().string();
        ImGui::Text("Active Video: %s", StEllipsize(vidName, 34).c_str());
        ImGui::Text("Render: %d FPS  |  Source: %.1f FPS", g_daemonFps, g_daemonVideoFps);
        ImGui::Text("Resolution: %dx%d (%s)", g_daemonWidth, g_daemonHeight, g_daemonCodec.c_str());
        ImGui::Text("Duration: %.1f seconds", g_daemonDuration);
    }
    colY2 += EndGlassCard() + colGap;

    // Kartu 5: Connected Monitors Topology
    size_t dispCount = g_hardwareInfo.displays.size();
    ImGui::SetCursorScreenPos(ImVec2(rightX, colY2));
    BeginGlassCard("PerfMonitorsCard", ImVec2(colW, 0.0f), 18.0f, 16.0f, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    GlassCardHeader(ICON_FA_TV "  CONNECTED MONITORS", "SYS // 02");

    if (HoloButtonSmall(ICON_FA_ROTATE "  RE-DETECT DISPLAYS")) {
        g_hardwareInfo.displays = HardwareDetector::GetDisplayList();
    }

    if (dispCount == 0) {
        TextDim("No displays enumerated.");
    } else {
        for (size_t idx = 0; idx < dispCount; ++idx) {
            const auto& disp = g_hardwareInfo.displays[idx];
            if (disp.is_primary) {
                ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f),
                    "• Display %zu [PRIMARY]: %s", idx + 1, disp.name.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.62f, 0.88f, 1.00f, 1.00f),
                    "• Display %zu [SECONDARY]: %s", idx + 1, disp.name.c_str());
            }
            TextDim("   %d x %d  |  %d Hz  |  pos (%d, %d)",
                    disp.width, disp.height, disp.refresh_rate, disp.pos_x, disp.pos_y);
        }
    }
    colY2 += EndGlassCard() + colGap;

    // Kartu 6: Hardware Topology (CPU & GPU Detection)
    size_t gpuCount = g_hardwareInfo.gpus.size();
    ImGui::SetCursorScreenPos(ImVec2(rightX, colY2));
    BeginGlassCard("PerfHardwareCard", ImVec2(colW, 0.0f), 18.0f, 16.0f, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    GlassCardHeader(ICON_FA_HARD_DRIVE "  HARDWARE TOPOLOGY", "SYS // 03");

    if (HoloButtonSmall(ICON_FA_ROTATE "  RE-DETECT HARDWARE")) {
        g_hardwareInfo = HardwareDetector::QuerySystemInfo();
    }

    if (g_daemonActiveGpuIndex == -1) {
        ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f),
            "CPU: %s [SOFTWARE RENDER]", StEllipsize(g_hardwareInfo.cpu.model_name, 26).c_str());
    } else {
        ImGui::TextColored(ImVec4(0.92f, 0.93f, 0.95f, 1.00f),
            "CPU: %s", StEllipsize(g_hardwareInfo.cpu.model_name, 36).c_str());
    }
    TextDim("   %d physical cores / %d logical threads",
                g_hardwareInfo.cpu.physical_cores, g_hardwareInfo.cpu.logical_cores);

    if (gpuCount == 0) {
        TextDim("GPU: Standard Display Adapter");
    } else {
        for (size_t g_idx = 0; g_idx < gpuCount; ++g_idx) {
            const auto& gpu = g_hardwareInfo.gpus[g_idx];
            bool is_this_gpu_active = (g_daemonActiveGpuIndex == static_cast<int>(g_idx));
            if (is_this_gpu_active) {
                ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f),
                    "GPU %zu: %s [ACTIVE D3D11]",
                    g_idx + 1, StEllipsize(gpu.name, 26).c_str());
            } else {
                ImGui::TextColored(ImVec4(0.80f, 0.82f, 0.88f, 1.00f),
                    "GPU %zu: %s [%s]",
                    g_idx + 1, StEllipsize(gpu.name, 30).c_str(), gpu.is_discrete ? "dGPU" : "iGPU");
            }
            TextDim("   VRAM: %zu MB (%.1f GB)  |  Shared: %zu MB",
                        gpu.dedicated_vram_mb, static_cast<float>(gpu.dedicated_vram_mb) / 1024.0f,
                        gpu.shared_vram_mb);
        }
    }
    colY2 += EndGlassCard() + colGap;

    // Kartu 7: Diagnostics & Activity Logs
    ImGui::SetCursorScreenPos(ImVec2(rightX, colY2));
    BeginGlassCard("PerfDiagCard", ImVec2(colW, 0.0f), 18.0f, 16.0f, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    GlassCardHeader(ICON_FA_CIRCLE_INFO "  DIAGNOSTICS & LOGS", "SYS // 04");

    if (g_daemonInjected) {
        ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f), "Desktop Injection (WorkerW): OK");
    } else {
        ImGui::TextColored(ImVec4(1.00f, 0.30f, 0.43f, 1.00f), "Desktop Injection (WorkerW): FAILED");
    }
    if (g_daemonActiveGpuIndex == -1 || !g_daemonHwDecode) {
        ImGui::Text("Decoder: Software (CPU swscale)");
    } else {
        ImGui::Text("Decoder: Hardware D3D11VA (HW decode)");
    }
    ImGui::Text("Frames Rendered: %llu", (unsigned long long)g_daemonFramesRendered);
    if (!g_daemonLastError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Last Error: %s", StEllipsize(g_daemonLastError, 28).c_str());
    } else {
        ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f), "Last Error: (none)");
    }

    {
        float btnW = (contentW - 8.0f) * 0.5f;
        if (HoloButtonEx(ICON_FA_EXPAND "  FLASH RENDER", ImVec2(btnW, 27.0f), false, true)) {
            SendIpcAsync("{\"cmd\":\"test_render\"}");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Flashes the injected render window cyan for diagnostics.");
        }
        ImGui::SameLine(0.0f, 8.0f);
        if (HoloButtonEx(ICON_FA_FOLDER_OPEN "  OPEN LOG", ImVec2(btnW, 27.0f), false, true)) {
            const char* appData = getenv("APPDATA");
            if (appData) {
                std::string logPath = std::string(appData) + "\\MiniWallpaper\\engine.log";
                ShellExecuteA(nullptr, "open", "notepad.exe", logPath.c_str(), nullptr, SW_SHOW);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Opens the background engine log in Notepad.");
        }
    }
    TextDim("*Cyan flash on the desktop = injection works.");
    colY2 += EndGlassCard() + colGap;

    // Akhiri layout 2 kolom: lanjutkan dari kolom terpanjang
    ImGui::SetCursorScreenPos(ImVec2(pageStart.x, (std::max)(colY, colY2)));
    // Ruang bawah agar konten tidak tertutup dock yg melayang
    ImGui::Dummy(ImVec2(1.0f, 72.0f));
}

static void RequestCaptureModalPreview() {
    if (g_captureModalPath.empty()) return;
    if (g_captureModalPreviewWorkerActive.load()) {
        g_captureModalQueuedSec.store(g_captureModalTimeSec);
        g_captureModalPreviewQueued.store(true);
        return;
    }
    g_captureModalPreviewWorkerActive.store(true);

    std::string vpath = g_captureModalPath;
    float timeSec = g_captureModalTimeSec;
    int pw = g_captureModalPreviewW;
    int ph = g_captureModalPreviewH;

    std::thread([vpath, timeSec, pw, ph]() {
        float currentTargetSec = timeSec;
        while (true) {
            std::vector<uint8_t> bgra;
            if (ThumbnailManager::ExtractFrameToBGRA(vpath, bgra, pw, ph, static_cast<double>(currentTargetSec))) {
                g_captureModalPreviewBgra = std::move(bgra);
                g_captureModalPreviewPending.store(true);
                g_captureModalPreviewSec = currentTargetSec;
            }
            if (g_captureModalPreviewQueued.exchange(false)) {
                currentTargetSec = g_captureModalQueuedSec.load();
            } else {
                break;
            }
        }
        g_captureModalPreviewWorkerActive.store(false);
    }).detach();
}

static void OpenCaptureModal(const std::string& video_path, int targetFilter) {
    if (video_path.empty()) return;

    g_captureModalPath = video_path;
    auto probe = VideoOptimizer::Probe(video_path);
    if (probe.valid) {
        g_captureModalDuration = (probe.duration > 0.0) ? probe.duration : 10.0;
        g_captureModalFps = (probe.fps > 0.0) ? probe.fps : 30.0;
        g_captureModalSourceW = probe.width > 0 ? probe.width : 1920;
        g_captureModalSourceH = probe.height > 0 ? probe.height : 1080;
    } else {
        g_captureModalDuration = (g_daemonDuration > 0.0) ? g_daemonDuration : 10.0;
        g_captureModalFps = (g_daemonVideoFps > 0.0) ? g_daemonVideoFps : 30.0;
        g_captureModalSourceW = g_daemonWidth > 0 ? g_daemonWidth : 1920;
        g_captureModalSourceH = g_daemonHeight > 0 ? g_daemonHeight : 1080;
    }

    auto& cfg = g_config.Get();
    if (targetFilter == 1) {
        g_captureModalTargetDesktop = true;
        g_captureModalTargetLockscreen = false;
    } else if (targetFilter == 2) {
        g_captureModalTargetDesktop = false;
        g_captureModalTargetLockscreen = true;
    } else {
        g_captureModalTargetDesktop = cfg.update_desktop_wallpaper;
        g_captureModalTargetLockscreen = cfg.update_lockscreen;
        if (!g_captureModalTargetDesktop && !g_captureModalTargetLockscreen) {
            g_captureModalTargetDesktop = true;
        }
    }

    // Default time: if this video matches configured custom desktop/lockscreen or active video, snap to that timestamp
    if (targetFilter == 1 && cfg.desktop_static_source_mode == 1 && cfg.desktop_static_video_path == video_path) {
        g_captureModalTimeSec = static_cast<float>(cfg.desktop_static_timestamp);
    } else if (targetFilter == 2 && cfg.lockscreen_source_mode == 1 && cfg.lockscreen_video_path == video_path) {
        g_captureModalTimeSec = static_cast<float>(cfg.lockscreen_timestamp);
    } else if (!g_daemonCurrentVideo.empty() && (g_daemonCurrentVideo == video_path || g_daemonCurrentVideo.find(fs::path(video_path).stem().string()) != std::string::npos)) {
        g_captureModalTimeSec = static_cast<float>(g_daemonCurrentTimeSec);
    } else {
        g_captureModalTimeSec = 1.0f;
    }
    if (g_captureModalTimeSec > g_captureModalDuration && g_captureModalDuration > 0.0) {
        g_captureModalTimeSec = static_cast<float>(g_captureModalDuration * 0.5);
    }
    g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
    g_captureModalStatus.clear();
    g_captureModalPreviewSRV.Reset();
    g_captureModalPreviewSec = -1.0f;
    g_captureModalAutoPreviewRequested = false;
    g_captureModalLastChangeTick = 0;
    g_captureModalPreviewQueued.store(false);
    g_showCaptureModal = true;
}

static void RenderCaptureFrameModal() {
    if (g_showCaptureModal) {
        ImGui::OpenPopup("Capture Static Wallpaper / Lock Screen Frame");
        if (!g_captureModalPreviewSRV && !g_captureModalPreviewWorkerActive.load() && g_captureModalPreviewSec < 0.0f) {
            RequestCaptureModalPreview();
        }

        if (g_captureModalAutoPreviewRequested) {
            uint64_t nowTick = GetTickCount64();
            if (nowTick >= g_captureModalLastChangeTick + 150) {
                g_captureModalAutoPreviewRequested = false;
                RequestCaptureModalPreview();
            }
        }
    }

    if (g_captureModalPreviewPending.exchange(false) && g_pd3dDevice) {
        g_captureModalPreviewSRV = ThumbnailManager::CreateSRVFromBGRA(
            g_pd3dDevice,
            g_captureModalPreviewBgra.data(),
            g_captureModalPreviewW,
            g_captureModalPreviewH
        );
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(ImVec2(center.x, 26.0f), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(680, 580));
    ImGui::SetNextWindowSizeConstraints(ImVec2(560, 300), ImVec2(680, 700));

    if (ImGui::BeginPopupModal("Capture Static Wallpaper / Lock Screen Frame", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
        ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_CAMERA "  Select Frame & Injection Destination");
        if (g_fontSemibold) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        fs::path p(g_captureModalPath);
        std::string filename = p.filename().string();
        if (filename.empty()) filename = g_captureModalPath;

        auto& cfg = g_config.Get();
        if (!cfg.gallery_history.empty()) {
            int currentIdx = -1;
            for (size_t i = 0; i < cfg.gallery_history.size(); ++i) {
                if (cfg.gallery_history[i] == g_captureModalPath) {
                    currentIdx = static_cast<int>(i);
                    break;
                }
            }
            std::string currentLabel = (currentIdx >= 0) ? fs::path(cfg.gallery_history[currentIdx]).filename().string() : filename;
            ImGui::SetNextItemWidth(450.0f);
            if (ImGui::BeginCombo("Select Video Source", currentLabel.c_str())) {
                for (size_t i = 0; i < cfg.gallery_history.size(); ++i) {
                    bool isSel = (static_cast<int>(i) == currentIdx);
                    std::string itemLabel = fs::path(cfg.gallery_history[i]).filename().string();
                    if (ImGui::Selectable(itemLabel.c_str(), isSel)) {
                        int filter = (g_captureModalTargetDesktop && !g_captureModalTargetLockscreen) ? 1 :
                                     (!g_captureModalTargetDesktop && g_captureModalTargetLockscreen) ? 2 : 0;
                        OpenCaptureModal(cfg.gallery_history[i], filter);
                    }
                    if (isSel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::TextColored(ImVec4(0.92f, 0.93f, 0.95f, 1.0f), "Video Source: %s", filename.c_str());
        }

        // Video info row
        int durMin = static_cast<int>(g_captureModalDuration) / 60;
        float durSec = static_cast<float>(g_captureModalDuration) - durMin * 60.0f;
        int totalFrames = static_cast<int>(g_captureModalDuration * g_captureModalFps);
        ImGui::TextColored(ImVec4(0.65f, 0.68f, 0.75f, 1.0f), "Resolution: %dx%d  |  Framerate: %.1f FPS  |  Duration: %02d:%05.2f (%d frames)",
            g_captureModalSourceW, g_captureModalSourceH, g_captureModalFps, durMin, durSec, totalFrames);
        ImGui::Spacing();

        // 1. Destination Checkboxes
        if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
        ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_DESKTOP "  Target Destination (Where to inject this frame):");
        if (g_fontSemibold) ImGui::PopFont();
        ToggleSwitch("Apply to Windows Desktop Wallpaper (0s Instant Boot Visual)", &g_captureModalTargetDesktop);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Sets this frame directly into the native Windows Desktop Wallpaper.\nIt appears instantly in 0.0s upon Windows boot without black screen or delay.");
        }

        ToggleSwitch("Apply to Windows Lock Screen (Win + L Seamless Transition)", &g_captureModalTargetLockscreen);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Pre-caches this frame for the Windows Lock Screen.\nWhen pressing Win+L or locking the PC, this image is shown seamlessly.");
        }

        if (!g_captureModalTargetDesktop && !g_captureModalTargetLockscreen) {
            ImGui::TextColored(ImVec4(1.00f, 0.30f, 0.43f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " Please select at least one destination (Desktop, Lock Screen, or Both).");
        }

        ImGui::Spacing();
        ImGui::Separator();

        float maxTime = static_cast<float>((std::max)(0.1, g_captureModalDuration));

        // Time Slider (Starlume)
        if (StarlumeSliderFloat("Preview Time", &g_captureModalTimeSec, 0.0f, maxTime, 2, " s", 520.0f)) {
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            g_captureModalAutoPreviewRequested = true;
            g_captureModalLastChangeTick = GetTickCount64();
        }
        if (ImGui::IsItemDeactivated()) {
            g_captureModalAutoPreviewRequested = false;
            RequestCaptureModalPreview();
        }

        // Two-way Synced Numeric Inputs
        ImGui::PushItemWidth(140.0f);
        if (ImGui::InputFloat("Time (seconds)", &g_captureModalTimeSec, 0.1f, 1.0f, "%.2f s")) {
            g_captureModalTimeSec = std::clamp(g_captureModalTimeSec, 0.0f, maxTime);
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            g_captureModalAutoPreviewRequested = true;
            g_captureModalLastChangeTick = GetTickCount64();
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            g_captureModalAutoPreviewRequested = false;
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (ImGui::InputInt("Frame Number", &g_captureModalFrameNum, 1, 10)) {
            g_captureModalFrameNum = std::clamp(g_captureModalFrameNum, 0, (std::max)(1, totalFrames));
            g_captureModalTimeSec = static_cast<float>(g_captureModalFrameNum / g_captureModalFps);
            g_captureModalTimeSec = std::clamp(g_captureModalTimeSec, 0.0f, maxTime);
            g_captureModalAutoPreviewRequested = true;
            g_captureModalLastChangeTick = GetTickCount64();
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            g_captureModalAutoPreviewRequested = false;
            RequestCaptureModalPreview();
        }
        ImGui::PopItemWidth();

        ImGui::SameLine();
        if (HoloButton(ICON_FA_ROTATE " Preview Frame")) {
            RequestCaptureModalPreview();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Render a thumbnail preview of the selected frame");
        }

        // Presets & Snap Button
        ImGui::Spacing();
        if (HoloButtonSmall("0s (Start)")) {
            g_captureModalTimeSec = 0.0f;
            g_captureModalFrameNum = 0;
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (HoloButtonSmall("25%")) {
            g_captureModalTimeSec = maxTime * 0.25f;
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (HoloButtonSmall("50% (Middle)")) {
            g_captureModalTimeSec = maxTime * 0.50f;
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (HoloButtonSmall("75%")) {
            g_captureModalTimeSec = maxTime * 0.75f;
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (HoloButtonSmall("End")) {
            g_captureModalTimeSec = (std::max)(0.0f, maxTime - 0.1f);
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (HoloButtonSmall(ICON_FA_CROSSHAIRS " Snap to Live Video")) {
            g_captureModalTimeSec = static_cast<float>(g_daemonCurrentTimeSec);
            g_captureModalTimeSec = std::clamp(g_captureModalTimeSec, 0.0f, maxTime);
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            RequestCaptureModalPreview();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Snap time to the exact position currently playing on your desktop");
        }

        // Preview box + action buttons side by side
        ImGui::Spacing();
        float previewBoxW = 300.0f;
        float previewBoxH = 169.0f;
        bool isLoadingPreview = g_captureModalPreviewWorkerActive.load() || g_captureModalAutoPreviewRequested;

        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + previewBoxW, p0.y + previewBoxH);

        if (g_captureModalPreviewSRV) {
            ImGui::Image((ImTextureID)g_captureModalPreviewSRV.Get(), ImVec2(previewBoxW, previewBoxH));
            if (isLoadingPreview) {
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                draw_list->AddRectFilled(p0, p1, IM_COL32(9, 12, 21, 150), 4.0f);
                draw_list->AddRect(p0, p1, IM_COL32(56, 232, 255, 180), 4.0f);
                draw_list->AddText(ImVec2(p0.x + 95.0f, p0.y + 80.0f), IM_COL32(56, 232, 255, 255), ICON_FA_ROTATE " Loading Frame...");
            }
        } else {
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            draw_list->AddRectFilled(p0, p1, IM_COL32(17, 23, 38, 255), 4.0f);
            draw_list->AddRect(p0, p1, IM_COL32(56, 232, 255, 46), 4.0f);
            if (isLoadingPreview) {
                draw_list->AddText(ImVec2(p0.x + 80.0f, p0.y + 80.0f), IM_COL32(56, 232, 255, 255), ICON_FA_ROTATE " Extracting Preview...");
            } else {
                draw_list->AddText(ImVec2(p0.x + 65.0f, p0.y + 80.0f), IM_COL32(148, 163, 184, 255), "Drag slider to preview frame");
            }
            ImGui::Dummy(ImVec2(previewBoxW, previewBoxH));
        }

        // Action column beside the preview
        ImGui::SameLine(0, 18.0f);
        ImGui::BeginGroup();
        {
            bool canApply = (g_captureModalTargetDesktop || g_captureModalTargetLockscreen);
            if (!canApply) ImGui::BeginDisabled();
            if (HoloButton(ICON_FA_CHECK "  Capture & Apply Static Wallpaper", ImVec2(320, 40))) {
                if (g_captureModalTargetDesktop) {
                    cfg.desktop_static_source_mode = 1;
                    cfg.desktop_static_video_path = g_captureModalPath;
                    cfg.desktop_static_timestamp = static_cast<double>(g_captureModalTimeSec);
                    cfg.update_desktop_wallpaper = true;
                }
                if (g_captureModalTargetLockscreen) {
                    cfg.lockscreen_source_mode = 1;
                    cfg.lockscreen_video_path = g_captureModalPath;
                    cfg.lockscreen_timestamp = static_cast<double>(g_captureModalTimeSec);
                    cfg.update_lockscreen = true;
                }
                g_config.Save();

                nlohmann::json req{
                    {"cmd", "sync_desktop_wallpaper"},
                    {"path", g_captureModalPath},
                    {"desktop", g_captureModalTargetDesktop},
                    {"lockscreen", g_captureModalTargetLockscreen},
                    {"timestamp", static_cast<double>(g_captureModalTimeSec)},
                    {"save_mode", 1}
                };
                SendIpcAsync(req.dump());

                std::string destStr = (g_captureModalTargetDesktop && g_captureModalTargetLockscreen) ? "Desktop & Lock Screen" :
                                      (g_captureModalTargetDesktop ? "Desktop Wallpaper" : "Lock Screen");
                char statusMsg[256];
                sprintf_s(statusMsg, "Pristine static frame at %.2fs successfully injected to %s!", g_captureModalTimeSec, destStr.c_str());
                g_captureModalStatus = statusMsg;

                g_showCaptureModal = false;
                ImGui::CloseCurrentPopup();
            }
            if (!canApply) ImGui::EndDisabled();

            ImGui::Spacing();
            if (HoloButton("Cancel", ImVec2(320, 36))) {
                g_showCaptureModal = false;
                ImGui::CloseCurrentPopup();
            }

            if (!g_captureModalStatus.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.0f), "%s", g_captureModalStatus.c_str());
            }
        }
        ImGui::EndGroup();

        ImGui::EndPopup();
    }
}

static void RenderOptimizeModal() {
    if (g_showOptimizeModal) {
        ImGui::OpenPopup("Optimize Video for Display?");
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620, 310));

    if (ImGui::BeginPopupModal("Optimize Video for Display?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
        ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_COMPACT_DISC "  High-Resolution Video Detected");
        if (g_fontSemibold) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        int screen_w = GetSystemMetrics(SM_CXSCREEN);
        int screen_h = GetSystemMetrics(SM_CYSCREEN);
        if (screen_w <= 0) screen_w = 1920;
        if (screen_h <= 0) screen_h = 1080;

        auto& cfg = g_config.Get();

        ImGui::Text("Source Video Resolution : %dx%d", g_pendingSourceW, g_pendingSourceH);

        ImGui::Spacing();
        if (g_fontSemibold) ImGui::PushFont(g_fontSemibold);
        ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_EXPAND "  Optimization Scaling Mode:");
        if (g_fontSemibold) ImGui::PopFont();

        if (ImGui::RadioButton("Aspect Fit (Proportional - keeps entire frame intact)", &cfg.optimizer_crop_mode, 0)) {
            g_config.Save();
        }
        if (ImGui::RadioButton("Aspect Fill / Center Crop (Exact Full Screen 1080p - zero black bars)", &cfg.optimizer_crop_mode, 1)) {
            g_config.Save();
        }

        auto [target_w, target_h] = VideoOptimizer::CalculateTargetDimensions(g_pendingSourceW, g_pendingSourceH, screen_w, screen_h, cfg.optimizer_crop_mode);
        g_pendingTargetW = target_w;
        g_pendingTargetH = target_h;

        ImGui::Spacing();
        if (cfg.optimizer_crop_mode == 0) {
            ImGui::Text("Target Resolution : %dx%d (Proportional Letterbox)", g_pendingTargetW, g_pendingTargetH);
        } else {
            ImGui::Text("Target Resolution : %dx%d (Full Screen Center-Crop)", g_pendingTargetW, g_pendingTargetH);
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f),
            "*Optimizing saves up to 75%% GPU & VRAM with crystal-clear Lanczos downscaling!");
        
        ImGui::Spacing();
        ToggleSwitch("Remember my choice (Auto-downscale in the future)", &g_rememberDownscaleChoice);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (HoloButton(ICON_FA_DOWNLOAD "  Optimize Video (Recommended)", ImVec2(260, 36))) {
            if (g_rememberDownscaleChoice) {
                cfg.auto_downscale_highres = true;
                cfg.prompt_downscale = false;
                g_config.Save();
            }
            g_showOptimizeModal = false;
            ImGui::CloseCurrentPopup();
            StartVideoOptimization(g_pendingOptimizePath, g_pendingTargetW, g_pendingTargetH, g_pendingOptimizeAction, cfg.optimizer_crop_mode);
        }

        ImGui::SameLine();
        if (HoloButton("Play Original", ImVec2(130, 36))) {
            if (g_rememberDownscaleChoice) {
                auto& cfg = g_config.Get();
                cfg.auto_downscale_highres = false;
                cfg.prompt_downscale = false;
                g_config.Save();
            }
            g_showOptimizeModal = false;
            ImGui::CloseCurrentPopup();
            ApplyAction(g_pendingOptimizePath, g_pendingOptimizeAction);
        }

        ImGui::SameLine();
        if (HoloButton("Cancel", ImVec2(100, 36))) {
            g_showOptimizeModal = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

static void RenderOptimizationProgress() {
    if (!g_video_optimizer.IsRunning()) return;

    BeginModuleCard("OptProgressBanner", ImVec2(0, 100), 14.0f, ST_CARD_NORMAL, ImGuiWindowFlags_NoScrollbar);

    float prog = g_video_optimizer.GetProgress();
    std::string status = g_video_optimizer.GetCurrentStatus();

    ImGui::TextColored(ImVec4(0.945f, 0.961f, 0.976f, 1.00f), ICON_FA_ROTATE "  %s", status.c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - 110);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 5.0f);
    if (HoloButtonSmall("Cancel")) {
        g_video_optimizer.Cancel();
    }

    char progText[32];
    snprintf(progText, sizeof(progText), "%.0f%%", prog * 100.0f);
    StarlumeProgress(prog, 14.0f, progText);

    EndModuleCard();
    ImGui::Spacing();
}

// Overlay drag & drop: tampil "Drop video here" saat file diseret ke atas window.
static bool g_dragOverlay = false;
static IDropTarget* g_dropTarget = nullptr;
static bool g_dropComInit = false;

class GalleryDropTarget : public IDropTarget {
public:
    GalleryDropTarget() : m_ref(1) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD, POINTL, DWORD* pdwEffect) override {
        bool ok = HasHDrop(pDataObj);
        if (pdwEffect) *pdwEffect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (ok) g_dragOverlay = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* pdwEffect) override {
        if (pdwEffect) *pdwEffect = g_dragOverlay ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        g_dragOverlay = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD, POINTL, DWORD* pdwEffect) override {
        g_dragOverlay = false;
        if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
        if (pDataObj) {
            FORMATETC f = {};
            f.cfFormat = (CLIPFORMAT)CF_HDROP;
            f.dwAspect = DVASPECT_CONTENT;
            f.lindex = -1;
            f.tymed = TYMED_HGLOBAL;
            STGMEDIUM m = {};
            if (SUCCEEDED(pDataObj->GetData(&f, &m)) && m.hGlobal) {
                HDROP hDrop = static_cast<HDROP>(GlobalLock(m.hGlobal));
                if (hDrop) {
                    UINT n = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                    for (UINT i = 0; i < n; i++) {
                        wchar_t fp[MAX_PATH] = {};
                        if (DragQueryFileW(hDrop, i, fp, MAX_PATH)) {
                            std::string ext;
                            if (wchar_t* dot = wcsrchr(fp, L'.')) {
                                char mb[16] = {};
                                WideCharToMultiByte(CP_ACP, 0, dot, -1, mb, (int)sizeof(mb), nullptr, nullptr);
                                ext = mb;
                                for (auto& c : ext) c = (char)::tolower((unsigned char)c);
                            }
                            if (ext == ".mp4" || ext == ".webm" || ext == ".mkv" || ext == ".avi" || ext == ".mov") {
                                int sz = WideCharToMultiByte(CP_UTF8, 0, fp, -1, nullptr, 0, nullptr, nullptr);
                                std::string utf8(sz > 0 ? sz - 1 : 0, 0);
                                if (sz > 0) WideCharToMultiByte(CP_UTF8, 0, fp, -1, &utf8[0], sz, nullptr, nullptr);
                                RequestApplyVideo(utf8, "wallpaper");
                                break;
                            }
                        }
                    }
                    GlobalUnlock(m.hGlobal);
                }
                ReleaseStgMedium(&m);
            }
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> m_ref{1};

    static bool HasHDrop(IDataObject* obj) {
        if (!obj) return false;
        FORMATETC f = {};
        f.cfFormat = (CLIPFORMAT)CF_HDROP;
        f.dwAspect = DVASPECT_CONTENT;
        f.lindex = -1;
        f.tymed = TYMED_HGLOBAL;
        return obj->QueryGetData(&f) == S_OK;
    }
};

bool SettingsUI::Open(HINSTANCE hInstance) {
    if (g_hWnd) {
        ShowWindow(g_hWnd, SW_RESTORE);
        ShowWindow(g_hWnd, SW_SHOW);
        SetForegroundWindow(g_hWnd);
        g_isOpen = true;
        return true;
    }

    g_hInstance = hInstance;

    WNDCLASSEXW wc = {
        sizeof(wc),
        CS_CLASSDC,
        SettingsWndProc,
        0L, 0L,
        hInstance,
        LoadIconW(hInstance, MAKEINTRESOURCEW(1)),
        LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)), // IDC_ARROW
        nullptr, nullptr,
        L"MiniWallpaper_SettingsClass",
        nullptr
    };
    RegisterClassExW(&wc);

    // Skala ukuran window mengikuti DPI (tanpa ini Windows me-stretch bitmap → font pecah di 125%+).
    float dpiScale = 1.0f;
    {
        UINT dpi = 96;
        if (HDC hdc = GetDC(nullptr)) {
            int v = GetDeviceCaps(hdc, LOGPIXELSX);
            if (v > 0) dpi = (UINT)v;
            ReleaseDC(nullptr, hdc);
        }
        if (HMODULE hU32 = GetModuleHandleW(L"user32.dll")) {
            using GetSysDpiFn = UINT(WINAPI*)();
            if (auto fn = reinterpret_cast<GetSysDpiFn>(GetProcAddress(hU32, "GetDpiForSystem"))) {
                UINT d2 = fn();
                if (d2 > 0) dpi = d2;
            }
        }
        if (dpi < 96) dpi = 96;
        dpiScale = static_cast<float>(dpi) / 96.0f;
    }
    g_dpiScale = dpiScale;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int winW = (int)(1060 * dpiScale);
    int winH = (int)(730 * dpiScale);
    int winX = (sw > winW) ? (sw - winW) / 2 : 100;
    int winY = (sh > winH) ? (sh - winH) / 2 : 100;

    g_hWnd = CreateWindowExW(
        WS_EX_LAYERED,
        wc.lpszClassName,
        L"MiniWallpaper v2.0.0",
        WS_OVERLAPPEDWINDOW,
        winX, winY, winW, winH,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!g_hWnd || !CreateDeviceD3D(g_hWnd)) {
        CleanupDeviceD3D();
        if (g_hWnd) DestroyWindow(g_hWnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        g_hWnd = nullptr;
        return false;
    }

    SetLayeredWindowAttributes(g_hWnd, 0, 235, LWA_ALPHA);

    SendMessageW(g_hWnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    SendMessageW(g_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIcon);

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(g_hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    COLORREF captionColor = RGB(20, 20, 26);
    DwmSetWindowAttribute(g_hWnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

    DragAcceptFiles(g_hWnd, TRUE);

    // Liquid glass: blur konten di belakang window (diabaikan bila OS tidak mendukung).
    {
        DWM_BLURBEHIND bb = {};
        bb.dwFlags = DWM_BB_ENABLE;
        bb.fEnable = TRUE;
        DwmEnableBlurBehindWindow(g_hWnd, &bb);
    }

    // OLE drag & drop utk overlay "drop video here" (fallback: WM_DROPFILES tetap aktif).
    if (SUCCEEDED(OleInitialize(nullptr))) g_dropComInit = true;
    if (GalleryDropTarget* dt = new GalleryDropTarget()) {
        if (SUCCEEDED(RegisterDragDrop(g_hWnd, dt))) g_dropTarget = dt;
        else dt->Release();
    }

    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    SetupImGuiStyle();
    if (dpiScale > 1.01f) ImGui::GetStyle().ScaleAllSizes(dpiScale);

    ImFontConfig main_cfg;
    main_cfg.OversampleH = 3;
    main_cfg.OversampleV = 3;
    main_cfg.RasterizerMultiply = 1.12f;
    ImFont* mainFont = nullptr;
    if (fs::exists("C:\\Windows\\Fonts\\segoeui.ttf")) {
        mainFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * dpiScale, &main_cfg);
    }
    // Semibold utk judul (hierarki ala LuaTools); icon digabung ke tiap font yg memakainya.
    ImFontConfig bold_cfg = main_cfg;
    bold_cfg.RasterizerMultiply = 1.0f; // semibold sudah tebal
    if (fs::exists("C:\\Windows\\Fonts\\segoeuisb.ttf")) {
        g_fontSemibold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuisb.ttf", 17.0f * dpiScale, &bold_cfg);
    }
    if (!mainFont) {
        io.Fonts->AddFontDefault();
    }

    static const ImWchar icons_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    ImFontConfig icons_config;
    icons_config.MergeMode = true;
    icons_config.PixelSnapH = true;
    icons_config.GlyphMinAdvanceX = 16.0f;
    icons_config.OversampleH = 3;
    icons_config.OversampleV = 3;

    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    fs::path fontPath = fs::path(exePath).parent_path() / "assets" / "fa-solid-900.ttf";

    if (fs::exists(fontPath)) {
        io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), 15.0f * dpiScale, &icons_config, icons_ranges);
        if (g_fontSemibold) {
            ImFontConfig icons_config2 = icons_config;
            io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), 15.0f * dpiScale, &icons_config2, icons_ranges);
        }
    }

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    g_config.Load();
    g_isOpen = true;
    return true;
}

void SettingsUI::RenderFrame() {
    if (!g_isOpen || !g_hWnd || !IsWindowVisible(g_hWnd) || !g_pd3dDeviceContext || !g_mainRenderTargetView) return;

    FetchDaemonStatus();
    ThumbnailManager::Instance().Update(g_pd3dDevice);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    RECT rect;
    GetClientRect(g_hWnd, &rect);
    int winWidth = rect.right - rect.left;
    int winHeight = rect.bottom - rect.top;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(winWidth), static_cast<float>(winHeight)));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("MiniWallpaper Control Panel", nullptr, flags);

    RenderOptimizationProgress();

    // PageHost transparan & full-height; dock navigasi melayang overlay di atasnya.
    ImVec2 pageAvail = ImGui::GetContentRegionAvail();
    float pageH = pageAvail.y - 2.0f;
    if (pageH < 200.0f) pageH = pageAvail.y > 0 ? pageAvail.y : 200.0f;
    ImGuiWindowFlags pageFlags = (g_activePage == 0) ? (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse) : ImGuiWindowFlags_AlwaysVerticalScrollbar;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("PageHost", ImVec2(0, pageH), false, pageFlags);
    if (g_activePage == 0) RenderGalleryTab();
    else if (g_activePage == 1) RenderLockscreenTab();
    else if (g_activePage == 2) RenderGeneralSettingsTab();
    else RenderPerformancePanel();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    RenderFloatingDock();

    // Overlay drop-file (paling atas): muncul saat file diseret ke atas window.
    if (g_dragOverlay) {
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        ImDrawList* odl = ImGui::GetWindowDrawList();
        odl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), IM_COL32(9, 12, 21, 190));
        ImVec2 f0 = ImVec2(wp.x + 56.0f, wp.y + 64.0f);
        ImVec2 f1 = ImVec2(wp.x + ws.x - 56.0f, wp.y + ws.y - 64.0f);
        odl->AddRect(f0, f1, IM_COL32(56, 232, 255, 235), 20.0f, 0, 3.0f);
        odl->AddRect(ImVec2(f0.x + 9.0f, f0.y + 9.0f), ImVec2(f1.x - 9.0f, f1.y - 9.0f), IM_COL32(56, 232, 255, 70), 14.0f, 0, 1.5f);
        ImVec2 ctr = ImVec2((f0.x + f1.x) * 0.5f, (f0.y + f1.y) * 0.5f);
        const char* dropTitle = "Drop video here";
        const char* dropSub = "MP4  /  WebM  /  MKV  /  AVI  /  MOV";
        ImGui::SetWindowFontScale(2.4f);
        ImVec2 tIco = ImGui::CalcTextSize(ICON_FA_ARROW_DOWN_TO_LINE);
        ImGui::SetWindowFontScale(1.0f);
        ImVec2 tTitle = ImGui::CalcTextSize(dropTitle);
        ImVec2 tSub = ImGui::CalcTextSize(dropSub);
        float gap1 = 18.0f, gap2 = 10.0f;
        float blockH = tIco.y + gap1 + tTitle.y + gap2 + tSub.y;
        float cy = ctr.y - blockH * 0.5f;
        ImGui::SetWindowFontScale(2.4f);
        ImGui::SetCursorScreenPos(ImVec2(ctr.x - tIco.x * 0.5f, cy));
        ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f), ICON_FA_ARROW_DOWN_TO_LINE);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::SetCursorScreenPos(ImVec2(ctr.x - tTitle.x * 0.5f, cy + tIco.y + gap1));
        ImGui::TextColored(ImVec4(0.945f, 0.961f, 0.976f, 1.0f), "%s", dropTitle);
        ImGui::SetCursorScreenPos(ImVec2(ctr.x - tSub.x * 0.5f, cy + tIco.y + gap1 + tTitle.y + gap2));
        ImGui::TextColored(ImVec4(0.580f, 0.639f, 0.722f, 1.0f), "%s", dropSub);
    }

    RenderOptimizeModal();
    RenderCaptureFrameModal();

    ImGui::End();

    ImGui::Render();
    const float clearColor[4] = { 0.035f, 0.047f, 0.082f, 0.94f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    g_pSwapChain->Present(1, 0);
}

bool SettingsUI::IsOpen() {
    return g_isOpen && g_hWnd && IsWindowVisible(g_hWnd);
}

void SettingsUI::Close() {
    if (g_hWnd) {
        ShowWindow(g_hWnd, SW_HIDE);
    }
    g_isOpen = false;
}

void SettingsUI::Shutdown() {
    if (!g_hWnd) return;

    if (g_dropTarget) {
        RevokeDragDrop(g_hWnd);
        g_dropTarget->Release();
        g_dropTarget = nullptr;
    }
    if (g_dropComInit) { OleUninitialize(); g_dropComInit = false; }
    g_dragOverlay = false;

    g_isOpen = false;

    ThumbnailManager::Instance().ReleaseTextures();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();

    if (g_hWnd) {
        DestroyWindow(g_hWnd);
        UnregisterClassW(L"MiniWallpaper_SettingsClass", g_hInstance);
        g_hWnd = nullptr;
    }
}

HWND SettingsUI::GetHwnd() {
    return g_hWnd;
}

} // namespace miniwp
