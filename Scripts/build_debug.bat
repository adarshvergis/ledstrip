@echo off
rem Debug firmware (CLI, self diagnostics, settings/WiFi from the host) -> build\ and dist\debug\.
rem Usage: build_debug.bat [build|flash|flash-monitor] [-Port COM5]
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
