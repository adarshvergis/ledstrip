<#
  Flash the firmware over the Xiao USB-C port (bootloader, partition table, otadata, app).
  Usage (via Scripts\firmware\flash.bat / flash.sh):  [-COMx] [-release | -debug] [-nobuild]
  Builds first (incremental) unless -nobuild. Settings (WiFi) and calibration partitions are not touched.
#>
[CmdletBinding(PositionalBinding = $false)]
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Words = @())
. "$PSScriptRoot\common.ps1"

$Variant = Get-Variant $Words
$Dirs = Get-VariantDirs $Variant

if (-not ($Words -match '^-{0,2}nobuild$')) {
    & "$PSScriptRoot\build.ps1" "-$Variant"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
foreach ($i in $FlashImages) {
    if (-not (Test-Path "$($Dirs.Artefacts)\$($i.File)")) { throw "$($i.File) missing in $($Dirs.Artefacts): run Scripts\firmware\build.bat -$Variant" }
}

Import-IdfEnv
$Port = Resolve-Port (Get-PortArg $Words)
$flashArgs = @("--chip", "esp32c3", "-p", $Port, "-b", "460800", "--before", "default_reset", "--after", "hard_reset", "--no-stub",
               "write_flash", "--flash_mode", "dio", "--flash_size", "4MB", "--flash_freq", "80m")
foreach ($i in $FlashImages) { $flashArgs += @($i.Offset, "$($Dirs.Artefacts)\$($i.File)") }
Write-Host "Flashing $Variant firmware to $Port"
Invoke-Checked { python -m esptool @flashArgs }
Write-Host "flash OK ($Variant)"
