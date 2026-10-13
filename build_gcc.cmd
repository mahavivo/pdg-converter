@echo off
setlocal
cd /d "%~dp0"
where gcc >nul 2>nul
if errorlevel 1 (
    echo A 64-bit MinGW-w64 GCC compiler is required to rebuild this program.
    exit /b 1
)
gcc -std=c11 -O3 -Wall -Wextra -D_WIN32_WINNT=0x0a00 -municode pdg_converter.c -lole32 -loleaut32 -lwindowscodecs -lbcrypt -luuid -o pdg_converter.exe
exit /b %errorlevel%
