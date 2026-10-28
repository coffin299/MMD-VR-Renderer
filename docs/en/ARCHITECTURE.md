# Architecture

For a fuller technical summary see [TECHNICAL_OVERVIEW.md](TECHNICAL_OVERVIEW.md).  
Japanese: [../ARCHITECTURE.md](../ARCHITECTURE.md)

## Overview

MMD-VR-Renderer installs into an existing MMD + MME setup as a `d3d9.dll` proxy that chains to `d3d9_mme.dll`. It does not replace MMD’s renderer and does not use MMDX12.

## Components

| Component | Role |
|-----------|------|
| `d3d9` proxy | Loads MME / system D3D9; hooks CreateDevice / Present / SetTransform / … |
| `PluginHost` | Menu UI, job orchestration, redraw requests |
| `CameraController` | Observes VIEW/PROJECTION; overrides during multi-view |
| `D3D9CaptureBackend` | Backbuffer readback before Present |
| `ProjectionEngine` | Cubemap → equirectangular (VR360 / VR180) |
| `StereoEngine` | IPD offsets + SBS compose |
| `RenderController` | Frame loop, resume via `job.json` |
| `ImageSequenceWriter` | PNG sequence |
| `VideoEncoder` | External FFmpeg (+ optional audio) |
| `SphericalMp4Inject` | YouTube Spherical metadata |
| `Diagnostics` | `mmd_vr_renderer.log` |

## Data flow

1. User opens **VR Renderer** → settings.  
2. Per animation frame: seek MMD frame Edit, then capture live view or cubemap faces.  
3. Stitch to equirect (and SBS if stereo).  
4. Write `frame_XXXXXX.png` and update `job.json`.  
5. Optionally encode MP4, mux audio, inject Spherical UUID.

## Constraints

- No official MMD plugin API; hooks are required.  
- `MMDExport.h` has no camera setters; transform-based control must be checked per MMD build.  
- Screen-space effects (SSR/SSAO) may seam across faces; effects are not modified.
