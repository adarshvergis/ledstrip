@echo off
rem Serial console (Ctrl+] exits). Usage: monitor.bat [-COM11] [-release|-debug]
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0sub\monitor.ps1" %*
exit /b %ERRORLEVEL%
