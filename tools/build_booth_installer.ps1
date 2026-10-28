#Requires -Version 5.1
# Publish BOOTH installer as a single self-contained exe.
# Output: dist\booth\MMD-VR-Renderer-Installer.exe
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Proj = Join-Path $PSScriptRoot 'booth_installer\MmdVrBoothInstaller.csproj'
$Out = Join-Path $Root 'dist\booth'

if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
  throw 'dotnet SDK not found. Install .NET 8 SDK.'
}

New-Item -ItemType Directory -Force -Path $Out | Out-Null

dotnet publish $Proj `
  -c Release `
  -r win-x64 `
  --self-contained true `
  -p:PublishSingleFile=true `
  -p:IncludeNativeLibrariesForSelfExtract=true `
  -p:EnableCompressionInSingleFile=true `
  -p:DebugType=none `
  -p:DebugSymbols=false `
  -o $Out

$exe = Join-Path $Out 'MMD-VR-Renderer-Installer.exe'
if (-not (Test-Path $exe)) {
  throw "publish failed: $exe missing"
}
Get-Item $exe | Format-List FullName, Length, LastWriteTime
Write-Host "Upload this exe to BOOTH: $exe"
Write-Host 'User picks MMD folder; installer fetches latest GitHub Release and installs/updates.'
