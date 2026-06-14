@echo off
setlocal EnableDelayedExpansion

REM ──────────────────────────────────────────────────────────────────────────
REM  Discord SOCKS5 Proxy  ─  Build Script
REM  Requires: Visual Studio 2019 or 2022 (x64 Native Tools Command Prompt)
REM  OR run this from a regular cmd and we'll try to find MSVC automatically.
REM ──────────────────────────────────────────────────────────────────────────

echo.
echo  ┌─────────────────────────────────────────┐
echo  │  Discord SOCKS5 Proxy  ─  Build Script  │
echo  └─────────────────────────────────────────┘
echo.

REM ── Locate cl.exe if not already in PATH ──────────────────────────────────
where cl.exe >nul 2>&1
if %errorlevel% neq 0 (
    echo [*] cl.exe not in PATH, searching for Visual Studio...

    REM Try VS 2022 first, then 2019
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (
            `"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`
        ) do set "VS_PATH=%%i"

        if defined VS_PATH (
            call "!VS_PATH!\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
            echo [+] Found Visual Studio at !VS_PATH!
        ) else (
            echo [ERROR] Visual Studio with C++ tools not found.
            echo         Install VS 2019/2022 with "Desktop development with C++" workload.
            pause
            exit /b 1
        )
    ) else (
        echo [ERROR] vswhere.exe not found.  Install Visual Studio 2019 or 2022.
        pause
        exit /b 1
    )
)

REM ── Create output directory ────────────────────────────────────────────────
if not exist "bin" mkdir bin

REM ── Compiler flags ────────────────────────────────────────────────────────
set "CL_FLAGS=/nologo /std:c++17 /O2 /W3 /EHsc /I src"
set "LINK_FLAGS=/MACHINE:X64"

REM ── Build proxy_hook.dll ──────────────────────────────────────────────────
echo [*] Building proxy_hook.dll ...

cl.exe %CL_FLAGS% ^
    /D_USRDLL /D_WINDLL ^
    src\proxy_hook.cpp ^
    /link %LINK_FLAGS% ^
    /DLL ^
    /OUT:bin\proxy_hook.dll ^
    ws2_32.lib psapi.lib

if %errorlevel% neq 0 (
    echo.
    echo [ERROR] proxy_hook.dll build FAILED.
    pause
    exit /b 1
)
echo [+] proxy_hook.dll  OK

REM ── Build discord_proxy.exe (the injector) ────────────────────────────────
echo [*] Building discord_proxy.exe ...

cl.exe %CL_FLAGS% ^
    src\injector.cpp ^
    /link %LINK_FLAGS% ^
    /OUT:bin\discord_proxy.exe ^
    ws2_32.lib psapi.lib

if %errorlevel% neq 0 (
    echo.
    echo [ERROR] discord_proxy.exe build FAILED.
    pause
    exit /b 1
)
echo [+] discord_proxy.exe  OK

REM ── Clean up intermediate files ───────────────────────────────────────────
del /q *.obj *.exp *.lib 2>nul

echo.
echo  ┌──────────────────────────────────────────────────────────────┐
echo  │  Build complete!  Output is in the  bin\  folder.           │
echo  │                                                              │
echo  │  To use:                                                     │
echo  │    1. Make sure Xray / V2Ray is running on 127.0.0.1:10808  │
echo  │    2. Right-click discord_proxy.exe → Run as Administrator   │
echo  │    3. Start (or restart) Discord                             │
echo  └──────────────────────────────────────────────────────────────┘
echo.

pause
