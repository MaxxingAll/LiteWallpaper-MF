# Benchmark Method

This document defines the repeatable benchmark used to compare upstream v2.0.0
with the Media Foundation fork. Run both builds on the same Windows machine,
with the same video, display configuration, power plan, and background workload.

## Reference workload

- Video: 1920x1080, 30 fps, VP9 WebM, Opus audio, 8 seconds
- Playback: muted, seamless looping, crossfade disabled
- Display: record resolution, refresh rate, scaling, and selected GPU
- Power: AC power, Windows power mode set to **Balanced**
- Sampling: close the settings panel and wait until at least five loops complete

Record the test clip's filename and SHA-256 with every result. Do not commit a
copyrighted clip to the repository.

## Build configurations

Build upstream v2.0.0 using its documented vcpkg and FFmpeg setup. Build this
fork in Release mode without a package manager:

```powershell
cmake -B build -S . -A x64
cmake --build build --config Release
```

## Measurements

1. Reboot, sign in, and wait for background activity to settle.
2. Start Windows Performance Recorder or Task Manager before launching the app.
3. Launch the build, select the reference clip, and close the settings panel.
4. Confirm **GPU > Video Decode** is active for the app's selected adapter.
5. After five loops, sample for 60 seconds and record median and peak values:
   - CPU percentage
   - private working set and private commit
   - dedicated and shared GPU memory
6. Pause playback. After deep suspend activates, record memory again.
7. Exit, clear standby effects by rebooting, then measure cold launch to first
   presented frame. Repeat three times and report the median.
8. Record the executable size and total uncompressed release size.

Use the existing `--memprobe` mode as supporting evidence for memory behavior,
but use Task Manager or WPR for the headline playing and GPU measurements.

## Environment

| Item | Value |
|---|---|
| Date | Pending |
| Commit | Pending |
| Windows build | Pending |
| CPU | Pending |
| GPU and driver | Pending |
| RAM | Pending |
| Display | Pending |
| Codec extensions | Pending |
| Reference clip SHA-256 | Pending |

## Baseline results

| Metric | Upstream v2.0.0 | Media Foundation | Target |
|---|---:|---:|---:|
| Executable size | Pending | Pending | <= 1.5 MB |
| Release size, uncompressed | Pending | Pending | <= 1.5 MB |
| CPU median / peak | Pending | Pending | <= 0.3% median |
| Private working set median / peak | Pending | Pending | <= 6 MB median |
| Paused private working set | Pending | Pending | < 1 MB |
| Dedicated/shared GPU memory | Pending | Pending | <= 50 MB total |
| Cold launch to first frame | Pending | Pending | Record only |

Raw WPR traces and screenshots should be attached to the corresponding GitHub
issue or release artifact; only the summarized results belong in this file.
