@echo off
title Tesla Dash HMI Simulator Launcher
color 0B

:: ── Auto-elevate to Administrator ──────────────────────────────────
:: Required because name_overwrite=True needs to rename the Windows BT adapter.
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [!] Requesting Administrator privileges...
    powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo ==========================================================
echo        Tesla BLE HMI Simulator  [Administrator Mode]
echo ==========================================================
echo.

:: ── Check Python ──────────────────────────────────────────────────
py --version >nul 2>&1
if %errorlevel% neq 0 (
    color 0C
    echo [-] Error: Python 3 not found. Please install Python 3 and add to PATH.
    pause
    exit /b
)

:: ── Install dependencies ──────────────────────────────────────────
echo [+] Validating dependencies...
py -m pip install -r requirements.txt -q
echo.

:: ── Launch simulator ──────────────────────────────────────────────
echo [+] Starting BLE Telemetry Simulation Service (Admin)...
cd /d "%~dp0"
py sim_server.py

pause
