@echo off
REM Builds Viture HUD and runs the geometry tests.
REM Needs MinGW-w64 (g++ and windres) on PATH - see README.md.
setlocal
cd /d "%~dp0"

where g++ >nul 2>nul || (echo [build] g++ not found on PATH & exit /b 1)
where windres >nul 2>nul || (echo [build] windres not found on PATH & exit /b 1)
if not exist build mkdir build

echo [build] icon...
windres -I res res\icon.rc -O coff -o build\icon.o || (echo [build] icon failed & exit /b 1)

echo [build] app...
REM -static bundles the MinGW C++ runtime, so the exe runs on any Windows
REM 10/11 PC without extra DLLs.
g++ -std=c++17 -O2 -municode -mwindows -static -Wall -Wextra -Wno-missing-field-initializers ^
    -o build\viture_hud.exe src\viture_hud.cpp src\viewer.cpp src\panel.cpp build\icon.o ^
    -luser32 -lgdi32 -lshell32 -ladvapi32 -lole32 -ldwmapi -luuid ^
    -ld3d11 -ldxgi -ld3dcompiler_47 -loleacc -loleaut32 -ld2d1 -ldwrite
if errorlevel 1 (echo [build] FAILED & exit /b 1)

echo [build] tests...
g++ -std=c++17 -O2 -Wall -Wextra -I src -o build\test_view_math.exe tests\test_view_math.cpp || (echo [build] tests failed to compile & exit /b 1)
build\test_view_math.exe
if errorlevel 1 (echo [build] TESTS FAILED & exit /b 1)

echo [build] docs...
powershell -NoProfile -ExecutionPolicy Bypass -File tools\check_docs.ps1
if errorlevel 1 (echo [build] README OUT OF DATE - see above & exit /b 1)

echo [build] OK -^> %~dp0build\viture_hud.exe
endlocal
