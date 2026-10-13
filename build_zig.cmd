@echo off
setlocal
cd /d "%~dp0"
set "PDG_ZIG=zig"
if not "%~1"=="" set "PDG_ZIG=%~1"
"%PDG_ZIG%" cc -std=c11 -O3 -Wall -Wextra -D_WIN32_WINNT=0x0a00 -municode pdg_converter.c -lole32 -loleaut32 -lwindowscodecs -lbcrypt -luuid -o pdg_converter.exe
exit /b %errorlevel%
