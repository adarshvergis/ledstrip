@echo off
rem Firmware build wrapper for CMD / PowerShell.
rem Usage: build.bat [--release] [build|flash|monitor|flash-monitor|size|clean] [COM5]   (or -Port COM5 -Release)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
