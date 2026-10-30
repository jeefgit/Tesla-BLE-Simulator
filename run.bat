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
py -m pip install -r "%~dp0requirements.txt" -q
echo.

:: ── Launch simulator ──────────────────────────────────────────────
echo [+] Starting BLE Telemetry Simulation Service (Admin)...
cd /d "%~dp0"

:: ── Client pairing whitelist (simulates a paired Tesla vehicle) ────
:: Must match ESP32 key: TESLA-DASHBOARD\include\secrets\tesla_client_key.h
:: After changing the ESP32 private key, re-derive the public key with:
::   py -3 -c "from cryptography.hazmat.primitives.asymmetric import ec;from cryptography.hazmat.primitives import serialization as s;k=ec.derive_private_key(int('PRIVHEX',16),ec.SECP256R1());print(k.public_key().public_bytes(s.Encoding.X962,s.PublicFormat.UncompressedPoint).hex())"
:: Remove --client-pub below to disable the whitelist (any valid key accepted).
set "CLIENT_PUB=0415358e9fb5d8baf98116fc7dba8eb803524c1472c96c9c25c7eb6f099ebca51621231cbe38bbb5edf17127740c58e72bf39cb8fceb05e6af8ca411587f8ebc91"

py sim_server.py --client-pub %CLIENT_PUB%

pause
