#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Third = Join-Path $Root 'third_party'

function Ensure-Dir([string]$Path) {
  if (-not (Test-Path $Path)) { New-Item -ItemType Directory -Force -Path $Path | Out-Null }
}

Write-Host '== Fetching MinHook =='
$mhDir = Join-Path $Third 'minhook'
Ensure-Dir $mhDir
$mhZip = Join-Path $env:TEMP 'minhook.zip'
$mhUrl = 'https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.3.zip'
if (-not (Test-Path (Join-Path $mhDir 'include\MinHook.h'))) {
  Invoke-WebRequest -Uri $mhUrl -OutFile $mhZip
  $extract = Join-Path $env:TEMP 'minhook_extract'
  if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
  Expand-Archive -Path $mhZip -DestinationPath $extract -Force
  $src = Get-ChildItem $extract -Directory | Select-Object -First 1
  Copy-Item (Join-Path $src.FullName '*') $mhDir -Recurse -Force
  Write-Host "MinHook installed to $mhDir"
} else {
  Write-Host 'MinHook already present'
}

Write-Host '== Ensuring stb_image_write.h =='
$stb = Join-Path $Third 'stb'
Ensure-Dir $stb
$stbFile = Join-Path $stb 'stb_image_write.h'
if (-not (Test-Path $stbFile)) {
  Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h' -OutFile $stbFile
}

Write-Host '== Ensuring nlohmann/json.hpp =='
$jsonDir = Join-Path $Third 'json\nlohmann'
Ensure-Dir $jsonDir
$jsonFile = Join-Path $jsonDir 'json.hpp'
if (-not (Test-Path $jsonFile)) {
  Invoke-WebRequest -Uri 'https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp' -OutFile $jsonFile
}

Write-Host '== Optional FFmpeg (essentials) =='
$ffDir = Join-Path $Root 'tools\ffmpeg'
Ensure-Dir $ffDir
$ffExe = Join-Path $ffDir 'ffmpeg.exe'
if (-not (Test-Path $ffExe)) {
  Write-Host 'Downloading FFmpeg essentials build...'
  $ffZip = Join-Path $env:TEMP 'ffmpeg-essentials.zip'
  # BtbN shared essentials (win64)
  $ffUrl = 'https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip'
  try {
    Invoke-WebRequest -Uri $ffUrl -OutFile $ffZip
    $ffExtract = Join-Path $env:TEMP 'ffmpeg_extract'
    if (Test-Path $ffExtract) { Remove-Item $ffExtract -Recurse -Force }
    Expand-Archive -Path $ffZip -DestinationPath $ffExtract -Force
    $bin = Get-ChildItem $ffExtract -Recurse -Filter 'ffmpeg.exe' | Select-Object -First 1
    if ($bin) {
      Copy-Item $bin.FullName $ffExe -Force
      $ffprobe = Join-Path $bin.DirectoryName 'ffprobe.exe'
      if (Test-Path $ffprobe) { Copy-Item $ffprobe (Join-Path $ffDir 'ffprobe.exe') -Force }
      Write-Host "ffmpeg.exe -> $ffExe"
    }
  } catch {
    Write-Warning "FFmpeg download failed: $_. Video encode will require PATH ffmpeg."
  }
} else {
  Write-Host 'ffmpeg already present'
}

Write-Host 'Done.'
