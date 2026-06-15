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

REM ══════════════════════════════════════════════════════════════════════
REM  USER CONFIGURATION  ─  edit these two lines to match your setup
set "PROXY_HOST=127.0.0.1"
set "PROXY_PORT=10808"
REM ══════════════════════════════════════════════════════════════════════

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

REM ── Verify Xray is running on the configured port ─────────────────────
netstat -ano | findstr /C:"%PROXY_HOST%:%PROXY_PORT%" | findstr LISTENING >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] No proxy detected on %PROXY_HOST%:%PROXY_PORT%
    echo.
    echo  Fix: open start_tun.bat and update PROXY_HOST / PROXY_PORT
    echo  to match your Xray / v2rayN SOCKS5 inbound port.
    echo.
    echo  Common ports:  10808 ^(v2rayN default^)  1080  7890
    pause & exit /b 1
)
echo [+] Proxy detected on %PROXY_HOST%:%PROXY_PORT%

REM ── Write sing-box config with the correct proxy address ───────────────
(
echo {
echo   "log": { "level": "info", "output": "logs/sing-box-tun.log", "timestamp": true },
echo   "inbounds": [{
echo     "type": "tun", "tag": "tun-in",
echo     "address": ["172.19.0.1/30", "fdfe:dcba:9876::1/126"],
echo     "mtu": 9000, "auto_route": true, "strict_route": false, "stack": "system"
echo   }],
echo   "outbounds": [
echo     { "type": "socks", "tag": "xray-socks5", "server": "%PROXY_HOST%", "server_port": %PROXY_PORT%, "version": "5" },
echo     { "type": "direct", "tag": "direct" }
echo   ],
echo   "route": {
echo     "rules": [
echo       { "ip_cidr": ["127.0.0.0/8", "::1/128", "172.19.0.0/30"], "action": "route", "outbound": "direct" },
echo       { "process_name": ["Discord.exe", "DiscordPTB.exe", "DiscordCanary.exe"], "action": "route", "outbound": "xray-socks5" }
echo     ],
echo     "final": "direct", "auto_detect_interface": true
echo   }
echo }
) > "%CONFIG%"

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
