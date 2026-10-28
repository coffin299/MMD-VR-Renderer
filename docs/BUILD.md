# ビルド

## 要件

- Windows 10／11 x64
- Visual Studio 2019 または 2022（C++ デスクトップ）
- CMake 3.20 以上（VS 同梱でも可）
- 初回のみネット: `tools/fetch_deps.ps1`（MinHook、stb、nlohmann/json、任意で FFmpeg）

## 手順

```powershell
cd <リポジトリルート>
powershell -ExecutionPolicy Bypass -File .\tools\fetch_deps.ps1

cmake -S . -B build -G "Visual Studio 16 2019" -A x64
# VS2022 なら: -G "Visual Studio 17 2022"
cmake --build build --config Release
.\build\Release\mmdvr_tests.exe
```

成果物:

- `build/Release/d3d9_mmdvr.dll` および同内容の `d3d9.dll`
- `build/Release/mmdvr_tests.exe`
- `build/Release/mmdvr_offline.exe`

## MMD への導入

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\install_to_mmd.ps1 -MmdDir 'C:\path\to\MMD'
```

削除:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\uninstall_from_mmd.ps1
```

配置の意味:

| ファイル | 説明 |
|----------|------|
| `d3d9.dll` | MMD が読むプロキシ本体 |
| `d3d9_mmdvr.dll` | 同内容の別名（削除時の目印） |
| `d3d9_mme.dll` | 元の MME `d3d9.dll` を改名したもの |

## BOOTH インストーラー（GitHub 最新 → MMD）

.NET 8 SDK が必要です。自己完結の単一 exe を出力します。

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_booth_installer.ps1
```

- ソース: `tools/booth_installer/`
- 出力: `dist/booth/MMD-VR-Renderer-Installer.exe`（`dist/` は gitignore）
- 挙動: `releases/latest` を問い合わせ、`*-win64.zip` を取得して指定 MMD へ導入

English: [en/BUILD.md](en/BUILD.md)
