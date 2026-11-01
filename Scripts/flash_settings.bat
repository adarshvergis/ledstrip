@echo off
rem Write the WiFi credentials (settings partition) over the Xiao USB-C port.
rem Usage: flash_settings.bat COM5 --ssid HomeWiFi --password secret [--name ledstrip]
rem        flash_settings.bat COM5 settings.json
setlocal
set "HERE=%~dp0"
if "%~2"=="" (
  echo Usage: flash_settings.bat COM5 --ssid HomeWiFi --password secret [--name ledstrip] 1>&2
  exit /b 1
)
set "PORT=%~1"
shift
set "ARGS="
:collect
if "%~1"=="" goto run
set "ARGS=%ARGS% %1"
shift
goto collect
:run
set "PY=python"
where py >nul 2>nul && set "PY=py -3"
if not exist "%HERE%..\build" mkdir "%HERE%..\build"
%PY% "%HERE%make_settings_image.py" %ARGS% -o "%HERE%..\build\settings.bin" || exit /b 1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%HERE%build.ps1" flash-settings -Port %PORT% -File "%HERE%..\build\settings.bin"
exit /b %ERRORLEVEL%
