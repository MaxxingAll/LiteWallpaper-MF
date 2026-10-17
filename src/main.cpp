// MiniWallpaper — Ultra-lightweight animated video wallpaper engine
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <thread>
#include <mutex>
#include <cmath>
#include <filesystem>
#include <mimalloc.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

#include "core/config.h"
#include "core/playback_clock.h"
#include "core/ipc_server.h"
#include "core/logger.h"
#include "core/engine_state.h"
#include "core/video_optimizer.h"
#include "decoder/ffmpeg_hw_decoder.h"
#include "decoder/audio_player.h"
#include "platform/win32/desktop_injector.h"
#include "platform/win32/d3d11_presenter.h"
#include "platform/win32/power_governor.h"
#include "platform/win32/tray_icon.h"
#include "platform/win32/hardware_info.h"
#include "platform/win32/lockscreen_manager.h"
#include "ui/settings_app.h"

#define WM_APP_OPEN_SETTINGS (WM_APP + 10)
#define WM_APP_ATTACH        (WM_APP + 11)
#define WM_APP_DEATTACH      (WM_APP + 12)

using namespace miniwp;

// Global engine state
static PlaybackClock     g_clock;
static DesktopInjector   g_injector;
static D3D11Presenter    g_presenter;
static FFmpegHWDecoder   g_decoder;
static AudioPlayer       g_audio;
static PowerGovernor     g_governor;
static TrayIcon          g_tray;
static IpcServer         g_ipc;
static LockScreenManager g_lockscreen;

static std::mutex        g_decoder_mutex; // Guards g_decoder and g_current_frame against race conditions

static bool g_running = true;
static bool g_paused = false;
static HWND g_main_hwnd = nullptr;
static VideoFrame g_current_frame;
static bool g_inject_ok = false;
static bool g_decoder_hw = false;
static std::string g_last_error;
static uint64_t g_frames_rendered = 0;
static int64_t g_flash_until_us = 0; // Diagnostic flash window (microseconds)

// Playback pacing state
static double g_video_fps = 0.0;
static int    g_frame_skip = 1;
static uint64_t g_frames_decoded = 0;

static bool g_fullscreen_paused = false;
static std::string g_loaded_video_path; // Last video successfully opened in the decoder

static int ComputeFrameSkip(double video_fps, int display_fps) {
    if (video_fps <= 0.0 || video_fps > 240.0) return 1;
    if (display_fps <= 0) return 1;
    if (display_fps >= video_fps) return 1;
    double skip = std::round(video_fps / display_fps);
    if (skip < 1.0) skip = 1.0;
    if (skip > 4.0) skip = 4.0; // Max safety clamp: never decode > 4 frames per tick
    return static_cast<int>(skip);
}

static void SetVideoPacing(double video_fps) {
    if (video_fps <= 0.0 || video_fps > 240.0) video_fps = 30.0;
    g_video_fps = video_fps;
    auto& cfg = g_config.Get();
    int fps = (cfg.target_fps > 0) ? cfg.target_fps : static_cast<int>(video_fps + 0.5);
    if (fps < 1) fps = 1;
    if (fps > 120) fps = 120;
    g_clock.SetTargetFPS(fps);
    g_frames_decoded = 0;
}

// Forward declarations
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void OnTrayAction(TrayAction action);
std::string OnIpcRequest(const std::string& json);
static bool OpenWallpaperVideo(const std::string& path);

static size_t GetProcessMemoryUsageMB() {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize / (1024 * 1024);
    }
    return 0;
}

static double GetProcessCpuUsagePercent() {
    static ULARGE_INTEGER lastCPU = {0}, lastSysCPU = {0}, lastUserCPU = {0};
    static int numProcessors = -1;
    static bool first = true;
    static double last_percent = 0.0;

    if (first) {
        SYSTEM_INFO sysInfo;
        FILETIME ftime, fsys, fuser;
        GetSystemInfo(&sysInfo);
        numProcessors = (sysInfo.dwNumberOfProcessors > 0) ? sysInfo.dwNumberOfProcessors : 1;
        GetSystemTimeAsFileTime(&ftime);
        memcpy(&lastCPU, &ftime, sizeof(FILETIME));
        GetProcessTimes(GetCurrentProcess(), &ftime, &ftime, &fsys, &fuser);
        memcpy(&lastSysCPU, &fsys, sizeof(FILETIME));
        memcpy(&lastUserCPU, &fuser, sizeof(FILETIME));
        first = false;
        return 0.0;
    }

    FILETIME ftime, fsys, fuser;
    ULARGE_INTEGER now, sys, user;
    GetSystemTimeAsFileTime(&ftime);
    memcpy(&now, &ftime, sizeof(FILETIME));
    GetProcessTimes(GetCurrentProcess(), &ftime, &ftime, &fsys, &fuser);
    memcpy(&sys, &fsys, sizeof(FILETIME));
    memcpy(&user, &fuser, sizeof(FILETIME));

    if (now.QuadPart > lastCPU.QuadPart) {
        double percent = (double)((sys.QuadPart - lastSysCPU.QuadPart) + (user.QuadPart - lastUserCPU.QuadPart));
        percent /= (now.QuadPart - lastCPU.QuadPart);
        percent /= numProcessors;
        last_percent = percent * 100.0;
        if (last_percent < 0.0) last_percent = 0.0;
        if (last_percent > 100.0) last_percent = 100.0;
        lastCPU = now;
        lastUserCPU = user;
        lastSysCPU = sys;
    }
    return last_percent;
}

static size_t GetProcessVramUsageMB(int vid_w, int vid_h, int screen_w, int screen_h, bool hw_decode) {
    if (!hw_decode || vid_w <= 0 || vid_h <= 0) return 12;
    size_t frame_bytes = (size_t)vid_w * vid_h * 3 / 2;
    size_t pool_bytes = 4 * frame_bytes; // 4 surfaces in minimal hardware pool
    size_t swapchain_bytes = 2 * (size_t)screen_w * screen_h * 4; // 2 backbuffers
    return (pool_bytes + swapchain_bytes) / (1024 * 1024) + 6; // +6MB driver/shader overhead
}

static void TrimWorkingSetMemory() {
    mi_collect(false);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static void OpenWallpaperDialog() {
    wchar_t filename[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = L"Video Files (*.mp4;*.webm;*.mkv;*.avi;*.mov)\0*.mp4;*.webm;*.mkv;*.avi;*.mov\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameW(&ofn)) {
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, filename, -1, NULL, 0, NULL, NULL);
        std::string utf8_path(size_needed - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, filename, -1, &utf8_path[0], size_needed, NULL, NULL);

        auto& cfg = g_config.Get();
        if (cfg.wallpapers.empty()) {
            MonitorWallpaper mw;
            mw.video_path = utf8_path;
            cfg.wallpapers.push_back(mw);
        } else {
            cfg.wallpapers[0].video_path = utf8_path;
        }
        cfg.AddToGallery(utf8_path);
        g_config.Save();

        OpenWallpaperVideo(utf8_path);
        TrimWorkingSetMemory();
    }
}

static bool g_first_frame_captured = false;
static bool g_desktop_snapshot_synced = false;

static bool OpenWallpaperVideo(const std::string& path) {
    auto& cfg = g_config.Get();
    bool audio_on = !cfg.wallpapers.empty() && cfg.wallpapers[0].audio_enabled && (cfg.wallpapers[0].volume > 0.0f);

    std::lock_guard<std::mutex> lock(g_decoder_mutex);
    g_current_frame = VideoFrame{};
    g_presenter.ResetStartFrame();
    g_first_frame_captured = false;
    g_desktop_snapshot_synced = false;
    g_decoder.Close();
    g_decoder.SetAudioEnabled(audio_on);
    g_decoder.SetForceSoftware(cfg.gpu_device_index == -1);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    bool ok = g_decoder.Open(path.c_str(), g_presenter.GetDevice(), vw, vh);
    g_decoder_hw = g_decoder.IsHWAccelerated();
    g_paused = false;
    g_clock.Reset();
    g_loaded_video_path.clear();
    if (!ok) {
        g_last_error = "Failed to open video: " + path;
        Logger::Info(g_last_error);
        return false;
    }
    g_loaded_video_path = path;
    auto info = g_decoder.GetInfo();
    g_last_error.clear();
    SetVideoPacing(info.fps);
    Logger::Info("Opened video OK: ", path,
                 " codec=", info.codec_name,
                 " ", info.width, "x", info.height,
                 " fps=", info.fps,
                 " hw=", g_decoder_hw ? "yes" : "no");
    return true;
}

static uint64_t g_pause_start_us = 0;
static bool g_deep_suspended = false;

static void SuspendWallpaper(const std::string& reason = "Fullscreen app", const std::string& detail = "") {
    if (g_fullscreen_paused) return;
    g_fullscreen_paused = true;
    g_pause_start_us = g_clock.GetCurrentTimeMicros();
    g_audio.Stop();
    if (!detail.empty()) {
        Logger::Info("Playback auto-paused: ", reason, " [", detail, "]");
    } else {
        Logger::Info("Playback auto-paused: ", reason);
    }
}

static void ResumeWallpaper() {
    if (!g_fullscreen_paused) return;
    g_fullscreen_paused = false;
    g_shared_engine_state.paused.store(false);
    g_pause_start_us = 0;
    if (g_deep_suspended) {
        g_deep_suspended = false;
        Logger::Info("Resumed from deep suspend (VRAM/RAM restored)");
    }

    auto& cfg = g_config.Get();
    // Self-healing: if a wallpaper was requested while playback was suspended
    // (e.g. Play clicked during Gaming Auto-Sleep / fullscreen pause), load it now
    // so the user's selection starts playing without requiring another click.
    bool need_load = false;
    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        need_load = g_loaded_video_path.empty() && !cfg.wallpapers.empty() && !cfg.wallpapers[0].video_path.empty();
    }
    if (need_load) {
        OpenWallpaperVideo(cfg.wallpapers[0].video_path);
    }
    if (!cfg.wallpapers.empty() && cfg.wallpapers[0].audio_enabled && cfg.wallpapers[0].volume > 0.0f) {
        g_audio.Init();
    }
    g_clock.Reset();

    // Ensure desktop injection is valid upon resume
    if (g_main_hwnd && (!g_inject_ok || !g_injector.IsAttachedValid())) {
        g_injector.Reattach(g_main_hwnd);
        g_inject_ok = g_injector.IsAttached();
        if (g_inject_ok) {
            ShowWindow(g_main_hwnd, SW_SHOW);
            RECT rc;
            if (GetClientRect(g_main_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
                g_presenter.Resize(rc.right - rc.left, rc.bottom - rc.top);
            }
        }
    }
    Logger::Info("Playback auto-resumed: Desktop is visible and active");
}

static POINT g_scr_mouse_init = {};
static bool g_scr_mouse_init_set = false;

static LRESULT CALLBACK ScreensaverWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            GetCursorPos(&g_scr_mouse_init);
            g_scr_mouse_init_set = true;
            return 0;
        }
        case WM_SETCURSOR: {
            SetCursor(nullptr);
            return TRUE;
        }
        case WM_MOUSEMOVE: {
            if (!g_scr_mouse_init_set) {
                GetCursorPos(&g_scr_mouse_init);
                g_scr_mouse_init_set = true;
                return 0;
            }
            POINT pt;
            GetCursorPos(&pt);
            if (abs(pt.x - g_scr_mouse_init.x) > 15 || abs(pt.y - g_scr_mouse_init.y) > 15) {
                PostQuitMessage(0);
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int RunScreensaverMode(HINSTANCE hInstance) {
    timeBeginPeriod(1);

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = ScreensaverWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"MiniWallpaper_Screensaver";
    wc.hCursor = nullptr;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        wc.lpszClassName,
        L"MiniWallpaper Screensaver",
        WS_POPUP | WS_VISIBLE,
        vx, vy, vw, vh,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) {
        timeEndPeriod(1);
        return 1;
    }

    ShowCursor(FALSE);

    D3D11Presenter presenter;
    if (!presenter.Init(hwnd, vw, vh)) {
        ShowCursor(TRUE);
        DestroyWindow(hwnd);
        timeEndPeriod(1);
        return 1;
    }

    Config config;
    config.Load();
    auto& cfg = config.Get();
    std::string video_path = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";
    if (video_path.empty() || !fs::exists(video_path)) {
        ShowCursor(TRUE);
        DestroyWindow(hwnd);
        timeEndPeriod(1);
        return 0;
    }

    FFmpegHWDecoder decoder;
    if (!decoder.Open(video_path.c_str(), presenter.GetDevice(), vw, vh)) {
        ShowCursor(TRUE);
        DestroyWindow(hwnd);
        timeEndPeriod(1);
        return 1;
    }

    PlaybackClock clock;
    clock.SetTargetFPS((cfg.target_fps > 0) ? cfg.target_fps : 60);

    VideoFrame frame;
    MSG msg = {};
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        if (clock.ShouldRenderFrame()) {
            if (decoder.DecodeNextFrame(frame)) {
                if (frame.texture) {
                    presenter.RenderFrame(frame.texture, frame.texture_index, cfg.scaling_mode);
                    presenter.Present(1);
                }
            } else {
                decoder.SeekToStart();
                clock.Reset();
            }
        } else {
            uint32_t sleep_ms = clock.GetSleepDurationMs();
            if (sleep_ms > 0) Sleep(sleep_ms);
            else YieldProcessor();
        }
    }

    ShowCursor(TRUE);
    decoder.Close();
    presenter.Cleanup();
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    timeEndPeriod(1);
    return 0;
}

// One-time rebranding migration: move legacy %APPDATA%\LiteWallpaper data
// (config, gallery, logs, thumbnails, optimized caches) to %APPDATA%\MiniWallpaper
// so existing users keep their setup after the LiteWallpaper -> MiniWallpaper rename.
static void MigrateLegacyAppData() {
    wchar_t appDataPath[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath))) return;

    fs::path legacyDir = fs::path(appDataPath) / L"LiteWallpaper";
    fs::path newDir = fs::path(appDataPath) / L"MiniWallpaper";

    std::error_code ec;
    if (fs::exists(newDir, ec)) return;     // Already migrated or fresh install
    if (!fs::exists(legacyDir, ec)) return; // Nothing to migrate

    fs::rename(legacyDir, newDir, ec);
    if (ec) {
        // Fallback: cross-volume move or locked file -> recursive copy, then remove source
        ec.clear();
        fs::copy(legacyDir, newDir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            Logger::Info("Legacy data migration failed: ", ec.message());
            return;
        }
        std::error_code removeEc;
        fs::remove_all(legacyDir, removeEc);
    }
    Logger::Info("Migrated legacy LiteWallpaper appdata to MiniWallpaper");
}

// Rewrite a REG_SZ registry value that still points into the legacy
// %APPDATA%\LiteWallpaper root, but only when the rewritten target file exists.
static void FixLegacyRegSzPath(HKEY rootKey, const wchar_t* subKey, const wchar_t* valueName,
                               const std::wstring& oldRoot, const std::wstring& newRoot) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(rootKey, subKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return;

    wchar_t val[1024] = {};
    DWORD size = sizeof(val);
    DWORD type = 0;
    if (RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(val), &size) == ERROR_SUCCESS && type == REG_SZ) {
        std::wstring value(val);
        size_t pos = value.find(oldRoot);
        if (pos != std::wstring::npos) {
            std::wstring fixed = value.substr(0, pos) + newRoot + value.substr(pos + oldRoot.size());
            if (GetFileAttributesW(fixed.c_str()) != INVALID_FILE_ATTRIBUTES) {
                RegSetValueExW(hKey, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(fixed.c_str()),
                               static_cast<DWORD>((fixed.length() + 1) * sizeof(wchar_t)));
            }
        }
    }
    RegCloseKey(hKey);
}

// One-time rebranding migration for registry values that keep absolute paths
// into the appdata cache: Windows desktop wallpaper image, lock screen image
// (Lock Screen\Creative + PersonalizationCSP keys) and the companion screensaver
// binary. Idempotent - safe to run on every startup.
static void MigrateLegacyRegistryPaths() {
    wchar_t appDataPath[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath))) return;
    std::wstring oldRoot = std::wstring(appDataPath) + L"\\LiteWallpaper";
    std::wstring newRoot = std::wstring(appDataPath) + L"\\MiniWallpaper";

    // Desktop wallpaper image
    FixLegacyRegSzPath(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"Wallpaper", oldRoot, newRoot);

    // Screensaver binary: LiteWallpaper.scr -> MiniWallpaper.scr (only if the new file exists)
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        wchar_t val[1024] = {};
        DWORD size = sizeof(val);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, L"SCRNSAVE.EXE", nullptr, &type, reinterpret_cast<LPBYTE>(val), &size) == ERROR_SUCCESS && type == REG_SZ) {
            std::wstring value(val);
            size_t pos = value.find(L"LiteWallpaper.scr");
            if (pos != std::wstring::npos) {
                std::wstring fixed = value.substr(0, pos) + L"MiniWallpaper.scr" + value.substr(pos + wcslen(L"LiteWallpaper.scr"));
                if (GetFileAttributesW(fixed.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    RegSetValueExW(hKey, L"SCRNSAVE.EXE", 0, REG_SZ, reinterpret_cast<const BYTE*>(fixed.c_str()),
                                   static_cast<DWORD>((fixed.length() + 1) * sizeof(wchar_t)));
                }
            }
        }
        RegCloseKey(hKey);
    }

    // Windows 10/11 lock screen image registries
    const wchar_t* creativeKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Lock Screen\\Creative";
    FixLegacyRegSzPath(HKEY_CURRENT_USER, creativeKey, L"LockScreenImage", oldRoot, newRoot);
    FixLegacyRegSzPath(HKEY_CURRENT_USER, creativeKey, L"LandscapeAssetPath", oldRoot, newRoot);
    FixLegacyRegSzPath(HKEY_CURRENT_USER, creativeKey, L"PortraitAssetPath", oldRoot, newRoot);

    const wchar_t* cspKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\PersonalizationCSP";
    FixLegacyRegSzPath(HKEY_CURRENT_USER, cspKey, L"LockScreenImagePath", oldRoot, newRoot);
    FixLegacyRegSzPath(HKEY_CURRENT_USER, cspKey, L"LockScreenImageUrl", oldRoot, newRoot);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPWSTR lpCmdLine, int /*nCmdShow*/) {
    // DPI awareness agar teks tajam & tidak di-stretch DWM (penting di scaling 125%/150%+).
    // Aman di semua OS: API dimuat dinamis + fallback berlapis, hasil gagal diabaikan.
    {
        bool dpiSet = false;
        if (HMODULE hU32 = GetModuleHandleW(L"user32.dll")) {
            using SetCtxFn = BOOL(WINAPI*)(HANDLE); // DPI_AWARENESS_CONTEXT = handle
            if (auto fn = reinterpret_cast<SetCtxFn>(GetProcAddress(hU32, "SetProcessDpiAwarenessContext"))) {
                dpiSet = (fn((HANDLE)-4) != FALSE) || (fn((HANDLE)-2) != FALSE); // PerMonitorV2, lalu SystemAware
            } else {
                using LegacyFn = BOOL(WINAPI*)();
                if (auto legacy = reinterpret_cast<LegacyFn>(GetProcAddress(hU32, "SetProcessDPIAware")))
                    dpiSet = (legacy() != FALSE);
            }
        }
        if (!dpiSet) {
            if (HMODULE hShcore = LoadLibraryW(L"shcore.dll")) {
                using SetProcFn = HRESULT(WINAPI*)(int); // 2 = PROCESS_PER_MONITOR_DPI_AWARE
                if (auto fn = reinterpret_cast<SetProcFn>(GetProcAddress(hShcore, "SetProcessDpiAwareness")))
                    fn(2);
                FreeLibrary(hShcore);
            }
        }
    }

    // Ultra-low footprint mimalloc tuning:
    // Prevent memory buildup in thread caches and OS page reservations
    mi_option_set(mi_option_arena_eager_commit, 0);       // Disable eager commit of arenas
    mi_option_set(mi_option_page_commit_on_demand, 1);    // Commit page memory strictly on-demand
    mi_option_set(mi_option_reserve_huge_os_pages, 0);    // No huge pages
    mi_option_set(mi_option_purge_decommits, 1);          // Purge actually decommits memory to OS
    mi_option_set(mi_option_purge_delay, 0);              // Zero purge delay (immediate release)

    // 0. Explicitly lock Current Working Directory to executable directory
    wchar_t exePathBuf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
    fs::path exeDirPath = fs::path(exePathBuf).parent_path();
    if (!exeDirPath.empty()) {
        SetCurrentDirectoryW(exeDirPath.c_str());
    }

    // Helper for exact command-line switch matching (e.g. /s, -s, /c, -c, /p, -p)
    auto MatchArg = [](const wchar_t* cmd, const wchar_t* arg) -> bool {
        if (!cmd || !arg) return false;
        const wchar_t* p = cmd;
        size_t len = wcslen(arg);
        while ((p = wcsstr(p, arg)) != nullptr) {
            bool start_ok = (p == cmd || *(p - 1) == L' ' || *(p - 1) == L'\t');
            wchar_t next = *(p + len);
            bool end_ok = (next == L'\0' || next == L' ' || next == L'\t' || next == L':');
            if (start_ok && end_ok) return true;
            p += len;
        }
        return false;
    };

    // Handle Windows Screensaver switches (/s, /c, /p)
    if (lpCmdLine) {
        if (MatchArg(lpCmdLine, L"/s") || MatchArg(lpCmdLine, L"/S") ||
            MatchArg(lpCmdLine, L"-s") || MatchArg(lpCmdLine, L"-S")) {
            return RunScreensaverMode(hInstance);
        }
        if (MatchArg(lpCmdLine, L"/c") || MatchArg(lpCmdLine, L"/C") ||
            MatchArg(lpCmdLine, L"-c") || MatchArg(lpCmdLine, L"-C")) {
            IpcClient client;
            std::string resp = client.SendRequest("{\"cmd\":\"open_settings\"}");
            if (!resp.empty()) {
                return 0;
            }
            SettingsUI::Open(hInstance);
            MSG msg;
            while (SettingsUI::IsOpen() && GetMessageW(&msg, nullptr, 0, 0)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                SettingsUI::RenderFrame();
            }
            SettingsUI::Shutdown();
            return 0;
        }
        if (MatchArg(lpCmdLine, L"/p") || MatchArg(lpCmdLine, L"/P") ||
            MatchArg(lpCmdLine, L"-p") || MatchArg(lpCmdLine, L"-P")) {
            return 0;
        }
    }

    bool is_silent_boot = (lpCmdLine && (wcsstr(lpCmdLine, L"--startup") != nullptr || wcsstr(lpCmdLine, L"-startup") != nullptr));

    int targetPage = -1;
    if (lpCmdLine) {
        const wchar_t* p = wcsstr(lpCmdLine, L"--page");
        if (!p) p = wcsstr(lpCmdLine, L"-page");
        if (p) {
            p += (wcsstr(p, L"--page") == p) ? 6 : 5;
            while (*p == L' ' || *p == L'=' || *p == L':') p++;
            if (*p >= L'0' && *p <= L'9') targetPage = _wtoi(p);
        }
    }
    if (targetPage >= 0) {
        SettingsUI::SetActivePage(targetPage);
    }

    // Single-Instance Check
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"MiniWallpaper_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!is_silent_boot) {
            IpcClient client;
            nlohmann::json r = {{"cmd", "open_settings"}};
            if (targetPage >= 0) r["page"] = targetPage;
            client.SendRequest(r.dump());
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    timeBeginPeriod(1);

    // One-time rebranding migration from legacy "LiteWallpaper" naming
    // (appdata folder + autostart registry entries + legacy startup shortcut)
    MigrateLegacyAppData();
    MigrateLegacyRegistryPaths();
    WindowsAutostart::MigrateLegacy();

    // 1. Load Configuration
    g_config.Load();
    auto& cfg = g_config.Get();

    // Auto-repair registry startup path if portable folder was moved
    if (cfg.run_on_startup) {
        WindowsAutostart::AutoRepairIfMoved(cfg.startup_priority);
    }

    // 2. Register Background Render Window Class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"MiniWallpaper_Daemon";
    RegisterClassExW(&wc);

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    g_main_hwnd = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"MiniWallpaper_Daemon",
        L"MiniWallpaper",
        WS_POPUP,
        vx, vy, vw, vh,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_main_hwnd) {
        timeEndPeriod(1);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 1;
    }

    SendMessageW(g_main_hwnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    SendMessageW(g_main_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);

    // 3. Inject window behind desktop icons (WorkerW) BEFORE DXGI initialization
    g_inject_ok = g_injector.Attach(g_main_hwnd);
    g_injector.RegisterExplorerRestart(g_main_hwnd);
    if (g_inject_ok) {
        ShowWindow(g_main_hwnd, SW_SHOW);
        UpdateWindow(g_main_hwnd);
    } else {
        ShowWindow(g_main_hwnd, SW_HIDE);
    }
    Logger::Info("Attach: ", g_inject_ok ? "OK" : "FAILED",
                 " workerw=0x", reinterpret_cast<size_t>(g_injector.GetWorkerW()),
                 " visible=", IsWindowVisible(g_main_hwnd) ? "yes" : "no");
    if (!g_inject_ok) {
        Logger::Info("Desktop injection failed, will retry periodically");
    }

    // 4. Initialize Direct3D 11 Presenter
    if (!g_presenter.Init(g_main_hwnd, vw, vh, cfg.gpu_device_index)) {
        g_last_error = "D3D11 presenter init failed";
        Logger::Info(g_last_error);
        timeEndPeriod(1);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 1;
    }
    g_shared_engine_state.active_gpu_index.store(cfg.gpu_device_index);
    Logger::Info("Presenter init OK (gpu_index=", cfg.gpu_device_index, "), swapchain size=", vw, "x", vh);

    g_flash_until_us = 0;

    // 5. Open video if available
    if (!cfg.wallpapers.empty() && !cfg.wallpapers[0].video_path.empty()) {
        OpenWallpaperVideo(cfg.wallpapers[0].video_path);
    }

    // Sync static visuals (Desktop & Lock Screen) based on individual configuration
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (cfg.update_desktop_wallpaper) {
        std::string d_vid = (cfg.desktop_static_source_mode == 1 && !cfg.desktop_static_video_path.empty())
            ? cfg.desktop_static_video_path
            : (!cfg.wallpapers.empty() ? cfg.wallpapers[0].video_path : "");
        double d_ts = (cfg.desktop_static_source_mode == 1) ? cfg.desktop_static_timestamp : 1.0;
        if (!d_vid.empty()) {
            g_lockscreen.SyncFromVideoAsync(d_vid, sw, sh, false, true, d_ts);
        }
    }
    if (cfg.update_lockscreen) {
        std::string l_vid = (cfg.lockscreen_source_mode == 1 && !cfg.lockscreen_video_path.empty())
            ? cfg.lockscreen_video_path
            : (!cfg.wallpapers.empty() ? cfg.wallpapers[0].video_path : "");
        double l_ts = (cfg.lockscreen_source_mode == 1) ? cfg.lockscreen_timestamp : 1.0;
        if (!l_vid.empty()) {
            g_lockscreen.SyncFromVideoAsync(l_vid, sw, sh, true, false, l_ts);
        }
    }

    // 6. Setup audio parameters
    if (!cfg.wallpapers.empty()) {
        g_audio.SetVolume(cfg.wallpapers[0].volume);
        g_audio.SetMuted(!cfg.wallpapers[0].audio_enabled);
        if (cfg.wallpapers[0].audio_enabled && cfg.wallpapers[0].volume > 0.0f) {
            g_audio.Init();
        }
    }

    // 7. Setup Power Governor
    g_governor.Init(g_main_hwnd);

    // 8. Setup System Tray Icon
    g_tray.Create(g_main_hwnd, OnTrayAction);

    // 9. Start IPC Server
    g_ipc.Start(OnIpcRequest);

    // 10. Open Settings UI on first manual launch (silent when launched by Windows boot via --startup)
    if (!is_silent_boot) {
        SettingsUI::Open(hInstance);
    }

    TrimWorkingSetMemory();

    // 11. Main Event & Render Loop
    MSG msg = {};
    float audio_buffer[4096 * 2];
    uint64_t last_trim_us = 0;
    uint64_t last_state_update_us = 0;

    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!g_running) break;

        // Render Settings UI if open.
        // Rate-limited: ~60 fps target when focused, ~15 fps when in background
        // (avoids burning CPU/GPU on an occluded window, e.g. while gaming).
        if (SettingsUI::IsOpen()) {
            static uint64_t last_ui_render_us = 0;
            uint64_t now_ui_us = g_clock.GetCurrentTimeMicros();
            bool ui_focused = (GetForegroundWindow() == SettingsUI::GetHwnd());
            uint64_t ui_interval_us = ui_focused ? 16000 : 66000;
            if (now_ui_us - last_ui_render_us >= ui_interval_us) {
                last_ui_render_us = now_ui_us;
                SettingsUI::RenderFrame();
            }
        }

        // Recreate render window if Explorer destroyed it
        if (!g_main_hwnd && g_running) {
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            g_main_hwnd = CreateWindowExW(
                WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                L"MiniWallpaper_Daemon",
                L"MiniWallpaper",
                WS_POPUP,
                vx, vy, vw, vh,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr
            );
            if (g_main_hwnd) {
                g_presenter.Init(g_main_hwnd, vw, vh, cfg.gpu_device_index);
            }
        }

        // Re-attach if desktop was rebuilt, or retry injection if it initially failed (runs UN-GATED so it never misses boot)
        static uint64_t last_inject_check_us = 0;
        uint64_t now_us = g_clock.GetCurrentTimeMicros();
        bool has_active_wallpaper = (!cfg.wallpapers.empty() && !cfg.wallpapers[0].video_path.empty() && !g_paused);

        if (has_active_wallpaper && g_main_hwnd && (now_us - last_inject_check_us >= 1000000)) {
            last_inject_check_us = now_us;

            if (!g_inject_ok) {
                // Retry initial injection
                g_inject_ok = g_injector.Attach(g_main_hwnd);
                if (g_inject_ok) {
                    Logger::Info("Injection retry succeeded, workerw=0x",
                                 reinterpret_cast<size_t>(g_injector.GetWorkerW()));
                    g_last_error.clear();
                    ShowWindow(g_main_hwnd, SW_SHOW);
                    UpdateWindow(g_main_hwnd);
                    RECT rc;
                    if (GetClientRect(g_main_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
                        g_presenter.Resize(rc.right - rc.left, rc.bottom - rc.top);
                    }
                }
            } else if (!g_injector.IsAttachedValid()) {
                // Desktop hierarchy changed, re-attach
                Logger::Info("Desktop hierarchy changed, re-attaching wallpaper to active WorkerW");
                g_injector.Reattach(g_main_hwnd);
                g_inject_ok = g_injector.IsAttached();
                if (g_inject_ok && g_main_hwnd) {
                    ShowWindow(g_main_hwnd, SW_SHOW);
                    UpdateWindow(g_main_hwnd);
                    RECT rc;
                    if (GetClientRect(g_main_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
                        g_presenter.Resize(rc.right - rc.left, rc.bottom - rc.top);
                    }
                }
            }
        }

        PowerState power = g_governor.GetCurrentState(
            cfg.pause_on_maximized,
            cfg.pause_on_resource_heavy,
            cfg.resource_ram_threshold_pct,
            cfg.resource_vram_threshold_pct,
            g_presenter.GetDevice()
        );

        // Update Shared Engine State for instant zero-latency UI reads
        if (now_us - last_state_update_us >= 250000) { // Every 250ms
            last_state_update_us = now_us;
            std::string cur_vid = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";
            auto info = g_decoder.GetInfo();
            size_t ram = GetProcessMemoryUsageMB();
            double cpu = GetProcessCpuUsagePercent();
            size_t vram = g_presenter.GetVramUsageMB();
            if (vram == 0 && g_decoder_hw) {
                vram = GetProcessVramUsageMB(info.width, info.height, vw, vh, g_decoder_hw);
            }

            g_shared_engine_state.connected.store(true);
            g_shared_engine_state.playing.store(!g_paused && !cur_vid.empty() && !g_fullscreen_paused && g_inject_ok);
            g_shared_engine_state.paused.store(g_paused && !cur_vid.empty());
            g_shared_engine_state.injected.store(g_inject_ok);
            g_shared_engine_state.hw_decode.store(g_decoder_hw);
            g_shared_engine_state.fps.store(g_clock.GetTargetFPS());
            g_shared_engine_state.video_fps.store(info.fps);
            g_shared_engine_state.width.store(info.width);
            g_shared_engine_state.height.store(info.height);
            g_shared_engine_state.duration.store(info.duration_seconds);
            g_shared_engine_state.ram_mb.store(ram);
            g_shared_engine_state.vram_mb.store(vram);
            g_shared_engine_state.cpu_percent.store(cpu);
            g_shared_engine_state.system_ram_percent.store(g_governor.GetSystemRamPercent());
            g_shared_engine_state.gpu_vram_percent.store(g_governor.GetGpuVramPercent());
            g_shared_engine_state.resource_heavy_sleep.store(g_governor.IsInResourceSleep());
            g_shared_engine_state.frames_rendered.store(g_frames_rendered);
            g_shared_engine_state.frames_decoded.store(g_frames_decoded);
            g_shared_engine_state.frame_skip.store(g_frame_skip);
            g_shared_engine_state.SetCurrentVideo(cur_vid);
            g_shared_engine_state.SetCodec(info.codec_name);
            g_shared_engine_state.SetLastError(g_last_error);
        }

        // Periodic process working set memory trimming every 1 second
        if (now_us - last_trim_us >= 1000000) {
            last_trim_us = now_us;
            TrimWorkingSetMemory();
        }

        // Window guard: Never render to a detached top-level window (prevents white empty window)
        if (!g_inject_ok) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            continue;
        }

        bool fullscreen_active = (power == PowerState::Paused && cfg.pause_on_fullscreen);
        bool occluded_active = (power == PowerState::Occluded && cfg.pause_on_maximized);
        bool battery_pause_active = (power == PowerState::Reduced && cfg.pause_on_battery);
        bool lock_pause_active = (power == PowerState::Sleeping && cfg.pause_on_lock);
        bool resource_pause_active = (power == PowerState::ResourceHeavy && cfg.pause_on_resource_heavy);

        if (fullscreen_active || occluded_active || battery_pause_active || lock_pause_active || resource_pause_active) {
            if (!g_fullscreen_paused) {
                std::string detail = g_governor.GetLastTriggerInfo();
                if (resource_pause_active) {
                    SuspendWallpaper("High system load (Gaming Auto-Sleep)", detail);
                } else if (occluded_active) {
                    SuspendWallpaper("Maximized window / desktop covered", detail);
                } else if (fullscreen_active) {
                    SuspendWallpaper("Fullscreen app / 3D game", detail);
                } else if (lock_pause_active) {
                    SuspendWallpaper("Workstation locked", detail);
                } else {
                    SuspendWallpaper("Battery saver", detail);
                }
            }

            // Deep Suspend: If paused due to resource load, trigger immediately! Or if paused for >5s
            bool trigger_deep_suspend = resource_pause_active || (g_pause_start_us > 0 && (now_us - g_pause_start_us >= 5000000));
            if (!g_deep_suspended && trigger_deep_suspend) {
                g_deep_suspended = true;
                {
                    std::lock_guard<std::mutex> lock(g_decoder_mutex);
                    g_decoder.FlushBuffers();
                    g_current_frame = VideoFrame{};
                    g_presenter.ResetStartFrame();
                    g_first_frame_captured = false;
                }
                TrimWorkingSetMemory();
                Logger::Info("Deep suspend active: released decoder VRAM & trimmed working set");
            }

            // Keep the settings UI responsive while the wallpaper is suspended
            DWORD idleWaitMs = SettingsUI::IsOpen() ? 16 : 100;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, idleWaitMs, QS_ALLINPUT);
            continue;
        } else if (g_fullscreen_paused) {
            ResumeWallpaper();
        }

        // Apply battery vs AC FPS throttling
        if (power == PowerState::Reduced && cfg.battery_fps > 0) {
            if (g_clock.GetTargetFPS() != static_cast<uint32_t>(cfg.battery_fps)) {
                g_clock.SetTargetFPS(cfg.battery_fps);
            }
        } else if (power == PowerState::Active && cfg.target_fps > 0) {
            if (g_clock.GetTargetFPS() != static_cast<uint32_t>(cfg.target_fps)) {
                g_clock.SetTargetFPS(cfg.target_fps);
            }
        }

        bool should_pause = (power == PowerState::Sleeping) ||
                            (power == PowerState::Reduced && cfg.pause_on_battery) ||
                            g_paused;

        if (should_pause) {
            if (g_pause_start_us == 0) {
                g_pause_start_us = now_us;
            }
            if (!g_deep_suspended && (now_us - g_pause_start_us >= 5000000)) {
                g_deep_suspended = true;
                {
                    std::lock_guard<std::mutex> lock(g_decoder_mutex);
                    g_decoder.FlushBuffers();
                    g_current_frame = VideoFrame{};
                    g_presenter.ResetStartFrame();
                    g_first_frame_captured = false;
                }
                TrimWorkingSetMemory();
                Logger::Info("Deep suspend active: released decoder VRAM & trimmed working set");
            }
            // Keep the settings UI responsive while playback is paused
            DWORD idleWaitMs = SettingsUI::IsOpen() ? 16 : 100;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, idleWaitMs, QS_ALLINPUT);
            continue;
        } else if (!g_fullscreen_paused && g_deep_suspended) {
            g_deep_suspended = false;
            g_pause_start_us = 0;
            Logger::Info("Resumed from deep suspend");
        }

        int display_fps_cap = (power == PowerState::Reduced) ? cfg.battery_fps : cfg.target_fps;
        g_frame_skip = ComputeFrameSkip(g_video_fps, display_fps_cap);

        if (g_clock.ShouldRenderFrame()) {
            if (g_clock.GetCurrentTimeMicros() < g_flash_until_us) {
                g_presenter.ClearAndPresent(0.22f, 0.91f, 1.00f);
                g_frames_rendered++;
                DWORD sleepMs = g_clock.GetSleepDurationMs();
                HANDLE waitable = g_presenter.GetWaitableObject();
                if (waitable) {
                    DWORD waitTimeout = (sleepMs > 0 && sleepMs < 100) ? sleepMs : 16;
                    MsgWaitForMultipleObjectsEx(1, &waitable, waitTimeout, QS_ALLINPUT, MWMO_ALERTABLE);
                } else if (sleepMs > 0) {
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, sleepMs, QS_ALLINPUT);
                }
                continue;
            }

            std::lock_guard<std::mutex> lock(g_decoder_mutex);
            bool decode_ok = false;
            int frames_to_decode = (g_frame_skip > 0) ? g_frame_skip : 1;
            for (int step = 0; step < frames_to_decode; ++step) {
                if (g_decoder.DecodeNextFrame(g_current_frame)) {
                    g_frames_decoded++;
                    decode_ok = true;
                } else {
                    break;
                }
            }

            if (decode_ok) {
                if (g_current_frame.texture) {
                    if (!g_first_frame_captured) {
                        g_presenter.CaptureStartFrame(g_current_frame.texture, g_current_frame.texture_index);
                        g_first_frame_captured = true;
                        TrimWorkingSetMemory();
                    }

                    // Track current playback time in engine state
                    auto info = g_decoder.GetInfo();
                    if (info.fps > 0.0 && info.duration_seconds > 0.0) {
                        double total_frames = info.duration_seconds * info.fps;
                        if (total_frames > 0.0) {
                            double current_frame_pos = static_cast<double>(g_frames_decoded % static_cast<uint64_t>(total_frames + 0.5));
                            double current_time_sec = current_frame_pos / info.fps;
                            g_shared_engine_state.current_time_sec.store(current_time_sec);
                        }
                    }

                    // Calculate Auto Smooth Loop crossfade blend alpha & dynamic speed ramp
                    float blend_alpha = 0.0f;
                    if (cfg.auto_smooth_loop && info.duration_seconds > 1.5 && info.fps > 0.0) {
                        double total_frames = info.duration_seconds * info.fps;
                        if (total_frames > 0.0) {
                            double current_frame_pos = static_cast<double>(g_frames_decoded % static_cast<uint64_t>(total_frames + 0.5));
                            double current_time_sec = current_frame_pos / info.fps;

                            float fade_dur = 0.8f;
                            bool enable_speed_ramp = false;
                            float min_speed = 0.75f;
                            int easing_mode = 1;

                            if (cfg.loop_preset == 0) { // Cinematic Speed Ramp
                                fade_dur = 1.2f;
                                enable_speed_ramp = true;
                                min_speed = 0.75f;
                                easing_mode = 1; // Smoothstep
                            } else if (cfg.loop_preset == 1) { // Smoothstep S-Curve
                                fade_dur = 0.8f;
                                enable_speed_ramp = false;
                                easing_mode = 1; // Smoothstep
                            } else if (cfg.loop_preset == 2) { // Gentle Flow
                                fade_dur = 1.8f;
                                enable_speed_ramp = true;
                                min_speed = 0.85f;
                                easing_mode = 2; // Sine
                            } else if (cfg.loop_preset == 3) { // Instant Snap
                                fade_dur = 0.4f;
                                enable_speed_ramp = false;
                                easing_mode = 0; // Linear
                            } else { // Custom Tuning
                                fade_dur = std::clamp(cfg.smooth_loop_duration, 0.2f, 2.5f);
                                enable_speed_ramp = cfg.loop_speed_ramp;
                                min_speed = std::clamp(cfg.loop_min_speed, 0.5f, 0.95f);
                                easing_mode = cfg.loop_easing_curve;
                            }

                            double time_remaining = info.duration_seconds - current_time_sec;
                            if (time_remaining >= 0.0 && time_remaining <= fade_dur) {
                                float raw_alpha = static_cast<float>(1.0 - (time_remaining / fade_dur));
                                
                                // Apply Easing Curve
                                if (easing_mode == 1) {
                                    // Smoothstep S-Curve: 3x^2 - 2x^3
                                    blend_alpha = raw_alpha * raw_alpha * (3.0f - 2.0f * raw_alpha);
                                } else if (easing_mode == 2) {
                                    // Sine In-Out: 0.5 * (1 - cos(pi * x))
                                    blend_alpha = 0.5f * (1.0f - cosf(raw_alpha * 3.14159265f));
                                } else if (easing_mode == 3) {
                                    // Smootherstep (Ken Perlin): 6x^5 - 15x^4 + 10x^3
                                    blend_alpha = raw_alpha * raw_alpha * raw_alpha * (raw_alpha * (raw_alpha * 6.0f - 15.0f) + 10.0f);
                                } else {
                                    // Linear
                                    blend_alpha = raw_alpha;
                                }

                                // Apply Dynamic Speed Ramp (Time Warp)
                                if (enable_speed_ramp) {
                                    // Sine bell curve: decelerate smoothly towards seam, then accelerate back to 1.0x
                                    float ramp_factor = sinf(raw_alpha * 3.14159265f);
                                    double target_speed = 1.0 - (1.0 - static_cast<double>(min_speed)) * ramp_factor;
                                    g_clock.SetSpeedMultiplier(target_speed);
                                } else {
                                    g_clock.SetSpeedMultiplier(1.0);
                                }
                            } else {
                                g_clock.SetSpeedMultiplier(1.0);
                            }
                        }
                    } else {
                        g_clock.SetSpeedMultiplier(1.0);
                    }

                    std::vector<DisplayViewport> target_vps;
                    if (!cfg.target_displays.empty()) {
                        auto all_displays = HardwareDetector::GetDisplayList();
                        int virt_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
                        int virt_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
                        for (int disp_idx : cfg.target_displays) {
                            if (disp_idx >= 0 && disp_idx < static_cast<int>(all_displays.size())) {
                                const auto& d = all_displays[disp_idx];
                                target_vps.push_back({
                                    static_cast<float>(d.pos_x - virt_x),
                                    static_cast<float>(d.pos_y - virt_y),
                                    static_cast<float>(d.width),
                                    static_cast<float>(d.height)
                                });
                            }
                        }
                    }

                    g_presenter.RenderFrame(g_current_frame.texture, g_current_frame.texture_index, cfg.scaling_mode, target_vps, blend_alpha);

                    if (FAILED(g_presenter.Present(0))) {
                        if (g_last_error.empty()) {
                            g_last_error = "Present() failed (DXGI error)";
                            Logger::Info(g_last_error);
                        }
                    }
                    g_frames_rendered++;
                } else if (!g_current_frame.texture && g_last_error.empty()) {
                    g_last_error = "Decoded frame has no GPU texture (HW decode inactive)";
                    Logger::Info(g_last_error);
                }

                if (!g_audio.IsMuted() && g_decoder.HasAudio()) {
                    int samples = 0;
                    while ((samples = g_decoder.DecodeAudioSamples(audio_buffer, 4096)) > 0) {
                        g_audio.PushSamples(audio_buffer, samples);
                    }
                }
            }
        } else {
            DWORD sleepMs = g_clock.GetSleepDurationMs();
            HANDLE waitable = g_presenter.GetWaitableObject();
            if (waitable) {
                DWORD waitTimeout = (sleepMs > 0 && sleepMs < 100) ? sleepMs : 16;
                MsgWaitForMultipleObjectsEx(1, &waitable, waitTimeout, QS_ALLINPUT, MWMO_ALERTABLE);
            } else if (sleepMs > 0) {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, sleepMs, QS_ALLINPUT);
            }
        }
    }

    // Cleanup
    SettingsUI::Shutdown();
    g_video_optimizer.Cancel();
    g_ipc.Stop();
    g_tray.Destroy();
    g_audio.Stop();
    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        g_decoder.Close();
    }
    g_presenter.Cleanup();
    g_injector.Detach();

    timeEndPeriod(1);
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_APP_OPEN_SETTINGS) {
        SettingsUI::Open(GetModuleHandleW(nullptr));
        return 0;
    }

    if (msg == g_injector.GetTaskbarRestartMsg()) {
        g_tray.Create(g_main_hwnd, OnTrayAction);
        g_injector.Reattach(g_main_hwnd);
        g_inject_ok = g_injector.IsAttached();
        if (g_inject_ok && g_main_hwnd) {
            ShowWindow(g_main_hwnd, SW_SHOW);
            RECT rc;
            if (GetClientRect(g_main_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
                g_presenter.Resize(rc.right - rc.left, rc.bottom - rc.top);
            }
        }
        return 0;
    }

    if (msg == WM_APP_ATTACH) {
        if (g_main_hwnd) {
            if (!g_injector.IsAttached() || !g_injector.IsAttachedValid()) {
                g_injector.Reattach(g_main_hwnd);
                g_inject_ok = g_injector.IsAttached();
            }
            if (g_inject_ok) {
                ShowWindow(g_main_hwnd, SW_SHOW);
                RECT rc;
                if (GetClientRect(g_main_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
                    g_presenter.Resize(rc.right - rc.left, rc.bottom - rc.top);
                }
            }
        }
        return 0;
    }

    if (msg == WM_APP_DEATTACH) {
        if (g_main_hwnd) {
            g_injector.Detach();
            ShowWindow(g_main_hwnd, SW_HIDE);
            g_inject_ok = false;
            // Repaint the desktop window hierarchy to reveal the underlying native desktop
            HWND shell = GetShellWindow();
            if (shell) InvalidateRect(shell, nullptr, TRUE);
            InvalidateRect(GetDesktopWindow(), nullptr, TRUE);
            RedrawWindow(nullptr, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        }
        return 0;
    }

    switch (msg) {
        case WM_WTSSESSION_CHANGE:
            g_governor.HandleSessionChange(wParam);
            return 0;

        case WM_POWERBROADCAST:
            g_governor.HandlePowerChange(wParam);
            return 0;

        case WM_DISPLAYCHANGE: {
            RECT rc = {};
            int w = 0, h = 0;
            HWND parent = g_injector.GetWorkerW();
            if (parent && GetClientRect(parent, &rc) && rc.right > 0 && rc.bottom > 0) {
                w = rc.right - rc.left;
                h = rc.bottom - rc.top;
            } else {
                w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            }
            if (g_main_hwnd && w > 0 && h > 0) {
                SetWindowPos(g_main_hwnd, HWND_BOTTOM, 0, 0, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                g_presenter.Resize(w, h);
            }
            Logger::Info("WM_DISPLAYCHANGE: adapted resolution to ", w, "x", h);
            return 0;
        }

        case WM_APP_TRAYICON:
            g_tray.HandleMessage(wParam, lParam);
            return 0;

        case WM_DESTROY:
            if (hwnd == g_main_hwnd) {
                Logger::Info("Main render window received WM_DESTROY from shell");
                g_main_hwnd = nullptr;
                g_inject_ok = false;
            }
            if (!g_running) {
                PostQuitMessage(0);
            }
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void OnTrayAction(TrayAction action) {
    switch (action) {
        case TrayAction::PauseResume:
            g_paused = !g_paused;
            if (!g_paused) g_clock.Reset();
            break;
        case TrayAction::MuteUnmute:
            g_audio.SetMuted(!g_audio.IsMuted());
            if (!g_audio.IsMuted() && !g_audio.IsRunning()) {
                g_audio.Init();
            }
            break;
        case TrayAction::OpenSettings:
            SettingsUI::Open(GetModuleHandleW(nullptr));
            break;
        case TrayAction::ChangeWallpaper:
            OpenWallpaperDialog();
            break;
        case TrayAction::Exit:
            g_running = false;
            PostQuitMessage(0);
            break;
    }
}

std::string OnIpcRequest(const std::string& request_json) {
    auto req = nlohmann::json::parse(request_json, nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        return "{\"ok\":false,\"error\":\"invalid JSON request\"}";
    }

    std::string cmd = req.value("cmd", "");

    if (cmd == "open_settings") {
        if (req.contains("page") && req["page"].is_number_integer()) {
            SettingsUI::SetActivePage(req["page"].get<int>());
        }
        if (g_main_hwnd) {
            PostMessageW(g_main_hwnd, WM_APP_OPEN_SETTINGS, 0, 0);
        }
        return "{\"ok\":true}";
    } else if (cmd == "set_wallpaper") {
        std::string path = req.value("path", "");
        if (!path.empty()) {
            auto& cfg = g_config.Get();
            if (cfg.wallpapers.empty()) {
                MonitorWallpaper mw;
                mw.video_path = path;
                cfg.wallpapers.push_back(mw);
            } else {
                cfg.wallpapers[0].video_path = path;
            }
            cfg.AddToGallery(path);
            g_config.Save();

            // Explicit user action (Play): load the video even while playback is
            // auto-paused (Gaming Auto-Sleep / fullscreen / etc). Rendering stays
            // governed, and deep suspend re-releases decoder VRAM until the system
            // load clears - then the chosen wallpaper starts playing automatically.
            bool ok = OpenWallpaperVideo(path);
            if (ok) {
                if (g_fullscreen_paused) {
                    g_deep_suspended = false; // let the render loop re-flush & trim VRAM while paused
                }
                TrimWorkingSetMemory();
                if (g_main_hwnd) {
                    PostMessageW(g_main_hwnd, WM_APP_ATTACH, 0, 0);
                }
                if (cfg.update_desktop_wallpaper || cfg.update_lockscreen) {
                    int sw = GetSystemMetrics(SM_CXSCREEN);
                    int sh = GetSystemMetrics(SM_CYSCREEN);
                    bool sync_desk = cfg.update_desktop_wallpaper && (cfg.desktop_static_source_mode == 0);
                    bool sync_lock = cfg.update_lockscreen && (cfg.lockscreen_source_mode == 0);
                    if (sync_desk || sync_lock) {
                        g_lockscreen.SyncFromVideoAsync(path, sw, sh, sync_lock, sync_desk, 1.0);
                    }
                }
            }
            return nlohmann::json{{"ok", ok}, {"hw", g_decoder_hw}}.dump();
        }
    } else if (cmd == "test_render") {
        g_flash_until_us = g_clock.GetCurrentTimeMicros() + 1500000;
        return nlohmann::json{{"ok", true}}.dump();
    } else if (cmd == "set_scaling") {
        int mode = req.value("mode", 0);
        g_config.Get().scaling_mode = mode;
        g_config.Save();
        return "{\"ok\":true}";
    } else if (cmd == "sync_desktop_wallpaper") {
        auto& cfg = g_config.Get();

        // Check for reset-to-auto commands
        if (req.value("reset_desktop_auto", false)) {
            cfg.desktop_static_source_mode = 0;
            cfg.update_desktop_wallpaper = true;
            g_config.Save();
            std::string cur_vid = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";
            if (!cur_vid.empty()) {
                int sw = GetSystemMetrics(SM_CXSCREEN);
                int sh = GetSystemMetrics(SM_CYSCREEN);
                g_lockscreen.SyncFromVideoAsync(cur_vid, sw, sh, false, true, 1.0);
            }
            return "{\"ok\":true}";
        }
        if (req.value("reset_lockscreen_auto", false)) {
            cfg.lockscreen_source_mode = 0;
            cfg.update_lockscreen = true;
            g_config.Save();
            std::string cur_vid = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";
            if (!cur_vid.empty()) {
                int sw = GetSystemMetrics(SM_CXSCREEN);
                int sh = GetSystemMetrics(SM_CYSCREEN);
                g_lockscreen.SyncFromVideoAsync(cur_vid, sw, sh, true, false, 1.0);
            }
            return "{\"ok\":true}";
        }

        std::string cur_vid = req.value("path", "");
        if (cur_vid.empty()) {
            cur_vid = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";
        }
        bool sync_desk = req.value("desktop", true);
        bool sync_lock = req.value("lockscreen", cfg.update_lockscreen);
        double ts = req.value("timestamp", 1.0);
        int save_mode = req.value("save_mode", -1);

        if (save_mode == 1) {
            if (sync_desk) {
                cfg.desktop_static_source_mode = 1;
                cfg.desktop_static_video_path = cur_vid;
                cfg.desktop_static_timestamp = ts;
                cfg.update_desktop_wallpaper = true;
            }
            if (sync_lock) {
                cfg.lockscreen_source_mode = 1;
                cfg.lockscreen_video_path = cur_vid;
                cfg.lockscreen_timestamp = ts;
                cfg.update_lockscreen = true;
            }
            g_config.Save();
        }

        if (!cur_vid.empty() && (sync_desk || sync_lock)) {
            int sw = GetSystemMetrics(SM_CXSCREEN);
            int sh = GetSystemMetrics(SM_CYSCREEN);
            g_lockscreen.SyncFromVideoAsync(cur_vid, sw, sh, sync_lock, sync_desk, ts);
        }
        return "{\"ok\":true}";
    } else if (cmd == "get_status") {
        size_t ram = GetProcessMemoryUsageMB();
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        auto info = g_decoder.GetInfo();
        auto& cfg = g_config.Get();
        std::string current_video = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";
        return nlohmann::json{
            {"ok", true},
            {"playing", !g_paused && !current_video.empty() && !g_fullscreen_paused},
            {"paused", g_paused && !current_video.empty()},
            {"fullscreen_paused", g_fullscreen_paused},
            {"current_video", current_video},
            {"fps", g_clock.GetTargetFPS()},
            {"video_fps", info.fps},
            {"width", info.width},
            {"height", info.height},
            {"duration", info.duration_seconds},
            {"current_time_sec", g_shared_engine_state.current_time_sec.load()},
            {"desktop_static_source_mode", cfg.desktop_static_source_mode},
            {"desktop_static_video_path", cfg.desktop_static_video_path},
            {"desktop_static_timestamp", cfg.desktop_static_timestamp},
            {"lockscreen_source_mode", cfg.lockscreen_source_mode},
            {"lockscreen_video_path", cfg.lockscreen_video_path},
            {"lockscreen_timestamp", cfg.lockscreen_timestamp},
            {"codec", info.codec_name},
            {"ram_mb", ram},
            {"injected", g_inject_ok},
            {"hw_decode", g_decoder_hw},
            {"system_ram_pct", g_governor.GetSystemRamPercent()},
            {"gpu_vram_pct", g_governor.GetGpuVramPercent()},
            {"resource_sleep", g_governor.IsInResourceSleep()},
            {"frames_rendered", g_frames_rendered},
            {"frames_decoded", g_frames_decoded},
            {"frame_skip", g_frame_skip},
            {"last_error", g_last_error}
        }.dump();
    } else if (cmd == "pause") {
        g_paused = true;
        return "{\"ok\":true}";
    } else if (cmd == "resume") {
        g_paused = false;
        g_clock.Reset();
        if (g_main_hwnd) {
            PostMessageW(g_main_hwnd, WM_APP_ATTACH, 0, 0);
        }
        return "{\"ok\":true}";
    } else if (cmd == "stop") {
        g_audio.Stop();
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        g_decoder.Close();
        g_current_frame = VideoFrame{};
        g_loaded_video_path.clear();
        g_paused = true;
        SetVideoPacing(0.0);
        auto& cfg = g_config.Get();
        if (!cfg.wallpapers.empty()) {
            cfg.wallpapers[0].video_path = "";
            g_config.Save();
        }
        g_shared_engine_state.playing.store(false);
        g_shared_engine_state.paused.store(false);
        g_shared_engine_state.SetCurrentVideo("");
        g_shared_engine_state.fps.store(0);
        g_shared_engine_state.video_fps.store(0.0);
        if (g_main_hwnd) {
            PostMessageW(g_main_hwnd, WM_APP_DEATTACH, 0, 0);
        }
        TrimWorkingSetMemory();
        return "{\"ok\":true}";
    } else if (cmd == "set_volume") {
        float vol = req.value("volume", 0.0f);
        g_audio.SetVolume(vol);
        if (vol > 0.0f && !g_audio.IsRunning()) {
            g_audio.Init();
        }
        g_decoder.SetAudioEnabled(vol > 0.0f && !g_audio.IsMuted());
        return "{\"ok\":true}";
    } else if (cmd == "set_fps") {
        int fps = req.value("fps", 30);
        if (fps < 1) fps = 1;
        g_config.Get().target_fps = fps;
        g_clock.SetTargetFPS(fps);
        g_config.Save();
        return "{\"ok\":true}";
    } else if (cmd == "set_render_device") {
        int gpu_idx = req.value("gpu_index", 0);
        auto& cfg = g_config.Get();
        cfg.gpu_device_index = gpu_idx;
        g_config.Save();

        std::string current_video = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";

        {
            std::lock_guard<std::mutex> lock(g_decoder_mutex);
            g_decoder.Close();
            g_current_frame = VideoFrame{};
            g_loaded_video_path.clear();

            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            g_presenter.Cleanup();
            g_presenter.Init(g_main_hwnd, vw, vh, gpu_idx);

            g_decoder.SetForceSoftware(gpu_idx == -1);
            if (!current_video.empty()) {
                bool audio_on = !cfg.wallpapers.empty() && cfg.wallpapers[0].audio_enabled && (cfg.wallpapers[0].volume > 0.0f);
                g_decoder.SetAudioEnabled(audio_on);
                g_decoder.Open(current_video.c_str(), g_presenter.GetDevice(), vw, vh);
                g_loaded_video_path = current_video;
                g_decoder_hw = g_decoder.IsHWAccelerated();
                g_clock.Reset();
            }
            g_shared_engine_state.active_gpu_index.store(gpu_idx);
            g_shared_engine_state.hw_decode.store(g_decoder_hw);
        }
        return "{\"ok\":true}";
    } else if (cmd == "set_target_displays") {
        auto& cfg = g_config.Get();
        cfg.target_displays.clear();
        if (req.contains("displays") && req["displays"].is_array()) {
            for (const auto& item : req["displays"]) {
                if (item.is_number_integer()) {
                    cfg.target_displays.push_back(item.get<int>());
                }
            }
        }
        g_config.Save();
        return "{\"ok\":true}";
    } else if (cmd == "reload_config") {
        g_config.Load();
        auto& cfg = g_config.Get();
        if (cfg.target_fps > 0) {
            g_clock.SetTargetFPS(cfg.target_fps);
        }
        if (!cfg.wallpapers.empty()) {
            float vol = cfg.wallpapers[0].volume;
            g_audio.SetVolume(vol);
            g_decoder.SetAudioEnabled(vol > 0.0f && !g_audio.IsMuted());
        }
        return "{\"ok\":true}";
    }

    return "{\"ok\":true}";
}
