@echo off
rem Write WiFi credentials over USB-C. Usage: update_settings.bat -ssid <name> -password <pw> [-name <device>] [-COM11]
rem    or: update_settings.bat -file settings.json [-COM11]    (replaces all stored settings)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0sub\update_settings.ps1" %*
exit /b %ERRORLEVEL%
