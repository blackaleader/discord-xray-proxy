@echo off
net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)
taskkill /f /im sing-box.exe >nul 2>&1
echo [+] TUN stopped. Discord traffic is now direct again.
timeout /t 2 /nobreak >nul
