# Compatibility matrix

Japanese: [../COMPATIBILITY_MATRIX.md](../COMPATIBILITY_MATRIX.md)

| Component | Version / path | Status | Notes |
|-----------|----------------|--------|-------|
| MikuMikuDance | 9.32 x64 | **Verified** | Boots with proxy |
| MikuMikuEffect | 0.37 as `d3d9_mme.dll` | **Verified** | Chain load logged |
| ray-mmd | 1.5.0 under UserFile | Present / **partial** | Single-frame OK; multi-view SSR seams scene-dependent |
| MMDPlugin | Not installed | N/A | Not required |
| MMDX12 | — | Not used | Reference only |
| Windows | 10/11 x64 | **Verified** | Dev machine |
| VS Build | 2019 BuildTools / VS2022 | **Verified** | CMake VS 16 2019 generator etc. |

## Runtime checks (reference)

Proxy load, MME chain, hooks, VR menu, capture PNG, camera probe, unit tests (7), offline compose: Pass.  
Full multi-frame ray-mmd stereo encode: manual via UI.  
PMM non-mutation: by design (no PMM writes).
