@echo off
setlocal
chcp 65001 >nul
where python >nul 2>nul
if errorlevel 1 (
  py -3 "%~dp0launcher.py" %*
) else (
  python "%~dp0launcher.py" %*
)
if errorlevel 1 (
  echo.
  echo Launcher failed. Review the message above.
  pause
  exit /b 1
)
