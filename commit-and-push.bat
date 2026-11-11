@echo off
setlocal
cd /d "%~dp0"
if errorlevel 1 goto failed

git rev-parse --show-toplevel >nul 2>&1
if errorlevel 1 (
  echo Error: this script must be inside a Git repository.
  goto failed
)

for /f "delims=" %%B in ('git rev-parse --abbrev-ref HEAD') do set "branch=%%B"
if not defined branch (
  echo Error: could not determine the current branch.
  goto failed
)
if "%branch%"=="HEAD" (
  echo Error: HEAD is detached. Check out a branch before committing.
  goto failed
)

git remote get-url origin >nul 2>&1
if errorlevel 1 (
  echo Error: no Git remote named origin is configured.
  goto failed
)

echo.
echo Current branch detected.

:commit_loop
echo Current changes:
git status --short
echo.
echo Staging all changes...
git add -A
if errorlevel 1 goto failed

git diff --cached --quiet
if not errorlevel 1 (
  echo Nothing staged to commit.
  goto ask_next
)

git diff --cached --check
if errorlevel 1 (
  echo Error: staged changes have whitespace problems. Fix them before committing.
  goto failed
)

echo.
echo Staged changes:
git diff --cached --stat
echo.
set "commit_message="
set /p "commit_message=Commit message: "
if not defined commit_message (
  echo Error: commit message cannot be empty.
  goto failed
)

git commit -m "%commit_message%"
if errorlevel 1 goto failed

git push -u origin "%branch%"
if errorlevel 1 (
  echo.
  echo The commit was created, but the push failed.
  echo Fix the push issue, then run: git push
  goto failed
)

echo.
echo Commit and push completed successfully.
goto ask_next

:ask_next
echo.
choice /C YN /M "Continue with the next commit"
if errorlevel 2 goto finished
if errorlevel 1 goto commit_loop
goto failed

:failed
echo.
echo The operation did not complete successfully.
pause
endlocal
exit /b 1

:finished
echo.
echo Exiting.
endlocal
exit /b 0
