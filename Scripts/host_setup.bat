@echo off
rem Create the host software virtual environment (Host\.venv) and install dependencies.
setlocal
set "HOST=%~dp0..\Host"
set "PY=python"
where py >nul 2>nul && set "PY=py -3"
echo Creating virtual environment in %HOST%\.venv ...
%PY% -m venv "%HOST%\.venv" || goto :fail
"%HOST%\.venv\Scripts\python.exe" -m pip install --upgrade pip >nul
"%HOST%\.venv\Scripts\python.exe" -m pip install -r "%HOST%\requirements.txt" || goto :fail
echo Host environment ready. Start it with: Scripts\host_run.bat [--device ^<ip^>]
exit /b 0
:fail
echo Setup failed. Is Python 3.9+ installed and on PATH? 1>&2
exit /b 1
