#Requires -Version 5.1
param(
  [string]$MmdDir = 'C:\Users\coffi\Desktop\MMMD',
  [string]$BuildConfig = 'Release',
  [string]$BuildDir = ''
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) {
  $BuildDir = Join-Path $Root "build\$BuildConfig"
}

function Find-BuiltDll([string]$name) {
  $candidates = @(
    (Join-Path $BuildDir $name),
    (Join-Path $Root "build\$BuildConfig\$name")
  )
  foreach ($c in $candidates) {
    if (Test-Path $c) { return (Resolve-Path $c).Path }
  }
  $hit = Get-ChildItem (Join-Path $Root 'build') -Recurse -Filter $name -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match [regex]::Escape($BuildConfig) } |
    Select-Object -First 1
  if ($hit) { return $hit.FullName }
  return $null
}

# Prefer named build; fall back to d3d9.dll copy from POST_BUILD.
$plugin = Find-BuiltDll 'd3d9_mmdvr.dll'
if (-not $plugin) { $plugin = Find-BuiltDll 'd3d9.dll' }
if (-not $plugin) { throw "d3d9_mmdvr.dll / d3d9.dll not found under build. Build target d3d9_mmdvr first." }
if (-not (Test-Path $MmdDir)) { throw "MMD dir not found: $MmdDir" }

$backup = Join-Path $MmdDir '.mmdvr_backup'
New-Item -ItemType Directory -Force -Path $backup | Out-Null
$mmeD3d9 = Join-Path $MmdDir 'd3d9.dll'
$mmeRenamed = Join-Path $MmdDir 'd3d9_mme.dll'
$named = Join-Path $MmdDir 'd3d9_mmdvr.dll'
$legacy = Join-Path $MmdDir 'mmdvr_d3d9.dll'
$marker = Join-Path $MmdDir 'MMD-VR-Renderer.txt'
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'

if ((Test-Path $mmeD3d9) -and -not (Test-Path $mmeRenamed)) {
  Copy-Item $mmeD3d9 (Join-Path $backup "d3d9_mme_$stamp.dll") -Force
  Rename-Item $mmeD3d9 $mmeRenamed
  Write-Host "Renamed MME d3d9.dll -> d3d9_mme.dll"
} elseif ((Test-Path $mmeD3d9) -and (Test-Path $mmeRenamed)) {
  Copy-Item $mmeD3d9 (Join-Path $backup "d3d9_proxy_$stamp.dll") -Force
  Write-Host 'Updating existing MMD-VR-Renderer install'
}

if (Test-Path $legacy) { Remove-Item $legacy -Force }

# Same binary twice: MMD loads d3d9.dll; d3d9_mmdvr.dll is the visible twin (like d3d9_mme.dll).
Copy-Item $plugin $mmeD3d9 -Force
Copy-Item $plugin $named -Force

@'
MMD-VR-Renderer がこのフォルダに導入されています。
MMD-VR-Renderer is installed in this folder.

  d3d9.dll       … MMD が読み込む本体（プロキシ）
  d3d9_mmdvr.dll … 同じ本体の別名コピー（アンインストールの目印 / MME の d3d9_mme.dll に対応）
  d3d9_mme.dll   … MME（退避済み）

削除 / Uninstall:
  tools\uninstall_from_mmd.ps1
  または BOOTH インストーラーのアンインストール
'@ | Set-Content -Path $marker -Encoding UTF8

Write-Host "Installed: $plugin -> $mmeD3d9"
Write-Host "Named copy: $named"
if (-not (Test-Path $mmeRenamed)) {
  Write-Warning 'd3d9_mme.dll missing. MME will not load until MME d3d9 is placed as d3d9_mme.dll'
}
Write-Host 'Done. Launch MikuMikuDance.exe and open menu VR Renderer / MMEffect.'
