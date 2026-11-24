# Build script for Clipboard Viewer (single-file Windows executable)
# Usage: powershell -ExecutionPolicy Bypass -File build.ps1
# Requires MinGW-w64 (w64devkit recommended): https://github.com/skeeto/w64devkit

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$src  = Join-Path $root 'src'
$dist = Join-Path $root 'dist'
$out  = Join-Path $dist 'ClipboardViewer.exe'

# Locate the compiler: PATH first, then common install locations
$candidates = @(
    'g++',
    'D:\c\w64devkit\bin\g++.exe',
    'C:\msys64\mingw64\bin\g++.exe',
    'C:\ProgramData\chocolatey\bin\g++.exe'
)
$gpp = $null
foreach ($c in $candidates) {
    $cmd = Get-Command $c -ErrorAction SilentlyContinue
    if ($cmd) { $gpp = $cmd.Source; break }
}
if (-not $gpp) {
    throw 'g++ not found. Install MinGW-w64 (w64devkit recommended) and add it to PATH.'
}

$windres = Join-Path (Split-Path $gpp) 'windres.exe'
if (-not (Test-Path $windres)) {
    $windres = (Get-Command windres -ErrorAction SilentlyContinue).Source
}
if (-not $windres) {
    throw 'windres not found. It ships with MinGW-w64.'
}

New-Item -ItemType Directory -Path $dist -Force | Out-Null

# 1) Compile resources (manifest: modern controls + DPI awareness)
$res = Join-Path $dist 'app.res'
& $windres (Join-Path $src 'app.rc') -O coff -o $res

# 2) Compile and link (static: single file, no runtime dependency)
& $gpp -O2 -s -mwindows -static -static-libgcc -static-libstdc++ `
    -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    -o $out (Join-Path $src 'main.cpp') $res `
    -lgdi32 -lcomctl32 -lshell32 -lcomdlg32 -luser32 -lole32 -luuid

if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

Write-Host "Build succeeded: $out" -ForegroundColor Green
Get-Item $out | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
