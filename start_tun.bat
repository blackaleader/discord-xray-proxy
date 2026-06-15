@echo off
setlocal EnableDelayedExpansion

REM ╔══════════════════════════════════════════════════════════════════════╗
REM ║  discord-xray-proxy  ─  TUN Launcher                                ║
REM ║  github.com/blackaleader/discord-xray-proxy                         ║
REM ║                                                                      ║
REM ║  Routes ALL Discord traffic (TCP + UDP voice) through Xray via a    ║
REM ║  sing-box TUN adapter — works even inside Chromium's sandbox.        ║
REM ║                                                                      ║
REM ║  Requires: Administrator privileges                                  ║
REM ║            Xray / v2rayN running on 127.0.0.1:10808                  ║
REM ╚══════════════════════════════════════════════════════════════════════╝

REM ── Re-launch as Administrator if not already ──────────────────────────
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [!] Requesting Administrator privileges...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

REM ── All paths relative to this script's directory ──────────────────────
set "ROOT=%~dp0"
set "SINGBOX=%ROOT%tools\sing-box.exe"
set "CONFIG=%ROOT%sing-box-tun.json"
set "LOGDIR=%ROOT%logs"

REM ── Sanity checks ──────────────────────────────────────────────────────
if not exist "%SINGBOX%" (
    echo [ERROR] tools\sing-box.exe not found.
    echo         Clone the full repo: https://github.com/blackaleader/discord-xray-proxy
    pause & exit /b 1
)

if not exist "%ROOT%tools\wintun.dll" (
    echo [ERROR] tools\wintun.dll not found.
    pause & exit /b 1
)

REM ── Verify Xray is running on 10808 ────────────────────────────────────
netstat -ano | findstr /C:"127.0.0.1:10808" | findstr LISTENING >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Xray / v2rayN is NOT listening on 127.0.0.1:10808
    echo         Start v2rayN first, then run this script again.
    pause & exit /b 1
)
echo [+] Xray detected on 127.0.0.1:10808

REM ── Create logs folder ─────────────────────────────────────────────────
if not exist "%LOGDIR%" mkdir "%LOGDIR%"

REM ── Stop old sing-box instance if any ─────────────────────────────────
taskkill /f /im sing-box.exe >nul 2>&1
timeout /t 1 /nobreak >nul

REM ── Kill Discord so it restarts without old DLL hooks ─────────────────
taskkill /f /im Discord.exe /t >nul 2>&1
taskkill /f /im DiscordPTB.exe /t >nul 2>&1
taskkill /f /im DiscordCanary.exe /t >nul 2>&1
timeout /t 2 /nobreak >nul

REM ── Start sing-box TUN (wintun.dll must be alongside sing-box.exe) ─────
echo [+] Starting sing-box TUN adapter...
cd /d "%ROOT%tools"
start "sing-box-tun" "%SINGBOX%" run -c "%CONFIG%"
cd /d "%ROOT%"

REM ── Wait for TUN interface to initialise ──────────────────────────────
echo [*] Waiting for TUN to come up (4 seconds)...
timeout /t 4 /nobreak >nul

REM ── Auto-detect latest Discord install ────────────────────────────────
set "DISCORD_EXE="
for /f "tokens=*" %%a in ('dir /b /ad /o:-n "%LOCALAPPDATA%\Discord\app-*" 2^>nul') do (
    if not defined DISCORD_EXE (
        if exist "%LOCALAPPDATA%\Discord\%%a\Discord.exe" (
            set "DISCORD_EXE=%LOCALAPPDATA%\Discord\%%a\Discord.exe"
        )
    )
)
if not defined DISCORD_EXE (
    echo [ERROR] Discord.exe not found in %%LOCALAPPDATA%%\Discord
    echo         Install Discord from https://discord.com/download
    pause & exit /b 1
)

REM ── Launch Discord ─────────────────────────────────────────────────────
echo [+] Launching Discord: %DISCORD_EXE%
start "" "%DISCORD_EXE%"

echo.
echo  ╔═══════════════════════════════════════════════════════════╗
echo  ║  Discord is running — all traffic routes through Xray     ║
echo  ║  Voice UDP is fully proxied via TUN adapter               ║
echo  ║                                                           ║
echo  ║  Log: logs\sing-box-tun.log                               ║
echo  ║  To stop: run stop_tun.bat (as Admin)                     ║
echo  ╚═══════════════════════════════════════════════════════════╝
echo.
pause
