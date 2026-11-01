@echo off
rem Start the host software (web UI on http://127.0.0.1:8080 by default).
rem Usage: Scripts\host_run.bat [--host 127.0.0.1] [--port 8080] [--device ^<ip[:port]^>]
setlocal
set "HOST=%~dp0..\Host"
if not exist "%HOST%\.venv\Scripts\python.exe" (
  echo Host\.venv missing - run Scripts\host_setup.bat first 1>&2
  exit /b 1
)
cd /d "%HOST%"
"%HOST%\.venv\Scripts\python.exe" -m ledhost %*
