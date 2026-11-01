<#
.SYNOPSIS
  Build / flash / monitor the LED strip firmware with ESP-IDF v5.3.

.EXAMPLE
  .\Scripts\build.ps1                       # debug (INTERNAL_TEST_BUILD) build in build/
  .\Scripts\build.ps1 -Release              # release build (main view, calibration, FW update only) in build_release/
  .\Scripts\build.ps1 flash -Port COM5      # build + flash over the Xiao USB-C port
  .\Scripts\build.ps1 flash-monitor -Port COM5
  .\Scripts\build.ps1 monitor -Port COM5
  .\Scripts\build.ps1 flash-settings -Port COM5 -File settings.bin   # WiFi credentials (Scripts/flash_settings.sh)
  .\Scripts\build.ps1 size | clean | fullclean | menuconfig

  IDF_PATH defaults to %USERPROFILE%\esp\v5.3.1\esp-idf.
  A successful build copies its images to dist\debug\ or dist\release\:
  led_strip_fw.bin (OTA / host "Firmware" tab file), bootloader.bin, partition-table.bin,
  ota_data_initial.bin (the last three only for a first flash over USB).
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    # Free-form words, same style as build.sh: an action, "--release" and/or a port (COM5)
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Words = @(),
    [string]$Port = "",
    [string]$File = "",
    [switch]$Release
)

$ErrorActionPreference = "Stop"
$Actions = @("build", "flash", "monitor", "flash-monitor", "flash-settings", "size", "clean", "fullclean", "menuconfig")
$Action = "build"
foreach ($w in $Words) {
    if ($w -in @("--release", "-release", "release")) { $Release = $true }
    elseif ($w -match '^(COM\d+|/dev/.+)$') { $Port = $w }
    elseif ($w -in $Actions) { $Action = $w }
    else { throw "unknown argument '$w' (actions: $($Actions -join ', '); --release; COMx)" }
}
$Root = Split-Path -Parent $PSScriptRoot
$SettingsAddr = "0x3D8000"      # settings partition, keep in sync with partitions.csv / build_config.h

if (-not $env:IDF_PATH) { $env:IDF_PATH = Join-Path $env:USERPROFILE "esp\v5.3.1\esp-idf" }
if (-not (Test-Path "$env:IDF_PATH\export.ps1")) { throw "ESP-IDF not found at $env:IDF_PATH (set IDF_PATH)" }

# ESP-IDF refuses to run when it detects an MSYS / Git-Bash environment.
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
. "$env:IDF_PATH\export.ps1" *> $null

Set-Location $Root

if ($Action -eq "flash-settings") {
    if (-not $Port) { throw "flash-settings needs -Port COMx" }
    if (-not $File -or -not (Test-Path $File)) { throw "flash-settings needs -File <settings image>" }
    python -m esptool --chip esp32c3 -p $Port write_flash $SettingsAddr $File
    exit $LASTEXITCODE
}

$BuildDir = "build"
$Variant = "debug"
$idfArgs = @()
if ($Release) {
    $BuildDir = "build_release"
    $Variant = "release"
    $idfArgs += @("-B", $BuildDir, "-DLEDFW_RELEASE=1")
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
$rc = $LASTEXITCODE

if ($rc -eq 0 -and $Action -in @("build", "flash", "flash-monitor")) {
    $Dist = Join-Path $Root "dist\$Variant"
    New-Item -ItemType Directory -Force -Path $Dist | Out-Null
    Copy-Item (Join-Path $BuildDir "led_strip_fw.bin") $Dist -Force
    Copy-Item (Join-Path $BuildDir "bootloader\bootloader.bin") $Dist -Force
    Copy-Item (Join-Path $BuildDir "partition_table\partition-table.bin") $Dist -Force
    Copy-Item (Join-Path $BuildDir "ota_data_initial.bin") $Dist -Force
    Write-Host "$Variant images copied to dist\$Variant\"
}
exit $rc
