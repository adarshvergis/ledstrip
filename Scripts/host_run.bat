@echo off
rem Start the desktop host app (Python 3.9+ with tkinter, no other packages).
rem Usage: Scripts\host_run.bat [--device ^<ip^>] [--debug]
setlocal
set "PY=python"
where py >nul 2>nul && set "PY=py -3"
%PY% "%~dp0..\Host\ledapp.py" %*
