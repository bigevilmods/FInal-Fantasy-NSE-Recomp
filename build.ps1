# Build the statically recompiled Final Fantasy executable with MSVC.
param([switch]$Clean)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -products * -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio C++ tools not found" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

if ($Clean) { Remove-Item out\gen.cpp, out\gen.h, *.obj, *.exe -ErrorAction SilentlyContinue }

python tools\recomp.py
if ($LASTEXITCODE -ne 0) { throw "recompiler failed" }

$bat = @"
@echo off
call "$vcvars" >nul
cl /nologo /O2 /utf-8 /std:c++17 /EHsc /W3 /bigobj /D_CRT_SECURE_NO_WARNINGS /I runtime runtime\mem.cpp runtime\ppu.cpp runtime\hd.cpp runtime\apu.cpp runtime\main.cpp runtime\video.cpp runtime\gpu.cpp runtime\state.cpp runtime\session.cpp out\gen.cpp /Fe:ff1.exe /link user32.lib gdi32.lib winmm.lib d3d11.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /utf-8 /std:c++17 /EHsc /W3 /bigobj /D_CRT_SECURE_NO_WARNINGS /I runtime runtime\mem.cpp runtime\ppu.cpp runtime\hd.cpp runtime\apu.cpp runtime\headless.cpp runtime\state.cpp out\gen.cpp /Fe:ff1_headless.exe /link user32.lib gdi32.lib winmm.lib d3d11.lib dxgi.lib d3dcompiler.lib
"@
Set-Content -Path build.bat -Value $bat -Encoding ASCII
cmd /c build.bat
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "built ff1.exe + ff1_headless.exe"
