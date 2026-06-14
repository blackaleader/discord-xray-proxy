@echo off
setlocal

REM ── Must run as Administrator ──────────────────────────────────────────────
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [!] Re-launching as Administrator...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

set "ROOT=%~dp0"
set "SINGBOX=C:\Users\arman\Desktop\v2rayN-windows-64-desktop\v2rayN-windows-64\bin\sing_box\sing-box.exe"
set "CONFIG=%ROOT%sing-box-tun.json"

REM ── Check Xray is running ──────────────────────────────────────────────────
netstat -ano | findstr /C:"127.0.0.1:10808" | findstr LISTENING >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Xray not running on 127.0.0.1:10808 - start v2rayN first!
    pause & exit /b 1
)
echo [+] Xray is running on 127.0.0.1:10808

REM ── Kill old sing-box if running ───────────────────────────────────────────
taskkill /f /im sing-box.exe >nul 2>&1
timeout /t 1 /nobreak >nul

REM ── Kill Discord so it starts fresh after TUN is up ───────────────────────
taskkill /f /im Discord.exe >nul 2>&1
timeout /t 1 /nobreak >nul

REM ── Start sing-box TUN ─────────────────────────────────────────────────────
echo [+] Starting sing-box TUN (Discord-only proxy mode)...
cd /d "%ROOT%"
start "sing-box-tun" "%SINGBOX%" run -c "%CONFIG%"

REM ── Wait for TUN interface to come up ─────────────────────────────────────
echo [*] Waiting for TUN to initialise...
timeout /t 4 /nobreak >nul

REM ── Detect Discord path ────────────────────────────────────────────────────
set "DISCORD_EXE="
for /f "tokens=*" %%a in ('dir /b /ad /o:-n "%LOCALAPPDATA%\Discord\app-*" 2^>nul') do (
    if not defined DISCORD_EXE (
        if exist "%LOCALAPPDATA%\Discord\%%a\Discord.exe" (
            set "DISCORD_EXE=%LOCALAPPDATA%\Discord\%%a\Discord.exe"
        )
    )
)
if not defined DISCORD_EXE (
    echo [ERROR] Discord.exe not found
    pause & exit /b 1
)

REM ── Launch Discord ─────────────────────────────────────────────────────────
echo [+] Launching Discord...
start "" "%DISCORD_EXE%"

echo.
echo  Discord is running through TUN proxy.
echo  ALL traffic (text + voice UDP) routes through Xray.
echo.
echo  Check sing-box-tun.log for connection details.
echo  Run stop_tun.bat to stop the TUN when done.
echo.
pause
