# Technical overview

How MMD-VR-Renderer is put together end-to-end. Motivation and research: [TECHNICAL_RESEARCH.md](TECHNICAL_RESEARCH.md). Short table: [ARCHITECTURE.md](ARCHITECTURE.md).

Japanese original: [../TECHNICAL_OVERVIEW.md](../TECHNICAL_OVERVIEW.md)

## 1. Role

| Item | Content |
|------|---------|
| Goal | Keep MMD+MME (including ray-mmd) look, produce VR180/VR360 equirect offline |
| Non-goals | Patching MMD.exe, moving to MMDX12, editing ray-mmd effect files |
| Install shape | Same-style `d3d9.dll` proxy chain as MME |

```
MikuMikuDance.exe
    └─ d3d9.dll          … this project
           └─ d3d9_mme.dll   … renamed MME
                  └─ system d3d9 (as needed)
```

A same-bytes alias `d3d9_mmdvr.dll` is also installed for uninstall detection.

## 2. Runtime layout

The main artifact is `d3d9_mmdvr.dll` (also copied as `d3d9.dll`).

```
┌─────────────────────────────────────────────────────────┐
│  UI (MenuUI / RenderDialog / ProgressDialog / UiStrings)│
├─────────────────────────────────────────────────────────┤
│  PluginHost — jobs, frame seek, face capture requests   │
├──────────────┬──────────────────┬───────────────────────┤
│ DeviceHooks  │ D3D9Capture      │ MmdTimeline / WaveDetect│
│ (MinHook)    │ Backend          │ (MMD UI automation)     │
├──────────────┴──────────────────┴───────────────────────┤
│  mmdvr_core — RenderController, Camera, Projection, …   │
└─────────────────────────────────────────────────────────┘
```

`mmdvr_offline.exe` stitches pre-captured faces without D3D/MMD.

## 3. Per-frame pipeline (cubemap ON, default)

1. Seek MMD frame Edit → evaluate pose  
2. Per eye: for each cube face, override main-RT perspective VIEW/PROJ for the whole Present, capture, restore observed camera  
3. Stitch to equirect (VR180 = forward hemisphere); SBS if stereo  
4. Write PNG + `job.json`  
5. After job: FFmpeg MP4, optional AAC from WAV, Spherical UUID inject  

Cubemap OFF = live camera scale only (not true VR FOV).

## 4. Hook policy (ray-mmd-oriented)

| Phase | Policy |
|-------|--------|
| Normal viewing | SetTransform passthrough |
| Face capture | Replace perspective VIEW/PROJ on main-sized RTs for the whole frame; GetTransform returns the same |
| Capture | Before Present, on the Present thread only |
| Cancel / end | Clear capture flags, disable override, restore observed matrices, release RT pool |
| Device state | Restore RT/DS/viewport/scissor only if capture changed them |

`MMDVR_ISOLATE=1..3` disables capture stages for bisecting.

## 5. Outputs

PNG sequence, `job.json`, optional `mmdvr_job.mp4` (with audio), Spherical UUID for YouTube, `mmd_vr_renderer.log`.

## 6. Limits

No official camera API; screen-space effects may seam across faces; seek-based timing may need physics bake; readback is heavy (offline-oriented).
