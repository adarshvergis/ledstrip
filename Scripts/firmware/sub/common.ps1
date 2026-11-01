# Shared helpers for the firmware scripts (dot-sourced, not run directly).

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))   # repo root
$BuildRoot = Join-Path $Root "build\firmware"
$FwDir = Join-Path $Root "firmware"                  # ESP-IDF project (CMakeLists.txt, App, Interface)

# Flash layout (keep in sync with partitions.csv and firmware/App/build_config.h).
$FlashImages = @(
    @{ Offset = "0x0";     File = "bootloader.bin" },
    @{ Offset = "0x8000";  File = "partition-table.bin" },
    @{ Offset = "0xd000";  File = "ota_data_initial.bin" },
    @{ Offset = "0x10000"; File = "led_strip_fw.bin" }
)
$SettingsAddr = "0x3D8000"

function Get-Variant([string[]]$Words) {
    # -release / -debug (also --release, release). Default: debug.
    $v = "debug"
    foreach ($w in $Words) {
        if ($w -match '^-{0,2}release$') { $v = "release" }
        elseif ($w -match '^-{0,2}debug$') { $v = "debug" }
    }
    return $v
}

function Get-VariantDirs([string]$Variant) {
    $base = Join-Path $BuildRoot $Variant
    return @{ Base = $base; Artefacts = (Join-Path $base "artefacts"); BuildFiles = (Join-Path $base "build_files") }
}

function Get-PortArg([string[]]$Words) {
    foreach ($w in $Words) { if ($w -match '^-{0,2}(COM\d+|/dev/\S+)$') { return $Matches[1] } }
    return ""
}

function Import-IdfEnv {
    if (-not $env:IDF_PATH) { $env:IDF_PATH = Join-Path $env:USERPROFILE "esp\v5.3.1\esp-idf" }
    if (-not (Test-Path "$env:IDF_PATH\export.ps1")) { throw "ESP-IDF not found at $env:IDF_PATH (set IDF_PATH, see Docs/setup.md)" }
    # ESP-IDF refuses to run when it detects an MSYS / Git-Bash environment.
    Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
    $env:IDF_TARGET = "esp32c3"
    . "$env:IDF_PATH\export.ps1" *> $null
}

function Resolve-Port([string]$Port) {
    # Explicit port wins; otherwise use the only non-Bluetooth serial port present.
    if ($Port) { return $Port }
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"      # list_ports prints its summary on stderr
    $lines = @(python -m serial.tools.list_ports -v 2>&1 | ForEach-Object { "$_" })
    $ErrorActionPreference = $old
    $ports = @()
    for ($n = 0; $n -lt $lines.Count; $n++) {
        if ($lines[$n] -match '^(COM\d+|/dev/\S+)\s*$') {
            $desc = if ($n + 1 -lt $lines.Count) { $lines[$n + 1] } else { "" }
            if ($desc -notmatch 'Bluetooth') { $ports += $Matches[1] }
        }
    }
    if ($ports.Count -eq 1) { Write-Host "Using serial port $($ports[0])"; return $ports[0] }
    if ($ports.Count -eq 0) { throw "no serial port found (Bluetooth ports ignored): connect the board or pass -COMx" }
    throw "several serial ports found ($($ports -join ', ')): pass -COMx"
}

function Invoke-Checked([scriptblock]$Cmd) {
    & $Cmd
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
