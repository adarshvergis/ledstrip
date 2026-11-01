<#
.SYNOPSIS
  Build / flash / monitor the LED strip firmware with ESP-IDF v5.3.

.EXAMPLE
  .\Scripts\build.ps1                       # debug (INTERNAL_TEST_BUILD) build
  .\Scripts\build.ps1 -Release              # release build (no CLI / diagnostics) in build_release/
  .\Scripts\build.ps1 flash -Port COM5      # build + flash over the Xiao USB-C port
  .\Scripts\build.ps1 flash-monitor -Port COM5
  .\Scripts\build.ps1 monitor -Port COM5
  .\Scripts\build.ps1 size | clean | fullclean | menuconfig

  IDF_PATH defaults to %USERPROFILE%\esp\v5.3.1\esp-idf.
  Output images: build\led_strip_fw.bin (OTA / host "Firmware Update" file),
  build\bootloader\bootloader.bin, build\partition_table\partition-table.bin.
#>
param(
    [Parameter(Position = 0)]
    [ValidateSet("build", "flash", "monitor", "flash-monitor", "size", "clean", "fullclean", "menuconfig")]
    [string]$Action = "build",
    [string]$Port = "",
    [switch]$Release
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

if (-not $env:IDF_PATH) { $env:IDF_PATH = Join-Path $env:USERPROFILE "esp\v5.3.1\esp-idf" }
if (-not (Test-Path "$env:IDF_PATH\export.ps1")) { throw "ESP-IDF not found at $env:IDF_PATH (set IDF_PATH)" }

# ESP-IDF refuses to run when it detects an MSYS / Git-Bash environment.
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
. "$env:IDF_PATH\export.ps1" *> $null

Set-Location $Root
$idfArgs = @()
if ($Release) {
    $idfArgs += @("-B", "build_release", "-DLEDFW_RELEASE=1")
}
if ($Port) { $idfArgs += @("-p", $Port) }

if (-not (Test-Path (Join-Path $Root "sdkconfig"))) {
    idf.py set-target esp32c3
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

switch ($Action) {
    "build"         { idf.py @idfArgs build }
    "flash"         { idf.py @idfArgs build flash }
    "monitor"       { idf.py @idfArgs monitor }
    "flash-monitor" { idf.py @idfArgs build flash monitor }
    "size"          { idf.py @idfArgs size }
    "clean"         { idf.py @idfArgs clean }
    "fullclean"     { idf.py @idfArgs fullclean }
    "menuconfig"    { idf.py @idfArgs menuconfig }
}
exit $LASTEXITCODE
