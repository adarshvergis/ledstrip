@echo off
rem Check the host software (byte-compile, no packaging needed). Output: build\software\build_files.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0sub\build.ps1" %*
exit /b %ERRORLEVEL%
