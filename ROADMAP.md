# LiteWallpaper-MF — Roadmap

Fork of [f4rdani/LiteWallpaper](https://github.com/f4rdani/LiteWallpaper) (GPL-3.0).

**Goal:** the lightest possible looping video wallpaper for Windows 10/11.
Drop the bundled FFmpeg (~152 MB) and let Windows decode instead, through
**Media Foundation** (the engine behind the Films & TV app). The codecs come
from the system: VP9 / AV1 / HEVC Video Extensions and Web Media Extensions.

Keep the parts upstream already does well:

- WorkerW desktop injection (`desktop_injector`)
- Zero-copy D3D11 NV12 video-processor presenter (`d3d11_presenter`)
- Power governor (pause on fullscreen, maximized, lock and battery)

## Targets

| Metric | Upstream v2.0.0 | Target |
|---|---|---|
| Release size (unzipped) | ~155 MB, 13 files | **≤ 1.5 MB, single exe** |
| RAM while playing (1080p) | 6–10 MB | ≤ 6 MB |
| RAM paused / deep sleep | < 1 MB | < 1 MB |
| VRAM while playing | 50–61 MB | ≤ 50 MB |
| CPU while playing | ~0.3 % | ≤ 0.3 % |
| Runtime dependencies | FFmpeg, mimalloc, VC++ DLLs, d3dcompiler_47 | none beyond Windows |
| Network access | none | none (keep it that way) |

Formats: anything Windows itself plays. That covers WebM (VP9/AV1), MP4 (H.264/HEVC/AV1) and MKV.
The reference test clip is a 1080p30 VP9 WebM with Opus audio, 8 s.

---

## Phase 0 — Baseline

- [ ] Build upstream locally (VS 2022 + vcpkg) and record a baseline:
      RAM, VRAM, CPU, disk, and cold start-up time with the reference clip.
- [x] Add `docs/BENCHMARK.md` with the method, so later phases measure the same way.

**Done when** the baseline numbers are committed.

## Phase 1 — Media Foundation decoder (the core swap)

The decoder already sits behind `IVideoDecoder` (`src/decoder/video_decoder.h`),
so this is a drop-in second implementation.

- [x] `src/decoder/mf_decoder.{h,cpp}` : `class MFDecoder : public IVideoDecoder`
  - `MFStartup`. Create `IMFDXGIDeviceManager`, then `ResetDevice(d3d_device)`.
    Turn on multithread protection on the D3D11 device.
  - `MFCreateSourceReaderFromURL` with these attributes:
    `MF_SOURCE_READER_D3D_MANAGER`, `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS = TRUE`,
    `MF_SOURCE_READER_DISABLE_DXVA = FALSE`.
    Do **not** turn on advanced video processing: we want NV12, not BGRA.
  - Set the output type for `MF_SOURCE_READER_FIRST_VIDEO_STREAM` to `MFVideoFormat_NV12`.
  - `DecodeNextFrame`: call `ReadSample`, then go IMFSample → `IMFDXGIBuffer`.
    `GetResource(ID3D11Texture2D)` and `GetSubresourceIndex` fill in
    `VideoFrame.texture` / `texture_index`. Keep the sample alive until the next frame.
  - `SeekToStart` / `SeekTo`: `SetCurrentPosition(GUID_NULL, PROPVARIANT{VT_I8})`.
  - `GetInfo`:
    - size from `MF_MT_FRAME_SIZE`
    - fps from `MF_MT_FRAME_RATE`
    - duration from `MF_PD_DURATION`
    - codec from the `MF_MT_SUBTYPE` of the native type
  - On a missing codec (`MF_E_TOPO_CODEC_NOT_FOUND` / `MF_E_INVALIDMEDIATYPE`),
    report a clear error that names the Store extension to install.
- [x] Use MF by default while FFmpeg remained, then retire the selector with Phase 2.
- [x] Check that the presenter handles MF texture arrays.
      The subresource index comes from `IMFDXGIBuffer`.

**Done when** the reference WebM loops seamlessly on the desktop through MF,
with GPU decode confirmed (Task Manager → GPU → Video Decode > 0 %),
and the FFmpeg DLLs are absent.

## Phase 2 — Remove FFmpeg completely

- [x] Delete `ffmpeg_hw_decoder.*`. Remove every `sws_*` / `swr_*` / `av*` use.
- [x] Delete `core/video_optimizer.*`, the 4K→1080p re-encoder. It needs FFmpeg encoders.
      Document a one-line `ffmpeg` command in the README instead.
- [x] Audio: drop it from the default build. Wallpapers are muted.
      Optional later: a second MF source reader → PCM → WASAPI (no swresample).
- [x] Remove thumbnails with the gallery UI. This drops the swscale path.
- [x] CI: remove the FFmpeg download step.

**Done when** `git grep -i "avcodec\|avformat\|swscale\|swresample"` is empty
and the release contains no FFmpeg.

## Phase 3 — Strip the extras

- [x] Remove `lockscreen_manager.*`. It changes the lock screen, turns off Spotlight
      (ContentDeliveryManager keys), writes PersonalizationCSP / HKLM `OEMBackground`,
      and runs `ffmpeg.exe` behind the scenes.
- [x] Remove the `.scr` screensaver companion. It is a 1.5 MB duplicate of the exe.
- [x] Remove the `StartupDelayInMSec = 0` tweak. It changes start-up timing for
      **every** app, not just this one. Autostart = a plain HKCU `Run` value only.
- [x] Drop mimalloc. It is overkill at this size, and the working-set trim needs no
      allocator swap.
- [x] Replace the shader path with the system D3D11 video processor.
      This removes `D3DCompile` and `d3dcompiler_47.dll` (~4 MB RAM).
- [x] Remove the loop crossfade. Seamless-loop clips don't need it,
      and it saves a full-frame snapshot texture and a blend pass.
- [x] Drop `hardware_info` telemetry except what the governor needs.

**Done when** the exe imports only system DLLs and the registry footprint is
config plus an optional `Run` key.

## Phase 4 — Tray-first UI (drop ImGui)

- [x] Replace the ImGui control panel (`ui/settings_app.cpp`, ~4.4k lines) and the
      Font Awesome TTF with a native tray menu:
  - Choose video… (`IFileOpenDialog`)
  - Pause / Resume
  - Pause when: fullscreen ☑ / maximized ☑ / on battery ☐
  - FPS cap: 15 / 30 / 60 / native
  - Run at startup
  - Open config folder
  - Exit
- [x] Settings live in `%APPDATA%\LiteWallpaper-MF\config.ini`.
      Consider replacing nlohmann/json with a tiny INI so the build has no vcpkg dependencies.
- [x] Multi-monitor: span the same video across the WorkerW desktop first.

**Done when** vcpkg is no longer needed and the exe is ≤ 1.5 MB.

## Phase 5 — Packaging and trust

- [x] Static CRT (`/MT`) → a single portable exe, with no VC++ runtime DLLs.
- [x] Release **only from CI** (`softprops/action-gh-release` on tag push).
      Never hand-upload assets. Publish the SHA-256 with each release.
- [x] Add build provenance attestation (`actions/attest-build-provenance`).
- [x] Update the README: what the fork changes, which codecs are needed, and how to uninstall
      (delete the exe; remove the HKCU `Run` value if it was set).

**Done when** a tagged release ships one exe built and attested by CI.

---

## Later / nice to have

- Per-monitor videos, playlist / shuffle on a timer
- Optional audio through WASAPI (Phase 2 note)
- HDR passthrough (scRGB swap chain) for HDR clips

## Risks

| Risk | Mitigation |
|---|---|
| VP9 / AV1 / Web Media extension missing (N editions, stripped installs) | Detect at open and show which Store extension to install. MP4/H.264 always works. |
| Some drivers return BGRA instead of NV12 from the MF transform | Fall back to BGRA and let the D3D11 video processor consume it directly. |
| MF texture-array sample lifetime (frame recycled too early) | Hold the `IMFSample` until the next frame is presented. |
| Windows build-specific WorkerW changes (24H2+) | Already handled upstream; re-test on every Windows feature update. |
