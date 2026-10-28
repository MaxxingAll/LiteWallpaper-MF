# FIXTODO

Review of `main` @ `98bbe4b` (2026-10-28). Work top to bottom: item 1 unblocks CI,
items 2–4 are user-visible bugs, the rest is cleanup. Each item lists **where**,
**change** and **done when**. Line numbers refer to `98bbe4b`.

Delete this file once every item is done.

---

## 1. Build is broken (CI run 40650826501 failed)

**Where**

- `src/platform/win32/tray_icon.h:3`: `NOTIFYICONDATAW` is undeclared.
  `WIN32_LEAN_AND_MEAN` (set in `CMakeLists.txt:33`) excludes `shellapi.h`.
- `src/platform/win32/power_governor.cpp:19,106`: `_countof` is not found.

**Change**

```cpp
// tray_icon.h
#include <windows.h>
#include <shellapi.h>   // NOTIFYICONDATAW

// power_governor.cpp, top
#include <cstdlib>      // _countof
```

`main.cpp` only compiled because it includes `shellapi.h` before `tray_icon.h`.
Don't rely on include order: every header includes what it uses.

**Done when** the `Build` workflow on `main` is green, including the 1.5 MB
size-budget step. Note the exe size in item 9.

---

## 2. Non-ASCII video paths are lost on restart

**Where:** `src/core/config.cpp:98-113` (`Config::Save`).

**Why:** `WritePrivateProfileStringW` writes **ANSI** when the file is missing
or has no UTF-16 BOM. A path like `D:\壁紙\テスト.mp4` is saved as `?`
and the wallpaper disappears on the next launch. Upstream stored UTF-8 JSON,
so this is a regression.

**Change:** add a helper to the anonymous namespace and call it at the top of
`Save()`, after `EnsureDirectory`:

```cpp
// Make sure config.ini is UTF-16LE with a BOM so the *PrivateProfile* APIs
// write Unicode. An existing ANSI file is replaced; Save() rewrites every key.
bool EnsureUnicodeIni(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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
```

`Save()`: `if (!EnsureUnicodeIni(path)) return false;` before the first write.
`Load()` stays as it is (`GetPrivateProfileStringW` reads both encodings).

**Done when**

- choosing `テスト.mp4` in a folder with non-ASCII characters still plays after Exit → relaunch
- `config.ini` starts with bytes `FF FE`
- an old ANSI `config.ini` is converted on the first save, and the settings stay the same

---

## 3. The app quits when Explorer restarts

**Where:** `src/main.cpp:324-358` (`WindowProc`), `389-437` (window creation and init),
`447-456` (5 s attach timer).

**Why:** the render window `g_window` is a child of Explorer's WorkerW, and it
also owns the tray icon and the power/session notifications. When Explorer dies,
the system destroys the window, `WM_DESTROY` calls `PostQuitMessage`, and the
app exits. Upstream recreated the window instead. A child window also misses
top-level broadcasts (`WM_POWERBROADCAST`, `WM_DISPLAYCHANGE`), and
`SetForegroundWindow` on a child doesn't let the tray menu close properly.

**Change: split into two windows**

1. **Control window**, new, top-level, never reparented.
   - Class `LiteWallpaperMFControl`, `WS_POPUP`, ex-style `WS_EX_TOOLWINDOW`, no size, never shown.
     Don't use `HWND_MESSAGE`: message-only windows don't receive broadcasts.
   - `ControlProc` handles everything `WindowProc` handles now: `WM_APP_TRAYICON`,
     `WM_APP_ATTACH`, `WM_WTSSESSION_CHANGE`, `WM_POWERBROADCAST`, `WM_DISPLAYCHANGE`,
     `WM_CLOSE` → `DestroyWindow`, `WM_DESTROY` → `g_running = false; PostQuitMessage(0)`.
   - Point these at it: `g_tray.Create(g_control, …)`, `g_governor.Init(g_control)`,
     `g_injector.RegisterExplorerRestart(g_control)`, and the tray's `Exit` →
     `PostMessageW(g_control, WM_CLOSE, 0, 0)`.
2. **Render window** (`g_window`) is used only for rendering.
   - `WindowProc` keeps only `WM_DESTROY`:
     ```cpp
     case WM_DESTROY:
         if (window == g_window) g_window = nullptr;  // destroyed by the shell
         return 0;
     ```
     No `PostQuitMessage` here.
   - Move window creation out of `wWinMain` into `bool CreateRenderWindow()`:
     create, `g_injector.Attach`, `ResizeToDesktop`, `SW_HIDE`, `g_presenter.Init`.
3. **Recreate** in the main loop, on the existing 5 s timer:
   ```cpp
   if (!g_window) {
       g_presenter.ReleaseFrameResources();
       g_frame = {};
       g_decoder.Close();            // holds textures from the old device
       g_decoder_open = false;
       g_presenter.Cleanup();        // swap chain was bound to the dead HWND
       g_injector.Detach();
       g_has_presented_frame = false;
       if (CreateRenderWindow() && !g_suspended) OpenConfiguredVideo(g_playback_seconds);
   } else if (!g_injector.IsAttachedValid() && g_injector.Reattach(g_window)) { … as now … }
   ```
   If Explorer isn't back yet, `CreateRenderWindow` fails and the next tick (5 s) retries.
   Every render path must check `g_window` first.
4. `DesktopInjector::Detach` (`desktop_injector.cpp:133`): wrap the `ShowWindow` /
   `SetWindowLongPtrW` / `SetParent` calls in `if (IsWindow(m_render_hwnd))`.

**Done when**

- `taskkill /f /im explorer.exe`, then `start explorer`, while a video plays: the app keeps running,
  the tray icon comes back, and the video plays again within 5 s
- tray **Exit** still closes the app cleanly, and the desktop repaints
- changing resolution or the monitor layout resizes the wallpaper (`WM_DISPLAYCHANGE` now arrives)
- clicking outside the open tray menu closes it

---

## 4. No recovery from a lost GPU device

**Where:** `src/main.cpp:216-220` (`RenderFrame` / `Present`), `200-206` (decode error).

**Why:** after a driver update, a TDR or a GPU switch, `Present` returns
`DXGI_ERROR_DEVICE_REMOVED` / `DXGI_ERROR_DEVICE_RESET` forever, and the
decoder shows an error box. The wallpaper stays frozen until the app restarts.

**Change**

- Add `bool g_device_lost = false;`.
- In `RenderNextFrame`, set it when `Present` returns either error, or when the decode fails and
  `g_presenter.GetDevice()->GetDeviceRemovedReason() != S_OK`. In that case
  **don't** call `ShowDecoderError`.
- Recover on the same 5 s timer as item 3, reusing its steps without destroying the window:
  close the decoder → `g_presenter.Cleanup()` → `g_presenter.Init(g_window, w, h)` →
  `OpenConfiguredVideo(g_playback_seconds)`. Clear `g_device_lost` only when `Init` succeeds.
  Put the shared part in one `RebuildGraphics()` used by items 3 and 4.

**Done when** disabling and re-enabling the display adapter in Device Manager
(or `dxcap -forcetdr` from Graphics Tools) brings the wallpaper back within 5 s
with no error box.

---

## 5. Repeated error boxes after a decode failure

**Where:** `src/main.cpp:126-143` (`OpenConfiguredVideo`), `177-181` (`LeaveSuspend`).

**Why:** after a failure, every pause/resume (e.g. each time a window is un-maximized)
reopens the same broken file and shows another modal box.

**Change**

- Add `std::string g_failed_video_path;`.
- `OpenConfiguredVideo`: if `path == g_failed_video_path`, return `false` without
  opening. On failure, show the box once and set `g_failed_video_path = path`.
  On success, clear it.
- `TrayAction::ChooseVideo`: clear `g_failed_video_path` before opening, so
  choosing the same file again is a deliberate retry.

**Done when** a file with a missing codec shows exactly one box, and
maximize/restore cycles show none.

---

## 6. Tray menu opens twice on double-click

**Where:** `src/platform/win32/tray_icon.cpp:69-71`.

**Change:** react to `WM_LBUTTONUP` and `WM_RBUTTONUP` only. Drop `WM_LBUTTONDBLCLK`,
and drop `WM_CONTEXTMENU` (it never arrives without `NOTIFYICON_VERSION_4`).

**Done when** a double-click opens the menu once.

---

## 7. The Run key comes back after the user removes it

**Where:** `src/main.cpp:376`.

**Why:** every launch rewrites the Run value when `run_on_startup=1`, so the
README's uninstall step ("remove the Run value") doesn't stick.

**Change:** replace the unconditional `SetRunAtStartup(true)` with:

- if `run_on_startup` is set, read the `LiteWallpaper-MF` Run value (`RegGetValueW`)
- value missing → set `run_on_startup = false` and `g_config.Save()` (respect the removal)
- value present but pointing to another exe path → `SetRunAtStartup(true)` (exe was moved)
- otherwise do nothing

**Done when** deleting the Run value by hand and relaunching leaves it deleted,
and the tray shows **Run at startup** unchecked.

---

## 8. Warnings and build flags

**Where**

- `src/decoder/mf_decoder.cpp` lines 109, 127, 136, 149, 161, 186, 278, 280, 310, 403:
  C4245 (enum → `DWORD`)
- `CMakeLists.txt:37-42`: `/O1 /GL /LTCG` also apply to Debug builds

**Change**

```cpp
// mf_decoder.cpp, anonymous namespace
constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kAllStreams  = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kMediaSource = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);
```

Use these at those lines.

```cmake
target_compile_options(LiteWallpaperMF PRIVATE
    /GR- /EHsc /W4 /utf-8
    $<$<CONFIG:Release>:/O1 /Oi /Gy /GL>
)
target_link_options(LiteWallpaperMF PRIVATE
    $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO>
)
```

**Done when** the Release build has zero warnings at `/W4`. Optionally add `/WX`
afterwards so CI keeps it that way.

---

## 9. Docs left over from upstream

**Where:** `docs/BENCHMARK.md`.

**Change**

- line 10: delete ", crossfade disabled"
- line 13: "close the settings panel and wait…" → "wait until at least five loops complete"
- line 32: "select the reference clip, and close the settings panel" → "select the reference clip from the tray"
- lines 43-44: delete the `--memprobe` paragraph (that mode no longer exists)
- fill in the Environment and Baseline tables (Roadmap Phase 0) once item 1 builds
- leftover names: `desktop_injector.cpp:193,199` class `MiniWallpaper_MsgReceiver` → `LiteWallpaperMF_MsgReceiver`.
  Optional: rename the `miniwp` namespace in one separate commit.

**Done when** `git grep -n "memprobe\|settings panel\|crossfade\|MiniWallpaper_"` returns nothing outside ROADMAP.md and this file.

---

## 10. Manual verification (Roadmap Phase 1 "done when")

After items 1–4, on real hardware, record each result in `docs/BENCHMARK.md`:

- [ ] 1080p30 VP9 WebM and H.264 MP4 loop with no visible hitch at the loop point
- [ ] Task Manager → GPU → **Video Decode** > 0 % for `LiteWallpaper-MF.exe`
- [ ] `dumpbin /imports LiteWallpaper-MF.exe` lists only Windows system DLLs (no `VCRUNTIME*`, `MSVCP*`, `d3dcompiler_47`)
- [ ] exe ≤ 1.5 MB
- [ ] pause/resume on fullscreen, maximized, battery, lock, and sleep
- [ ] the Explorer-restart and monitor-change tests from item 3

---

## Decide (owner's call, not code)

- **First fork tag.** Upstream's `v1.0.0`–`v2.0.0` already exist and CMake says `0.1.0`.
  Pushing any `v*` tag publishes a public release, so choose the scheme first
  (e.g. `mf-v0.1.0`, or `v3.0.0`, and change `tags: ["v*"]` in `build-and-release.yml` to match).
- **Multi-monitor.** Right now one video is spread across the whole virtual desktop (each monitor
  shows a slice). The original roadmap said "same video on each monitor". Keep span, or
  move per-monitor to Phase 4?
