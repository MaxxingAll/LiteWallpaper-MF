#pragma once

#include <windows.h>

#define WM_APP_TRAYICON (WM_APP + 1)

namespace miniwp {

enum class TrayAction {
    ChooseVideo,
    PauseResume,
    TogglePauseFullscreen,
    TogglePauseMaximized,
    TogglePauseBattery,
    Fps15,
    Fps30,
    Fps60,
    FpsNative,
    ToggleStartup,
    OpenConfigFolder,
    Exit,
};

using TrayCallback = void (*)(TrayAction action);

class TrayIcon {
public:
    ~TrayIcon();

    bool Create(HWND window, TrayCallback callback);
    void UpdateState(bool paused, bool pause_fullscreen, bool pause_maximized,
                     bool pause_battery, int fps_cap, bool startup);
    void SetTooltip(const wchar_t* text);
    void HandleMessage(LPARAM event);
    void Destroy();

private:
    void ShowMenu();
    void RebuildMenu();

    NOTIFYICONDATAW m_icon = {};
    TrayCallback m_callback = nullptr;
    HMENU m_menu = nullptr;
    bool m_paused = false;
    bool m_pause_fullscreen = true;
    bool m_pause_maximized = true;
    bool m_pause_battery = false;
    int m_fps_cap = 30;
    bool m_startup = false;
};

} // namespace miniwp
