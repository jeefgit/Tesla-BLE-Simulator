@echo off
setlocal
cd /d "%~dp0"

set "PY=py"
where py >nul 2>nul
if errorlevel 1 set "PY=python"
where %PY% >nul 2>nul
if errorlevel 1 (
  echo [ERROR] 未找到 Python 3，请先安装并加入 PATH。
  pause
  exit /b 1
)

%PY% "%~dp0serve.py"
if errorlevel 1 pause
