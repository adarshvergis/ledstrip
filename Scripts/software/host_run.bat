@echo off
rem Start the desktop host app. Usage: host_run.bat [-device <ip>] [-debug]
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0sub\host_run.ps1" %*
exit /b %ERRORLEVEL%
