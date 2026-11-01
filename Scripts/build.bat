@echo off
rem Firmware build wrapper for CMD. Usage: build.bat [build|flash|monitor|flash-monitor|size|clean] [-Port COM5] [-Release]
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
