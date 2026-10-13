@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
    echo Open an x64 Native Tools Command Prompt for Visual Studio first.
    exit /b 1
)
cl /nologo /std:c11 /O2 /W4 /utf-8 /D_WIN32_WINNT=0x0a00 pdg_converter.c /Fe:pdg_converter.exe /link ole32.lib oleaut32.lib windowscodecs.lib bcrypt.lib uuid.lib
exit /b %errorlevel%
