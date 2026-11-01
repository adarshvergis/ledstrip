@echo off
rem Host protocol tests against the device simulator.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0sub\unit_tests.ps1" %*
exit /b %ERRORLEVEL%
