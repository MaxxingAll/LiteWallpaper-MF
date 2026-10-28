#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "core/config.h"
#include "core/playback_clock.h"
#include "decoder/mf_decoder.h"
#include "platform/win32/d3d11_presenter.h"
#include "platform/win32/desktop_injector.h"
#include "platform/win32/power_governor.h"
#include "platform/win32/tray_icon.h"

#define WM_APP_ATTACH (WM_APP + 11)

using namespace miniwp;

namespace {

D3D11Presenter g_presenter;
DesktopInjector g_injector;
MFDecoder g_decoder;
PowerGovernor g_governor;
TrayIcon g_tray;
PlaybackClock g_clock;

HWND g_window = nullptr;
VideoFrame g_frame;
VideoInfo g_video_info;
bool g_running = true;
bool g_manual_paused = false;
bool g_suspended = false;
bool g_decoder_open = false;
bool g_has_presented_frame = false;
double g_playback_seconds = 0.0;
uint64_t g_decoded_frames = 0;
double g_decode_accumulator = 0.0;
ULONGLONG g_last_attach_attempt = 0;

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           value.c_str(), -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1,
                        result.data(), count);
    result.resize(static_cast<size_t>(count - 1));
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1,
                                           nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), count,
                        nullptr, nullptr);
    result.resize(static_cast<size_t>(count - 1));
    return result;
}

void UpdateTrayState() {
    const AppConfig& config = g_config.Get();
    g_tray.UpdateState(g_manual_paused, config.pause_on_fullscreen,
                       config.pause_on_maximized, config.pause_on_battery,
                       config.fps_cap, config.run_on_startup);
}

void ShowDecoderError(const std::string& fallback) {
    std::string message = g_decoder.GetLastError();
    if (message.empty()) message = fallback;
    const std::wstring wide = Utf8ToWide(message);
    MessageBoxW(nullptr, wide.c_str(), L"LiteWallpaper-MF",
                MB_OK | MB_ICONERROR);
}

bool SetRunAtStartup(bool enabled) {
    HKEY key = nullptr;
    constexpr wchar_t path[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    constexpr wchar_t name[] = L"LiteWallpaper-MF";
    LSTATUS status = ERROR_SUCCESS;
    if (enabled) {
        wchar_t executable[32768] = {};
        GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(_countof(executable)));
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --startup";
        status = RegSetValueExW(
            key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, name);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

int NativeFps() {
    if (g_video_info.fps < 1.0 || g_video_info.fps > 240.0) return 30;
    return static_cast<int>(std::lround(g_video_info.fps));
}

int EffectiveFpsCap(PowerState state) {
    int cap = g_config.Get().fps_cap;
    if (cap == 0) cap = NativeFps();
    if (state == PowerState::Reduced) cap = (std::min)(cap, 15);
    cap = (std::min)(cap, NativeFps());
    return (std::max)(1, cap);
}

bool OpenConfiguredVideo(double seek_seconds = 0.0) {
    g_presenter.ReleaseFrameResources();
    g_frame = {};
    g_decoder.Close();
    g_decoder_open = false;
    g_video_info = {};

    const std::string& path = g_config.Get().video_path;
    const std::wstring wide_path = Utf8ToWide(path);
    const DWORD attributes = GetFileAttributesW(wide_path.c_str());
    if (wide_path.empty() || attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    if (!g_decoder.Open(path.c_str(), g_presenter.GetDevice())) {
        ShowDecoderError("Windows could not open this video.");
        return false;
    }

    g_video_info = g_decoder.GetInfo();
    if (seek_seconds > 0.0) g_decoder.SeekTo(seek_seconds);
    g_playback_seconds = (std::max)(0.0, seek_seconds);
    g_decoded_frames = static_cast<uint64_t>(g_playback_seconds * NativeFps());
    g_decode_accumulator = 0.0;
    g_clock.SetTargetFPS(g_config.Get().fps_cap == 0
                             ? NativeFps()
                             : g_config.Get().fps_cap);
    g_clock.Reset();
    g_decoder_open = true;
    g_suspended = false;

    std::wstring tooltip = L"LiteWallpaper-MF - ";
    const size_t separator = wide_path.find_last_of(L"\\/");
    tooltip += separator == std::wstring::npos
                   ? wide_path
                   : wide_path.substr(separator + 1);
    g_tray.SetTooltip(tooltip.c_str());
    return true;
}

void EnterSuspend() {
    if (g_suspended) return;
    g_presenter.ReleaseFrameResources();
    g_frame = {};
    g_decoder.Close();
    g_decoder_open = false;
    g_suspended = true;
    SetProcessWorkingSetSize(GetCurrentProcess(),
                             static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

void LeaveSuspend() {
    if (!g_suspended) return;
    g_suspended = false;
    OpenConfiguredVideo(g_playback_seconds);
}

bool RenderNextFrame(PowerState power_state) {
    if (!g_decoder_open) return false;
    const int cap = EffectiveFpsCap(power_state);
    if (g_clock.GetTargetFPS() != cap) g_clock.SetTargetFPS(cap);
    if (!g_clock.ShouldRenderFrame()) return false;

    const double native_fps = g_video_info.fps > 0.0 ? g_video_info.fps : 30.0;
    g_decode_accumulator += native_fps / cap;
    int frames = static_cast<int>(std::floor(g_decode_accumulator));
    g_decode_accumulator -= frames;
    frames = std::clamp(frames, 1, 16);
    bool decoded = false;
    for (int i = 0; i < frames; ++i) {
        if (!g_decoder.DecodeNextFrame(g_frame)) break;
        decoded = true;
        ++g_decoded_frames;
    }
    if (!decoded || !g_frame.texture) {
        if (!g_decoder.GetLastError().empty()) {
            ShowDecoderError("Video decoding failed.");
            g_decoder.Close();
            g_decoder_open = false;
        }
        return false;
    }

    g_playback_seconds = g_frame.pts >= 0
                             ? static_cast<double>(g_frame.pts) / 1000000.0
                             : static_cast<double>(g_decoded_frames) / native_fps;
    if (g_video_info.duration_seconds > 0.0) {
        g_playback_seconds = std::fmod(g_playback_seconds,
                                      g_video_info.duration_seconds);
    }
    if (!g_presenter.RenderFrame(g_frame.texture, g_frame.texture_index,
                                 g_config.Get().scaling_mode)) {
        return false;
    }
    if (FAILED(g_presenter.Present(0))) return false;
    if (!g_has_presented_frame) {
        g_has_presented_frame = true;
        ShowWindow(g_window, SW_SHOWNA);
    }
    return true;
}

std::wstring ChooseVideoFile() {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return {};
    }
    const COMDLG_FILTERSPEC filters[] = {
        {L"Video files", L"*.mp4;*.webm;*.mkv;*.mov;*.avi"},
        {L"All files", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(_countof(filters)), filters);
    dialog->SetTitle(L"Choose wallpaper video");
    if (FAILED(dialog->Show(nullptr))) return {};

    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return {};
    PWSTR selected = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected))) return {};
    std::wstring result(selected);
    CoTaskMemFree(selected);
    return result;
}

void ResizeToDesktop() {
    if (!g_window) return;
    HWND parent = g_injector.GetWorkerW();
    RECT rect = {};
    if (!parent || !GetClientRect(parent, &rect)) {
        rect.right = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        rect.bottom = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }
    const int width = (std::max)(1L, rect.right - rect.left);
    const int height = (std::max)(1L, rect.bottom - rect.top);
    SetWindowPos(g_window, HWND_BOTTOM, 0, 0, width, height,
                 SWP_NOACTIVATE);
    if (g_presenter.Resize(width, height)) {
        g_has_presented_frame = false;
        ShowWindow(g_window, SW_HIDE);
    }
}

void OnTrayAction(TrayAction action) {
    AppConfig& config = g_config.Get();
    switch (action) {
        case TrayAction::ChooseVideo: {
            const std::wstring selected = ChooseVideoFile();
            if (!selected.empty()) {
                config.video_path = WideToUtf8(selected);
                g_config.Save();
                g_manual_paused = false;
                if (!OpenConfiguredVideo()) {
                    g_has_presented_frame = false;
                    ShowWindow(g_window, SW_HIDE);
                }
            }
            break;
        }
        case TrayAction::PauseResume:
            g_manual_paused = !g_manual_paused;
            break;
        case TrayAction::TogglePauseFullscreen:
            config.pause_on_fullscreen = !config.pause_on_fullscreen;
            g_config.Save();
            break;
        case TrayAction::TogglePauseMaximized:
            config.pause_on_maximized = !config.pause_on_maximized;
            g_config.Save();
            break;
        case TrayAction::TogglePauseBattery:
            config.pause_on_battery = !config.pause_on_battery;
            g_config.Save();
            break;
        case TrayAction::Fps15: config.fps_cap = 15; g_config.Save(); break;
        case TrayAction::Fps30: config.fps_cap = 30; g_config.Save(); break;
        case TrayAction::Fps60: config.fps_cap = 60; g_config.Save(); break;
        case TrayAction::FpsNative: config.fps_cap = 0; g_config.Save(); break;
        case TrayAction::ToggleStartup: {
            const bool requested = !config.run_on_startup;
            if (SetRunAtStartup(requested)) {
                config.run_on_startup = requested;
                g_config.Save();
            }
            break;
        }
        case TrayAction::OpenConfigFolder:
            ShellExecuteW(g_window, L"open", Config::GetConfigDir().c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case TrayAction::Exit:
            g_running = false;
            PostMessageW(g_window, WM_CLOSE, 0, 0);
            break;
    }
    UpdateTrayState();
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
    if (message == WM_APP_TRAYICON) {
        g_tray.HandleMessage(lparam);
        return 0;
    }
    if (message == WM_APP_ATTACH) {
        g_injector.Reattach(window);
        ResizeToDesktop();
        ShowWindow(window, g_has_presented_frame ? SW_SHOWNA : SW_HIDE);
        g_tray.Destroy();
        g_tray.Create(window, OnTrayAction);
        UpdateTrayState();
        return 0;
    }
    switch (message) {
        case WM_WTSSESSION_CHANGE:
            g_governor.HandleSessionChange(wparam);
            return 0;
        case WM_POWERBROADCAST:
            g_governor.HandlePowerChange(wparam);
            return TRUE;
        case WM_DISPLAYCHANGE:
            ResizeToDesktop();
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            g_running = false;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize_com = SUCCEEDED(com_result);

    HANDLE instance_mutex = CreateMutexW(nullptr, TRUE,
                                         L"Local\\LiteWallpaper-MF.Instance");
    if (!instance_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (instance_mutex) CloseHandle(instance_mutex);
        if (uninitialize_com) CoUninitialize();
        return 0;
    }

    g_config.Load();
    if (g_config.Get().run_on_startup) SetRunAtStartup(true);
    int argument_count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments) {
        for (int i = 1; i + 1 < argument_count; ++i) {
            if (_wcsicmp(arguments[i], L"--video") == 0) {
                g_config.Get().video_path = WideToUtf8(arguments[++i]);
                g_config.Save();
            }
        }
        LocalFree(arguments);
    }

    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    window_class.hIconSm = window_class.hIcon;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = L"LiteWallpaperMFWindow";
    if (!RegisterClassExW(&window_class)) {
        CloseHandle(instance_mutex);
        if (uninitialize_com) CoUninitialize();
        return 1;
    }

    const int virtual_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtual_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtual_width = (std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int virtual_height = (std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    g_window = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, window_class.lpszClassName,
        L"LiteWallpaper-MF", WS_POPUP, virtual_x, virtual_y, virtual_width,
        virtual_height, nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        CloseHandle(instance_mutex);
        if (uninitialize_com) CoUninitialize();
        return 1;
    }

    g_injector.Attach(g_window);
    g_injector.RegisterExplorerRestart(g_window);
    ResizeToDesktop();
    ShowWindow(g_window, SW_HIDE);
    RECT client = {};
    GetClientRect(g_window, &client);
    if (!g_presenter.Init(g_window, (std::max)(1L, client.right),
                          (std::max)(1L, client.bottom))) {
        MessageBoxW(nullptr, L"Direct3D 11 video presentation is unavailable.",
                    L"LiteWallpaper-MF", MB_OK | MB_ICONERROR);
        DestroyWindow(g_window);
        CloseHandle(instance_mutex);
        if (uninitialize_com) CoUninitialize();
        return 1;
    }

    g_governor.Init(g_window);
    g_tray.Create(g_window, OnTrayAction);
    UpdateTrayState();
    if (!g_config.Get().video_path.empty()) OpenConfiguredVideo();

    MSG message = {};
    while (g_running) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) g_running = false;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!g_running) break;

        const ULONGLONG now = GetTickCount64();
        if (now - g_last_attach_attempt >= 5000) {
            g_last_attach_attempt = now;
            if (!g_injector.IsAttachedValid() &&
                g_injector.Reattach(g_window)) {
                ResizeToDesktop();
                ShowWindow(g_window,
                           g_has_presented_frame ? SW_SHOWNA : SW_HIDE);
            }
        }

        const AppConfig& config = g_config.Get();
        const PowerState power = g_governor.GetCurrentState(
            config.pause_on_fullscreen, config.pause_on_maximized,
            config.pause_on_battery);
        const bool governor_paused = power == PowerState::Paused ||
                                     power == PowerState::Occluded ||
                                     power == PowerState::Sleeping;
        if (g_manual_paused || governor_paused) {
            EnterSuspend();
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 200, QS_ALLINPUT);
            continue;
        }
        if (g_suspended) LeaveSuspend();

        if (g_decoder_open) {
            RenderNextFrame(power);
            DWORD wait_ms = (std::min)(16u, g_clock.GetSleepDurationMs());
            if (wait_ms == 0) wait_ms = 1;
            HANDLE frame_wait = g_presenter.GetWaitableObject();
            if (frame_wait) {
                MsgWaitForMultipleObjectsEx(1, &frame_wait, wait_ms, QS_ALLINPUT,
                                            MWMO_INPUTAVAILABLE);
            } else {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, wait_ms, QS_ALLINPUT);
            }
        } else {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 200, QS_ALLINPUT);
        }
    }

    g_presenter.ReleaseFrameResources();
    g_frame = {};
    g_decoder.Close();
    g_tray.Destroy();
    g_governor.Shutdown();
    HWND desktop_window = g_injector.GetWorkerW();
    g_injector.Detach();
    if (desktop_window && IsWindow(desktop_window)) {
        RedrawWindow(desktop_window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    g_presenter.Cleanup();
    if (IsWindow(g_window)) DestroyWindow(g_window);
    ReleaseMutex(instance_mutex);
    CloseHandle(instance_mutex);
    if (uninitialize_com) CoUninitialize();
    return 0;
}
