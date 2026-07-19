@echo off
title Tesla Dash HMI Simulator Launcher
color 0B

echo ==========================================================
echo        Tesla BLE HMI Windows Simulator Launcher
echo ==========================================================
echo.

:: 1. Check Python Environment
python --version >nul 2>&1
if %errorlevel% neq 0 (
    color 0C
    echo [-] Error: Python 3 was not detected on this system.
    echo Please install Python 3 and add it to your system PATH.
    pause
    exit /b
)

:: 2. Verify and Install Dependencies
echo [+] Validating and installing dependencies...
python -m pip install -r requirements.txt
if %errorlevel% neq 0 (
    echo [-] Failed to install dependencies. Please check your network connection.
    pause
    exit /b
)
echo.

:: 3. Start BLE Simulation Service
echo [+] Starting BLE Telemetry Simulation Service...
echo [+] Please ensure Bluetooth is enabled in your Windows settings.
python sim_server.py

pause
