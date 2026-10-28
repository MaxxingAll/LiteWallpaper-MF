#pragma once

#include <windows.h>
#include <wtsapi32.h>

namespace miniwp {

enum class PowerState {
    Active,
    Reduced,
    Paused,
    Occluded,
    Sleeping,
};

class PowerGovernor {
public:
    ~PowerGovernor();

    bool Init(HWND message_window);
    PowerState GetCurrentState(bool pause_fullscreen,
                               bool pause_maximized,
                               bool pause_on_battery);
    void HandleSessionChange(WPARAM event);
    void HandlePowerChange(WPARAM event);
    void Shutdown();

private:
    bool IsFullscreenAppRunning() const;
    bool IsMaximizedAppRunning() const;
    static bool IsOnBattery();

    HWND m_hwnd = nullptr;
    bool m_locked = false;
    bool m_suspended = false;
    bool m_on_battery = false;
    ULONGLONG m_last_check = 0;
    PowerState m_cached_state = PowerState::Active;
};

} // namespace miniwp
