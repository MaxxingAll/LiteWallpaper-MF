#pragma once
#include <windows.h>
#include <string>

namespace miniwp {

class SettingsUI {
public:
    // Open the Settings window on-demand (allocates D3D11 swapchain & ImGui)
    static bool Open(HINSTANCE hInstance);

    // Render one frame if Settings window is active
    static void RenderFrame();

    // Check if the Settings window is currently open
    static bool IsOpen();

    // Hide the Settings window to tray
    static void Close();

    // Shutdown and destroy the Settings window & resources on app exit
    static void Shutdown();

    // Get HWND of Settings window
    static HWND GetHwnd();

    // Set active tab/page (0=Gallery, 1=Lockscreen, 2=General, 3=Performance)
    static void SetActivePage(int page);
    static int GetActivePage();

    // Capture pixel-perfect screenshot of the active UI to PNG via WIC
    static bool SaveScreenshot(const std::string& path);
};

} // namespace miniwp
