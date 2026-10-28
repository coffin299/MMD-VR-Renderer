# Testing

Japanese: [../TESTING.md](../TESTING.md)

## Automated

```powershell
cmake --build build --config Release
.\build\Release\mmdvr_tests.exe
.\build\Release\mmdvr_offline.exe --vr180 --stereo --out dist\sample_vr180
.\build\Release\mmdvr_offline.exe --vr360 --mono --out dist\sample_vr360
```

| Suite | Result (reference) |
|-------|---------------------|
| Projection / stereo / job / math (7) | Pass |
| Offline VR180 SBS + VR360 mono | Pass |

## Manual / live MMD

1. `tools\install_to_mmd.ps1`  
2. Launch MMD → **MMEffect** + **VR Renderer** menus  
3. **Capture Current Frame** → PNG under `mmdvr_captures`  
4. **Camera Override Probe** → base vs yaw PNGs should differ  
5. **Open VR Renderer...** → short 1–2 frame job  
6. Optional PMM hash before/after (tool does not write PMM)
