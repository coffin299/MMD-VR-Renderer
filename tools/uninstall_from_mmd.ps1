#Requires -Version 5.1
param(
  [string]$MmdDir = 'C:\Users\coffi\Desktop\MMMD'
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $MmdDir)) { throw "MMD dir not found: $MmdDir" }

$mmeRenamed = Join-Path $MmdDir 'd3d9_mme.dll'
$proxy = Join-Path $MmdDir 'd3d9.dll'
$named = Join-Path $MmdDir 'd3d9_mmdvr.dll'
$legacy = Join-Path $MmdDir 'mmdvr_d3d9.dll'
$marker = Join-Path $MmdDir 'MMD-VR-Renderer.txt'
$state = Join-Path $MmdDir '.mmdvr_install.json'
$ver = Join-Path $MmdDir '.mmdvr_version'

Write-Host 'Uninstalling MMD-VR-Renderer...'

if (Test-Path $named) {
  Remove-Item $named -Force
  Write-Host 'Removed d3d9_mmdvr.dll'
}
if (Test-Path $legacy) {
  Remove-Item $legacy -Force
  Write-Host 'Removed legacy mmdvr_d3d9.dll'
}
if (Test-Path $marker) { Remove-Item $marker -Force }
if (Test-Path $state) { Remove-Item $state -Force }
if (Test-Path $ver) { Remove-Item $ver -Force }

if (Test-Path $mmeRenamed) {
  if (Test-Path $proxy) { Remove-Item $proxy -Force }
  Rename-Item $mmeRenamed $proxy
  Write-Host 'Restored MME d3d9.dll from d3d9_mme.dll'
} else {
  Write-Warning 'd3d9_mme.dll not found; left d3d9.dll as-is'
}

Write-Host 'MMD-VR-Renderer uninstall complete.'
