# 🛠️ MiniWallpaper — Developer & Architecture Guide

Welcome to the **MiniWallpaper** codebase! This guide is written for developers and contributors who want to understand the internal architecture, set up the development environment, build from source, and extend the engine.

---

## 📑 Table of Contents
1. [Core Philosophy & Constraints](#-core-philosophy--constraints)
2. [High-Level Architecture](#-high-level-architecture)
3. [Component Breakdown](#-component-breakdown)
4. [Prerequisites & Environment Setup](#-prerequisites--environment-setup)
5. [Building from Source](#-building-from-source)
6. [Packaging Portable Distribution](#-packaging-portable-distribution)
7. [IPC & Engine Command Protocol](#-ipc--engine-command-protocol)
8. [Critical Engineering Rules](#-critical-engineering-rules)
9. [Hardware Acceleration & Dual-GPU Pipeline](#-hardware-acceleration--dual-gpu-pipeline)
10. [Memory Verification (`--memprobe`)](#-memory-verification---memprobe)

---

## 🎯 Core Philosophy & Constraints

MiniWallpaper is engineered with extreme performance constraints that differentiate it from existing wallpaper engines:

| Constraint | Target / Rule | Rationale |
|:---|:---|:---|
| **CPU Usage** | **< 1.0%** (0.0% when idle/locked) | Adaptive QPC frame pacing; hardware-accelerated video decoding |
| **RAM Usage** | **≤ 10 MB background / ≤ 15 MB panel-open** (verified on the live daemon in deep sleep: 4.6 MB background, 5.6 MB panel-open; playing: 6–10 MB with the adaptive working-set trim) | Native C++20 + mimalloc; zero web runtime overhead (no Electron/Chromium/WPF) |
| **GPU Power Saving**| **Deep Sleep for dGPU (0% NVIDIA load)** | Automatic smart routing to Integrated GPU (Intel UHD / AMD APU) |
| **Compatibility** | **Windows 7 SP1+ → Windows 11** | Direct3D 11 with FLIP_DISCARD on Win10/11 and DISCARD fallback on Win7 |
| **Process Model** | **Unified Single-Executable Architecture** | Shared D3D11 device between renderer and panel; closing or minimizing the panel (X, title-bar minimize button, Win+Down / taskbar minimize) releases the full UI stack (device refs, ImGui, swapchain, thumbnails); deep sleep releases the video decoder itself |

---

## 🏗️ High-Level Architecture

MiniWallpaper is built as a unified native binary (`MiniWallpaper.exe`) combining an ultra-low-overhead resident render loop and an on-demand Dear ImGui control panel:

```
┌─────────────────────────────────────────────────────────────┐
│                    MiniWallpaper.exe                        │
├─────────────────────────────────────────────────────────────┤
│  1. Background Desktop Renderer Loop                        │
│     - Memory: ~1 MB deep-sleep, ~6-10 MB playing | CPU<1%   │
│     - Injects into desktop canvas (WorkerW 0x052C)          │
│     - D3D11VA Hardware Accelerated Video Decoding           │
│     - Low-overhead WASAPI Shared Audio Output               │
│     - Power Governor (Auto-pauses on game / lock / battery) │
│     - System Tray Icon & Local Pipe IPC Server              │
│                                                             │
│  2. Integrated Control Panel (On-Demand)                    │
│     - Dear ImGui (Dark Modern UI) with FontAwesome icons    │
│     - Real-Time Hardware & Telemetry Monitor                │
│     - Fluid Responsive Video Gallery & Multi-Monitor Target │
│     - Built-in 1080p GPU Video Optimizer & Downscaler       │
│     - Instantaneous (0ms) Tray Hide / Restore Lifecycle     │
└─────────────────────────────────────────────────────────────┘
```

---

## 📂 Component Breakdown

```
src/
├── main.cpp                         # Daemon main loop, window class, & message dispatcher
├── app.rc                           # Windows resource script (Embedded multi-res icon)
│
├── core/                            # Platform-agnostic core logic
│   ├── config.h / .cpp              # JSON configuration load/save (nlohmann_json)
│   ├── engine_state.h / .cpp        # Thread-safe atomic shared engine state
│   ├── playback_clock.h / .cpp      # QPC-driven adaptive frame pacing clock
│   ├── ipc_server.h / .cpp          # Windows Named Pipe server & async client
│   ├── ring_buffer.h                # Lock-free SPSC packet ring buffer
│   └── video_optimizer.h / .cpp     # Multi-threaded GPU video pre-scaler / downscaler
│
├── decoder/                         # Media playback & hardware acceleration
│   ├── video_decoder.h              # Abstract video decoder interface
│   ├── ffmpeg_hw_decoder.h / .cpp   # FFmpeg D3D11VA hardware video decoder
│   └── audio_player.h / .cpp        # Low-latency WASAPI shared audio streamer
│
├── platform/win32/                  # Windows OS integration layer
│   ├── desktop_injector.h / .cpp    # WorkerW desktop canvas injector (0x052C)
│   ├── d3d11_presenter.h / .cpp     # Direct3D 11 device, swapchain & NV12 shaders
│   ├── power_governor.h / .cpp      # Auto-pause heuristics (fullscreen/lock/battery)
│   ├── lockscreen_manager.h / .cpp  # Lock screen snapshot capture & staging
│   ├── hardware_info.h / .cpp       # Multi-GPU / multi-monitor hardware detector
│   └── tray_icon.h / .cpp           # Shell_NotifyIconW & tray popup menu
│
├── ui/                              # User Interface
│   ├── settings_app.h / .cpp        # Responsive Dear ImGui settings application
│   └── IconsFontAwesome6.h          # FontAwesome 6 icon glyph definitions
│
└── shaders/                         # Embedded HLSL Shader files
    ├── fullscreen_quad.hlsl         # 3-vertex fullscreen triangle vertex shader
    └── nv12_to_rgb.hlsl             # BT.709 NV12 (Y + UV) to RGB pixel shader
```

---

## 🧰 Prerequisites & Environment Setup

### Required Tools
1. **Operating System**: Windows 10/11 (64-bit) for development (binaries run on Windows 7 SP1+).
2. **Compiler**: Visual Studio 2022 (MSVC v143 toolset) with *Desktop development with C++*.
3. **CMake**: Version 3.20 or newer.
4. **vcpkg**: Microsoft C++ package manager.
5. **FFmpeg Developer Shared Libraries (64-bit)**:
   - Header files in `third_party/ffmpeg/include`
   - Import libraries in `third_party/ffmpeg/lib`
   - Runtime DLLs in `third_party/ffmpeg/bin`

---

## 🔨 Building from Source

### Step 1: Install Dependencies via vcpkg
In the project root directory (manifest mode will read `vcpkg.json` automatically):

```powershell
# Using vcpkg instance:
vcpkg install nlohmann-json:x64-windows mimalloc:x64-windows imgui[docking-experimental,win32-binding,dx11-binding]:x64-windows
```

### Step 2: Set Up FFmpeg Developer Files
Ensure FFmpeg 64-bit shared developer files are extracted in `third_party/ffmpeg`:
```
third_party/ffmpeg/
├── include/
│   ├── libavcodec/
│   ├── libavformat/
│   ├── libavutil/
│   ├── libswresample/
│   └── libswscale/
├── lib/
│   ├── avcodec.lib
│   ├── avformat.lib
│   ├── avutil.lib
│   ├── swresample.lib
│   └── swscale.lib
└── bin/
    ├── avcodec-*.dll
    ├── avformat-*.dll
    ├── avutil-*.dll
    ├── swresample-*.dll
    └── swscale-*.dll
```

### Step 3: Configure and Build
```powershell
# Configure CMake with vcpkg toolchain
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows

# Build in Release configuration
cmake --build build --config Release
```

Output executable will be located at:
- `build/Release/MiniWallpaper.exe`

---

## 📦 Packaging Portable Distribution

To bundle the executable, runtime DLLs, assets, and shaders for release:

```powershell
$dist = "dist/MiniWallpaper-Portable"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
New-Item -ItemType Directory -Force -Path "$dist/assets" | Out-Null

Copy-Item "build/Release/MiniWallpaper.exe" -Destination $dist
Copy-Item "third_party/ffmpeg/bin/*.dll" -Destination $dist
Copy-Item "build/vcpkg_installed/x64-windows/bin/*.dll" -Destination $dist
Copy-Item "assets/*" -Destination "$dist/assets"
Copy-Item "LICENSE" -Destination $dist
Copy-Item "README.md" -Destination $dist

# Compress to Zip
Compress-Archive -Path "$dist/*" -DestinationPath "dist/MiniWallpaper-Portable-win64.zip" -Force
```

---

## 🔌 IPC & Engine Command Protocol

The engine provides a local Windows Named Pipe interface for single-instance activation and IPC control:
- **Pipe Address**: `\\.\pipe\MiniWallpaper`
- **Format**: JSON-RPC over message pipe (`PIPE_TYPE_MESSAGE`)

### Supported Commands

| Command | Payload | Description |
| :--- | :--- | :--- |
| `open_settings` | `{ "cmd": "open_settings" }` | Restores and brings Control Panel window to front |
| `set_wallpaper` | `{ "cmd": "set_wallpaper", "path": "...", "action": "wallpaper" }` | Applies video as desktop wallpaper |
| `set_render_device` | `{ "cmd": "set_render_device", "gpu_index": 1 }` | Switches rendering between GPU 1, GPU 2, or CPU (-1) |
| `set_target_displays`| `{ "cmd": "set_target_displays", "displays": [0, 1] }` | Directs wallpaper rendering to specific monitors |
| `set_scaling` | `{ "cmd": "set_scaling", "mode": 0 }` | Sets scaling mode (0=Cover, 1=Fit, 2=Stretch) |
| `set_fps` | `{ "cmd": "set_fps", "fps": 30 }` | Sets target render frame rate |
| `set_volume` | `{ "cmd": "set_volume", "volume": 0.5 }` | Adjusts master playback volume |
| `pause` / `resume` / `stop` | `{ "cmd": "pause" }` | Controls playback state and restores native desktop on stop |

---

## ⚡ Hardware Acceleration & Dual-GPU Pipeline

MiniWallpaper employs a specialized DirectX 11 rendering pipeline designed for minimal power and memory consumption:

1. **Integrated GPU (iGPU) Preference**:
   * When multiple GPUs are detected (e.g. Intel UHD Graphics + NVIDIA RTX), the default hardware device routes to the iGPU (`gpu_index = 1`).
   * This allows discrete gaming GPUs to remain in **0% power-saving sleep state** while the desktop wallpaper animates smoothly.
2. **Zero-Copy NV12 D3D11VA Decoding**:
   * Decoded video frames remain as hardware texture surfaces in GPU memory.
   * Pixel shaders perform color space conversion (BT.709 YUV $\rightarrow$ RGB) on the GPU without intermediate CPU memory copying.
3. **Double-Buffered Swapchain**:
   * Uses `DXGI_SWAP_EFFECT_FLIP_DISCARD` on Windows 10/11 and `DXGI_SWAP_EFFECT_DISCARD` on Windows 7/8.

---

## ⚠️ Critical Engineering Rules

When contributing code to MiniWallpaper, **always adhere to these principles**:

1. **Preserve Ultra-Low RAM Footprint**: Never introduce heavy libraries, web runtimes, or unmanaged thread pools into hot render loops.
2. **Thread Safety**: All state shared between the daemon render loop and the UI thread must use `std::atomic` or `g_decoder_mutex` protection.
3. **Release the UI Stack on Close or Minimize**: Closing *or minimizing* the Control Panel (X button, title-bar minimize button, Win+Down / taskbar minimize — all three funnel into `SettingsUI::Close()`) must destroy its window and release the ImGui context, font atlas, swapchain, DXGI device references and thumbnail textures (`SettingsUI::ReleaseUiStack()`) instead of only hiding the window. The panel reuses the daemon's D3D11 device via `D3D11Presenter::GetSharedD3DDevice()`; a second device (and its driver allocation) is only created by the standalone `--screenshot` / `/c`-without-daemon fallback path.
4. **Desktop Cleanup on Stop**: Detaching the wallpaper must trigger desktop refresh (`SystemParametersInfoW(SPI_SETDESKWALLPAPER, ...)` + `InvalidateRect`) to restore native Windows wallpaper cleanly.
5. **Deep Sleep Releases the Decoder**: Every suspend path (governor resource/fullscreen/lock triggers, and manual pause) must funnel through `EnterDeepSuspend()` / `LeaveDeepSuspend()` in `main.cpp`. Never suspend by merely stopping the render loop: the decoder, its D3D11VA frame pool and FFmpeg contexts must be closed, and the playback position saved so resume can reopen and `SeekTo()` back to where the user left off.
6. **Bound the Playing Working Set (Adaptive Trim)**: The D3D11VA decode/present pipeline re-warms ~12 MB of pages in one burst per loop re-prime and then never touches them until the next cycle. The render loop must re-trim (`TrimWorkingSetMemory()`) as soon as the working set crosses the soft cap (16 MB, checked once per second, minimum 8 s between trims; a 30 s fallback covers idle states) so Task Manager's *Memory* column stays in the 6–10 MB band without measurable CPU or page-fault cost.

---

## 🧪 Memory Verification (`--memprobe`)

`MiniWallpaper.exe --memprobe [seconds]` is a hidden developer mode that boots the engine **without side effects** (no WorkerW injection, no wallpaper/lockscreen writes, no `Config::Save`, hidden 1920×1080 window, no tray, but full IPC server, governor, thumbnail worker and audio init). It samples the process once per second and writes `memprobe.txt` next to the executable:

- `privateWS` — Task Manager's *Memory* column (active private working set; computed per page via `QueryWorkingSet`'s `Shared` flag)
- `ws`, `privateCommit`, `peakWS`, `threads`
- per-phase top modules / anonymous memory attribution
- `*-trimmed` samples taken ~1.2 s after `TrimWorkingSetMemory()`, matching what Task Manager shows right after a working-set trim (adaptive on memory creep, 30 s fallback)
- an `OPEN/CLOSE ×5` leak-check phase, then a **deep-suspend phase** (panel closed), a **deep-suspend + panel-open phase** and a **resume phase** (decoder reopened + seek back)

The probe runs 6 phases, which is how the numbers below were verified (release build, NVIDIA RTX 3060 Laptop GPU, 1920×1080 @ 60 fps hardware-decoded wallpaper):

| State (panel / wallpaper) | privateWS steady | Right after a working-set trim |
|:---|:---|:---|
| Background, **deep sleep** (governor suspend or manual pause, decoder fully released) | **1.1 MB** | 0.2 MB |
| Control panel open, **deep sleep** (decoder released) | **10.4–11.5 MB** | 0.2 MB |
| Background or panel open, wallpaper playing | **6–10 MB** with the adaptive working-set trim (steady; the pipeline re-warms ~12 MB for < 1 s per loop re-prime, trimmed on the next 1 s check). Before the adaptive trim: ~19–23 MB | 0.1–0.4 MB |
| Panel closed after use | full UI-stack release, window destroyed (`hwnd = 0`), no retained resident memory | 0.2 MB |

### Adaptive working-set trim (playing state)

While the wallpaper plays, the D3D11VA decode/present pipeline warms up ~12 MB of pages in a short burst once per loop re-prime (measured: ~6,400 faults within one second), then leaves them resident even though they are not touched again until the next cycle — the old fixed 30 s trim let Task Manager show a 19–23 MB plateau most of the time. The daemon therefore re-trims as soon as the working set crosses a 16 MB soft cap (checked once per second, minimum 8 s between trims; the 30 s cadence remains as a fallback for idle states). Measured cost: steady-state < 200 faults/s, one ~6 k-fault re-warm burst per trim (tens of ms), CPU unchanged — while the *Memory* column stays in a 6–10 MB band. The fullscreen-game / high-RAM / high-VRAM safeguards are untouched.

Deep-sleep semantics (the deep-suspend and resume phases verify the whole contract):

- Suspend releases the decoder **completely** (`EnterDeepSuspend()` → `FFmpegHWDecoder::Close()`: FFmpeg contexts, D3D11VA frame pool and codec buffers) instead of only flushing its frame queue. The last presented frame stays on screen, so the desktop still shows the frozen wallpaper.
- The playback position is captured at suspend and restored on resume: the video is reopened and `SeekTo(position)` lands on the nearest keyframe at or before it (probe: suspended at frame `pts=180224`, decoding resumed at `pts=61440` for a 5.3 s target, 361 frames rendered in the following 12 s). Reopening costs ~0.3–0.5 s once per resume — in exchange for the freed driver/decoder pages above.
- `FlushBuffers()` (partial release) remains in the decoder interface but is no longer used by the daemon path.

For reference, comparing against the earlier optimization rounds (same metric, same machine): a build that only *hid* the control panel kept its whole UI stack committed (19.5 MB panel-open, window handle still alive after closing); after the UI-stack release but before the decoder release, a suspended wallpaper still cost 15.5 MB panel-open / 7.7 MB background because the decoder stayed resident.

Cross-check on the **live daemon** (injected wallpaper, same machine, `Working Set - Private` counter; sequence: pause → 10 s → open panel → resume):

| Live daemon state | privateWS |
|:---|:---|
| Wallpaper playing, panel closed | **5.4–10 MB** (median 8.3; adaptive trim — sub-second ~19 MB spikes right after a loop re-prime, caught by the next 1 s check) |
| Wallpaper playing, panel open | ~7–10 MB (panel pages are trimmed the same way; closing the panel returns ~3 MB instantly) |
| **Deep sleep** (manual pause), panel closed | **4.6 MB** |
| **Deep sleep**, control panel open | **5.6 MB** |
| After `resume` (`{"cmd":"resume"}`) | decoder reopened, `frames_rendered` 306 → 665, `playing=true`, `hw_decode=true` |

The probe table above measures ~1.2 s after `TrimWorkingSetMemory()` (the tightest case); in the live daemon the pages that the tray/panel/driver fault back in while suspended settle around 4–6 MB. Both stay well inside the ≤ 10 MB / ≤ 15 MB targets. With the adaptive trim the playing state also stays inside its band: 6–10 MB typical, 5.4 MB floor, at the cost of one ~6 k-fault re-warm burst per loop re-prime (no CPU change).

---

## 📄 License

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.
