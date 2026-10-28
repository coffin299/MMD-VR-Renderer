# Build

Japanese: [../BUILD.md](../BUILD.md)

## Requirements

- Windows 10/11 x64
- Visual Studio 2019 or 2022 (C++ desktop)
- CMake 3.20+
- Internet once for `tools/fetch_deps.ps1` (MinHook, stb, nlohmann/json, optional FFmpeg)

## Steps

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\fetch_deps.ps1
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release
.\build\Release\mmdvr_tests.exe
```

Outputs: `d3d9_mmdvr.dll` / `d3d9.dll`, `mmdvr_tests.exe`, `mmdvr_offline.exe` under `build/Release/`.

## Install into MMD

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\install_to_mmd.ps1 -MmdDir 'C:\path\to\MMD'
```

Uninstall: `tools\uninstall_from_mmd.ps1`.

| File | Meaning |
|------|---------|
| `d3d9.dll` | Proxy loaded by MMD |
| `d3d9_mmdvr.dll` | Same bytes (uninstall marker) |
| `d3d9_mme.dll` | Original MME `d3d9.dll` renamed |

## BOOTH installer

Requires .NET 8 SDK:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_booth_installer.ps1
```

Output: `dist/booth/MMD-VR-Renderer-Installer.exe`
