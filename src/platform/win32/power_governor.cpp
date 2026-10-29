#include "power_governor.h"

#include <cstdlib>
#include <dwmapi.h>
#include <shellapi.h>

namespace miniwp {
namespace {

bool IsAppWindow(HWND window) {
    if (!window || !IsWindowVisible(window) || IsIconic(window)) return false;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked))) && cloaked) {
        return false;
    }

    wchar_t class_name[64] = {};
    GetClassNameW(window, class_name, static_cast<int>(_countof(class_name)));
    return wcscmp(class_name, L"Progman") != 0 &&
           wcscmp(class_name, L"WorkerW") != 0 &&
           wcscmp(class_name, L"Shell_TrayWnd") != 0 &&
           wcscmp(class_name, L"LiteWallpaperMFWindow") != 0;
}

} // namespace

PowerGovernor::~PowerGovernor() {
    Shutdown();
}

bool PowerGovernor::Init(HWND message_window) {
    m_hwnd = message_window;
    if (m_hwnd) WTSRegisterSessionNotification(m_hwnd, NOTIFY_FOR_THIS_SESSION);
    m_on_battery = IsOnBattery();
    return true;
}

PowerState PowerGovernor::GetCurrentState(bool pause_fullscreen,
                                          bool pause_maximized,
                                          bool pause_on_battery) {
    if (m_locked || m_suspended) return PowerState::Sleeping;

    const ULONGLONG now = GetTickCount64();
    if (now - m_last_check < 250) return m_cached_state;
    m_last_check = now;
    m_on_battery = IsOnBattery();

    if (pause_fullscreen && IsFullscreenAppRunning()) {
        m_cached_state = PowerState::Paused;
    } else if (pause_maximized && IsMaximizedAppRunning()) {
        m_cached_state = PowerState::Occluded;
    } else if (m_on_battery && pause_on_battery) {
        m_cached_state = PowerState::Paused;
    } else if (m_on_battery) {
        m_cached_state = PowerState::Reduced;
    } else {
        m_cached_state = PowerState::Active;
    }
    return m_cached_state;
}

void PowerGovernor::HandleSessionChange(WPARAM event) {
    if (event == WTS_SESSION_LOCK) m_locked = true;
    if (event == WTS_SESSION_UNLOCK) {
        m_locked = false;
        m_last_check = 0;
    }
}

void PowerGovernor::HandlePowerChange(WPARAM event) {
    if (event == PBT_APMSUSPEND) m_suspended = true;
    if (event == PBT_APMRESUMEAUTOMATIC) m_suspended = false;
    if (event == PBT_APMPOWERSTATUSCHANGE || event == PBT_APMRESUMEAUTOMATIC) {
        m_on_battery = IsOnBattery();
        m_last_check = 0;
    }
}

bool PowerGovernor::IsFullscreenAppRunning() const {
    QUERY_USER_NOTIFICATION_STATE notification_state;
    if (SUCCEEDED(SHQueryUserNotificationState(&notification_state)) &&
        notification_state == QUNS_RUNNING_D3D_FULL_SCREEN) {
        return true;
    }

    HWND window = GetForegroundWindow();
    if (!IsAppWindow(window)) return false;
    MONITORINFO monitor = {};
    monitor.cbSize = sizeof(monitor);
    RECT rect = {};
    return GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) &&
           GetWindowRect(window, &rect) &&
           rect.left <= monitor.rcMonitor.left && rect.top <= monitor.rcMonitor.top &&
           rect.right >= monitor.rcMonitor.right && rect.bottom >= monitor.rcMonitor.bottom;
}

bool PowerGovernor::IsMaximizedAppRunning() const {
    HWND window = GetForegroundWindow();
    if (IsAppWindow(window) && IsZoomed(window)) return true;

    bool found = false;
    EnumWindows([](HWND candidate, LPARAM parameter) -> BOOL {
        wchar_t class_name[64] = {};
        GetClassNameW(candidate, class_name,
                      static_cast<int>(_countof(class_name)));
        if (wcscmp(class_name, L"Progman") == 0 ||
            wcscmp(class_name, L"WorkerW") == 0) {
            return TRUE;
        }
        if (IsAppWindow(candidate) && IsZoomed(candidate)) {
            *reinterpret_cast<bool*>(parameter) = true;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

bool PowerGovernor::IsOnBattery() {
    SYSTEM_POWER_STATUS status = {};
    return GetSystemPowerStatus(&status) && status.ACLineStatus == 0;
}

void PowerGovernor::Shutdown() {
    if (m_hwnd) WTSUnRegisterSessionNotification(m_hwnd);
    m_hwnd = nullptr;
}

} // namespace miniwp
