@echo off
rem Build (incremental) and flash over USB-C. Usage: flash.bat [-COM11] [-release|-debug] [-nobuild]
rem Without -COMx the only connected serial port is used.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0sub\flash.ps1" %*
exit /b %ERRORLEVEL%
