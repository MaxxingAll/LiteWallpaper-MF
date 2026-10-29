#include "config.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cwchar>

namespace miniwp {
namespace {

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

bool ReadBool(const wchar_t* key, bool fallback, const std::wstring& path) {
    return GetPrivateProfileIntW(L"settings", key, fallback ? 1 : 0,
                                 path.c_str()) != 0;
}

bool WriteInt(const wchar_t* key, int value, const std::wstring& path) {
    wchar_t buffer[32] = {};
    swprintf_s(buffer, _countof(buffer), L"%d", value);
    return WritePrivateProfileStringW(L"settings", key, buffer,
                                      path.c_str()) != FALSE;
}

bool WriteBool(const wchar_t* key, bool value, const std::wstring& path) {
    return WritePrivateProfileStringW(L"settings", key, value ? L"1" : L"0",
                                      path.c_str()) != FALSE;
}

bool EnsureDirectory(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// The profile APIs only preserve Unicode when the INI already has a UTF-16LE
// BOM. Save rewrites every setting, so replace a legacy ANSI file first.
bool EnsureUnicodeIni(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        unsigned char bom[2] = {};
        DWORD read = 0;
        const bool unicode = ReadFile(file, bom, 2, &read, nullptr) && read == 2 &&
                             bom[0] == 0xFF && bom[1] == 0xFE;
        CloseHandle(file);
        if (unicode) return true;
        DeleteFileW(path.c_str());
    }

    file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const unsigned char bom[2] = {0xFF, 0xFE};
    DWORD written = 0;
    const bool ok = WriteFile(file, bom, 2, &written, nullptr) && written == 2;
    CloseHandle(file);
    return ok;
}

} // namespace

Config g_config;

std::wstring Config::GetConfigDir() {
    wchar_t app_data[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr,
                                SHGFP_TYPE_CURRENT, app_data))) {
        return L".";
    }
    return std::wstring(app_data) + L"\\LiteWallpaper-MF";
}

std::wstring Config::GetConfigFilePath() {
    return GetConfigDir() + L"\\config.ini";
}

bool Config::Load() {
    const std::wstring dir = GetConfigDir();
    const bool directory_ready = EnsureDirectory(dir);

    const std::wstring path = GetConfigFilePath();
    wchar_t video[32768] = {};
    GetPrivateProfileStringW(L"settings", L"video", L"", video,
                             static_cast<DWORD>(_countof(video)), path.c_str());
    m_config.video_path = WideToUtf8(video);
    m_config.scaling_mode = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(L"settings", L"scaling", 0,
                                               path.c_str())), 0, 2);
    const int fps = static_cast<int>(GetPrivateProfileIntW(
        L"settings", L"fps", 30, path.c_str()));
    m_config.fps_cap = (fps == 0 || fps == 15 || fps == 30 || fps == 60) ? fps : 30;
    m_config.pause_on_fullscreen = ReadBool(L"pause_fullscreen", true, path);
    m_config.pause_on_maximized = ReadBool(L"pause_maximized", true, path);
    m_config.pause_on_battery = ReadBool(L"pause_battery", false, path);
    m_config.run_on_startup = ReadBool(L"run_on_startup", false, path);
    return directory_ready;
}

bool Config::Save() const {
    const std::wstring dir = GetConfigDir();
    if (!EnsureDirectory(dir)) return false;

    const std::wstring path = GetConfigFilePath();
    if (!EnsureUnicodeIni(path)) return false;
    const std::wstring video = Utf8ToWide(m_config.video_path);
    bool ok = WritePrivateProfileStringW(L"settings", L"video", video.c_str(),
                                         path.c_str()) != FALSE;
    if (!WriteInt(L"scaling", m_config.scaling_mode, path)) ok = false;
    if (!WriteInt(L"fps", m_config.fps_cap, path)) ok = false;
    if (!WriteBool(L"pause_fullscreen", m_config.pause_on_fullscreen, path)) {
        ok = false;
    }
    if (!WriteBool(L"pause_maximized", m_config.pause_on_maximized, path)) {
        ok = false;
    }
    if (!WriteBool(L"pause_battery", m_config.pause_on_battery, path)) ok = false;
    if (!WriteBool(L"run_on_startup", m_config.run_on_startup, path)) ok = false;
    return ok;
}

} // namespace miniwp
