@echo off
rem Release firmware (main view, calibration, firmware update only) -> build_release\ and dist\release\.
rem Usage: build_release.bat [build|flash|flash-monitor] [-Port COM5]
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %* -Release
exit /b %ERRORLEVEL%
