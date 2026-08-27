# Package the built plugin into a distributable zip (run after cmake --build).
# Usage:  powershell -ExecutionPolicy Bypass -File package.ps1
# Result: obs-ai-matte-dist.zip at repo root -> unzip into OBS install dir.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root "build"

$plugin = Get-ChildItem -Path $build -Recurse -Filter "obs-ai-matte.dll" | Select-Object -First 1
if (-not $plugin) {
    Write-Error "obs-ai-matte.dll not found. Build first with: cmake --build build --config RelWithDebInfo"
    exit 1
}

$out = Join-Path $root "dist"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
$plugins64 = Join-Path $out "obs-plugins\64bit"
$data = Join-Path $out "data\obs-plugins\obs-ai-matte"
New-Item -ItemType Directory -Force -Path $plugins64 | Out-Null
New-Item -ItemType Directory -Force -Path $data | Out-Null

Copy-Item (Join-Path $plugin.Directory "obs-ai-matte.dll") $plugins64
foreach ($d in @("onnxruntime.dll", "onnxruntime_providers_dml.dll", "DirectML.dll")) {
    $src = Join-Path $plugin.Directory $d
    if (Test-Path $src) { Copy-Item $src $plugins64 }
}

$modelSrc = Join-Path $root "data\obs-ai-matte"
if (Test-Path $modelSrc) { Copy-Item (Join-Path $modelSrc "*.onnx") $data }

$zip = Join-Path $root "obs-ai-matte-dist.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $out "*") -DestinationPath $zip
Write-Host "Packaged -> $zip  (unzip into your OBS Studio install directory)"
