# Technical research

Japanese: [../TECHNICAL_RESEARCH.md](../TECHNICAL_RESEARCH.md)

## References

- [cchina33/MMD-Plugin-Loader-Injection-Hooking](https://github.com/cchina33/MMD-Plugin-Loader-Injection-Hooking): MME-compatible `d3d9` → `d3d9_mme.dll` chain; Present capture pattern.  
- oigami MMDPlugin: multi-plugin hub — **not required** for raw MME installs.  
- MMDX12: separate DX12 app — **reference only**.  
- Community VR workflows (EquirectangularX, DomeMaster-style cubemap): inform projection design.

## MMDExport (local 9.32 `Data/MMDExport.h`)

Available: `ExpGetFrameTime`, bone/accessory queries, `ExpSetRenderRepeatCount`.  
**Not available:** camera eye/target setters.

## Risks

1. SetTransform override may be overwritten each pass; multi-view quality depends on timing.  
2. Soul-chan `MMDMainData` RVAs target 9.31 — **not used** on 9.32.  
3. `GetRenderTargetData` is slow; offline only.  
4. Stereo + ray-mmd SSR will not match a true single-pass panoramic BRDF.

## Decisions

| Decision | Choice | Reason |
|----------|--------|--------|
| Integration | d3d9 proxy chain | Matches installed MME; no MMDPlugin.lib |
| Projection | Cubemap stitch | Offline-testable; DomeMaster-like practice |
| Encoder | External FFmpeg | Avoid in-process GPL redistribution |
| Renderer | Keep MMD+MME | Spec priority #1 |
| Multi-view wait | Background thread + Present CV | UI-thread Present wait deadlocks |
| YouTube VR | Spherical UUID inject | ffmpeg `-metadata` tags alone are ignored |
| Animation advance | UI frame Edit seek | No internal frame API |

## Empirical notes (MMD 9.32 + MME 0.37)

- Pre-Present StretchRect → SYSTEMMEM: **works**  
- VIEW override while Present continues: **works**  
- UI-thread Present wait: **fails**  
- Face vs deferred matrix mismatch: can shred → unify GetTransform during face capture  
