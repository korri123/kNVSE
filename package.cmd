@echo off
rem Double-click to run scripts\package.ps1 (arguments are passed on: package.cmd -Build -Configuration Debug)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\package.ps1" %*
set code=%errorlevel%
rem Started from Explorer (cmd /c with this file): keep the window open to show the result
echo(%cmdcmdline% | findstr /i /c:"%~nx0" >nul && pause
exit /b %code%
