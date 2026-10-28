#include "tray_icon.h"

#include <shellapi.h>

namespace miniwp {
namespace {

enum MenuId : UINT {
    MenuChoose = 1,
    MenuPause,
    MenuPauseFullscreen,
    MenuPauseMaximized,
    MenuPauseBattery,
    MenuFps15,
    MenuFps30,
    MenuFps60,
    MenuFpsNative,
    MenuStartup,
    MenuConfigFolder,
    MenuExit,
};

UINT Checked(bool value) {
    return MF_STRING | (value ? MF_CHECKED : MF_UNCHECKED);
}

} // namespace

TrayIcon::~TrayIcon() {
    Destroy();
}

bool TrayIcon::Create(HWND window, TrayCallback callback) {
    m_callback = callback;
    m_icon.cbSize = sizeof(m_icon);
    m_icon.hWnd = window;
    m_icon.uID = 1;
    m_icon.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    m_icon.uCallbackMessage = WM_APP_TRAYICON;
    m_icon.hIcon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR | LR_SHARED));
    if (!m_icon.hIcon) m_icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(m_icon.szTip, L"LiteWallpaper-MF");
    RebuildMenu();
    return Shell_NotifyIconW(NIM_ADD, &m_icon) != FALSE;
}

void TrayIcon::UpdateState(bool paused, bool pause_fullscreen,
                           bool pause_maximized, bool pause_battery,
                           int fps_cap, bool startup) {
    m_paused = paused;
    m_pause_fullscreen = pause_fullscreen;
    m_pause_maximized = pause_maximized;
    m_pause_battery = pause_battery;
    m_fps_cap = fps_cap;
    m_startup = startup;
}

void TrayIcon::SetTooltip(const wchar_t* text) {
    if (!text) return;
    wcsncpy_s(m_icon.szTip, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &m_icon);
}

void TrayIcon::HandleMessage(LPARAM event) {
    const UINT message = LOWORD(event);
    if (message == WM_CONTEXTMENU || message == WM_RBUTTONUP ||
        message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK) {
        ShowMenu();
    }
}

void TrayIcon::RebuildMenu() {
    if (m_menu) DestroyMenu(m_menu);
    m_menu = CreatePopupMenu();
    AppendMenuW(m_menu, MF_STRING, MenuChoose, L"Choose video...");
    AppendMenuW(m_menu, MF_STRING, MenuPause, m_paused ? L"Resume" : L"Pause");
    AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);

    HMENU pause_menu = CreatePopupMenu();
    AppendMenuW(pause_menu, Checked(m_pause_fullscreen), MenuPauseFullscreen, L"Fullscreen apps");
    AppendMenuW(pause_menu, Checked(m_pause_maximized), MenuPauseMaximized, L"Maximized apps");
    AppendMenuW(pause_menu, Checked(m_pause_battery), MenuPauseBattery, L"On battery");
    AppendMenuW(m_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(pause_menu), L"Pause when");

    HMENU fps_menu = CreatePopupMenu();
    AppendMenuW(fps_menu, Checked(m_fps_cap == 15), MenuFps15, L"15 FPS");
    AppendMenuW(fps_menu, Checked(m_fps_cap == 30), MenuFps30, L"30 FPS");
    AppendMenuW(fps_menu, Checked(m_fps_cap == 60), MenuFps60, L"60 FPS");
    AppendMenuW(fps_menu, Checked(m_fps_cap == 0), MenuFpsNative, L"Native");
    AppendMenuW(m_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fps_menu), L"FPS cap");

    AppendMenuW(m_menu, Checked(m_startup), MenuStartup, L"Run at startup");
    AppendMenuW(m_menu, MF_STRING, MenuConfigFolder, L"Open config folder");
    AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m_menu, MF_STRING, MenuExit, L"Exit");
    SetMenuDefaultItem(m_menu, MenuChoose, FALSE);
}

void TrayIcon::ShowMenu() {
    RebuildMenu();
    POINT point = {};
    GetCursorPos(&point);
    SetForegroundWindow(m_icon.hWnd);
    const UINT command = TrackPopupMenu(
        m_menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        point.x, point.y, 0, m_icon.hWnd, nullptr);
    PostMessageW(m_icon.hWnd, WM_NULL, 0, 0);
    if (!m_callback || command == 0) return;

    switch (command) {
        case MenuChoose: m_callback(TrayAction::ChooseVideo); break;
        case MenuPause: m_callback(TrayAction::PauseResume); break;
        case MenuPauseFullscreen: m_callback(TrayAction::TogglePauseFullscreen); break;
        case MenuPauseMaximized: m_callback(TrayAction::TogglePauseMaximized); break;
        case MenuPauseBattery: m_callback(TrayAction::TogglePauseBattery); break;
        case MenuFps15: m_callback(TrayAction::Fps15); break;
        case MenuFps30: m_callback(TrayAction::Fps30); break;
        case MenuFps60: m_callback(TrayAction::Fps60); break;
        case MenuFpsNative: m_callback(TrayAction::FpsNative); break;
        case MenuStartup: m_callback(TrayAction::ToggleStartup); break;
        case MenuConfigFolder: m_callback(TrayAction::OpenConfigFolder); break;
        case MenuExit: m_callback(TrayAction::Exit); break;
    }
}

void TrayIcon::Destroy() {
    if (m_icon.hWnd) Shell_NotifyIconW(NIM_DELETE, &m_icon);
    m_icon.hWnd = nullptr;
    if (m_menu) DestroyMenu(m_menu);
    m_menu = nullptr;
}

} // namespace miniwp
