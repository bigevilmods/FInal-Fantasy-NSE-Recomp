@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /utf-8 /std:c++17 /EHsc /W3 /bigobj /D_CRT_SECURE_NO_WARNINGS /I runtime runtime\mem.cpp runtime\ppu.cpp runtime\hd.cpp runtime\apu.cpp runtime\main.cpp runtime\video.cpp runtime\gpu.cpp runtime\state.cpp runtime\session.cpp out\gen.cpp /Fe:ff1.exe /link user32.lib gdi32.lib winmm.lib d3d11.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /utf-8 /std:c++17 /EHsc /W3 /bigobj /D_CRT_SECURE_NO_WARNINGS /I runtime runtime\mem.cpp runtime\ppu.cpp runtime\hd.cpp runtime\apu.cpp runtime\headless.cpp runtime\state.cpp out\gen.cpp /Fe:ff1_headless.exe /link user32.lib gdi32.lib winmm.lib d3d11.lib dxgi.lib d3dcompiler.lib
