@echo off
rem Build the firmware (ESP-IDF). Usage: build.bat [-release|-debug] [-clean] [-size] [-menuconfig]
rem Output: build\firmware\<variant>\artefacts (.bin .hex .elf) and build_files.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0sub\build.ps1" %*
exit /b %ERRORLEVEL%
