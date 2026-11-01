@echo off
rem Native DSP / pipeline unit tests (gcc on PATH). Output: build\firmware\unit_tests.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0sub\unit_tests.ps1" %*
exit /b %ERRORLEVEL%
