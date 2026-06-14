@echo off
setlocal

REM ╔══════════════════════════════════════════════════════════════════════╗
REM ║  Discord Proxied Launcher  v2                                        ║
REM ║                                                                      ║
REM ║  Does THREE things:                                                  ║
REM ║  1. Adds a Windows Firewall rule to block Discord UDP at OS level    ║
REM ║     so WebRTC is FORCED to fall back to TCP TURN relay               ║
REM ║  2. Launches Discord with --proxy-server so ALL TCP (including       ║
REM ║     TURN relay) goes through Xray SOCKS5                             ║
REM ║  3. Passes --force-webrtc-ip-handling-policy to disable direct UDP   ║
REM ╚══════════════════════════════════════════════════════════════════════╝

REM ── Auto-detect Discord version ────────────────────────────────────────
set "DISCORD_EXE="
for /f "tokens=*" %%a in ('dir /b /ad /o:-n "%LOCALAPPDATA%\Discord\app-*" 2^>nul') do (
    if not defined DISCORD_EXE (
        if exist "%LOCALAPPDATA%\Discord\%%a\Discord.exe" (
            set "DISCORD_EXE=%LOCALAPPDATA%\Discord\%%a\Discord.exe"
        )
    )
)
if not defined DISCORD_EXE (
    echo [ERROR] Could not find Discord.exe under %%LOCALAPPDATA%%\Discord
    pause & exit /b 1
)
echo [+] Discord : %DISCORD_EXE%

REM ── Verify Xray is running ──────────────────────────────────────────────
netstat -ano | findstr /C:"127.0.0.1:10808" | findstr LISTENING >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Xray is NOT listening on 127.0.0.1:10808
    echo         Start v2rayN first, then run this launcher.
    pause & exit /b 1
)
echo [+] Xray    : 127.0.0.1:10808 detected

REM ── Add OS-level firewall rule to block Discord UDP ─────────────────────
REM    This guarantees WebRTC falls back to TCP TURN even without the DLL.
netsh advfirewall firewall delete rule name="DiscordBlockUDP" >nul 2>&1
netsh advfirewall firewall add rule ^
    name="DiscordBlockUDP" ^
    program="%DISCORD_EXE%" ^
    protocol=UDP ^
    dir=out ^
    action=block >nul 2>&1
echo [+] Firewall: UDP from Discord blocked (forces TCP TURN fallback)

REM ── Kill existing Discord instance ──────────────────────────────────────
taskkill /f /im Discord.exe >nul 2>&1
timeout /t 2 /nobreak >nul

REM ── Launch Discord with Chromium proxy flags ────────────────────────────
REM    Note: no quotes around the proxy value — Chrome requires bare URL
echo [+] Launching Discord with SOCKS5 proxy...
echo.

start "" "%DISCORD_EXE%" --proxy-server=socks5://127.0.0.1:10808 --proxy-bypass-list=^<-loopback^> --force-webrtc-ip-handling-policy=disable_non_proxied_udp

echo Done.  Discord is starting...
echo Voice will connect in ~5-15 seconds (TCP TURN handshake).
echo.
echo To REMOVE the firewall rule when done, run:
echo   netsh advfirewall firewall delete rule name="DiscordBlockUDP"
echo.
timeout /t 8 /nobreak >nul
