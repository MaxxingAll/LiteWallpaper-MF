# LiteWallpaper-MF

LiteWallpaper-MF is a small, native video-wallpaper player for Windows 10 and
Windows 11. It uses Windows Media Foundation for decoding and Direct3D 11 for
presentation, so releases contain one executable and no bundled FFmpeg or codec
DLLs.

This project is a GPL-3.0 fork of `f4rdani/LiteWallpaper`.

## Features

- Media Foundation hardware decoding into D3D11 NV12 surfaces
- Direct3D 11 video-processor presentation without a CPU frame copy
- WorkerW desktop integration
- Native system-tray controls; no web runtime or UI framework
- Automatic pause for fullscreen apps, maximized apps, lock, and optionally battery
- 15, 30, 60, or native FPS cap
- Plain HKCU `Run` autostart entry
- No network access

## Usage

1. Run `LiteWallpaper-MF.exe`.
2. Open its tray icon and choose **Choose video...**.
3. Use the same menu to pause, select an FPS cap, configure pause behavior, or exit.

Settings are stored in `%APPDATA%\LiteWallpaper-MF\config.ini`.

Supported formats are the formats installed Windows can decode. H.264 MP4 is
built into standard Windows editions. VP9, AV1, HEVC, WebM, or MKV playback can
require the matching Windows codec extension.

Wallpapers are muted. To create a smaller 1080p H.264 copy outside the app:

```text
ffmpeg -i input.mp4 -vf scale=-2:1080 -an -c:v libx264 -crf 20 -preset medium output.mp4
```

## Build

Requirements:

- Windows 10 or 11
- Visual Studio 2022 with Desktop development with C++
- CMake 3.20 or newer

```powershell
cmake -B build -S . -A x64
cmake --build build --config Release
```

The result is `build/Release/LiteWallpaper-MF.exe`. The release build uses the
static C runtime and imports only Windows system libraries.

## Uninstall

Exit the tray application and delete the executable. If autostart was enabled,
turn it off from the tray first or remove the `LiteWallpaper-MF` value under
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`. The optional configuration
directory can then be deleted.

## License

GPL-3.0. See `LICENSE`.
