# MMD-VR-Renderer (English summary)

Windows extension that adds **VR180 / VR360** offline rendering (Mono / Stereo) to an existing **MikuMikuDance + MikuMikuEffect (+ ray-mmd)** setup.

It does **not** modify `MikuMikuDance.exe` and does **not** switch to MMDX12. Integration is a `d3d9.dll` proxy that chains to MME’s renamed `d3d9_mme.dll`.

The canonical project README is Japanese: [../../README.md](../../README.md).

## Features

- VR180 / VR360 × Mono / Stereo (SBS)
- Cubemap multi-view capture + equirectangular stitch
- IPD / SBS stereo
- PNG sequence + optional FFmpeg MP4
- Optional audio mux (auto-detect or browse WAV)
- YouTube Spherical Video UUID metadata injection
- Progress, cancel, pause, resume via `job.json`
- Default **60 FPS**, quality presets

## Requirements

- MMD 9.32 x64
- MME 0.37 as `d3d9_mme.dll`
- Windows 10/11 x64
- Optional FFmpeg for MP4

## Quick start (from source)

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\fetch_deps.ps1
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File .\tools\install_to_mmd.ps1
```

In MMD: **VR Renderer** menu.

## Docs

See [README.md](README.md) in this folder for the full English doc index.
