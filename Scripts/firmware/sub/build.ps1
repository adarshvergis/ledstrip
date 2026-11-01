<#
  Build the firmware with ESP-IDF.
  Usage (via Scripts\firmware\build.bat / build.sh):  -release | -debug  [-clean] [-size] [-menuconfig]
  Output: build\firmware\<variant>\build_files\ (CMake / Ninja, sdkconfig) and
          build\firmware\<variant>\artefacts\   (.bin .hex .elf .map)
#>
[CmdletBinding(PositionalBinding = $false)]
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Words = @())
. "$PSScriptRoot\common.ps1"

$Variant = Get-Variant $Words
$Dirs = Get-VariantDirs $Variant
$Clean = [bool]($Words -match '^-{0,2}clean$')
$Size = [bool]($Words -match '^-{0,2}size$')
$Menu = [bool]($Words -match '^-{0,2}menuconfig$')

if ($Clean) {
    if (Test-Path $Dirs.Base) { Remove-Item -Recurse -Force $Dirs.Base }
    Write-Host "removed $($Dirs.Base)"
    exit 0
}

Import-IdfEnv
Set-Location $Root
New-Item -ItemType Directory -Force -Path $Dirs.BuildFiles, $Dirs.Artefacts | Out-Null

$idf = @("-B", $Dirs.BuildFiles, "-DSDKCONFIG=$($Dirs.BuildFiles)\sdkconfig")
if ($Variant -eq "release") { $idf += "-DLEDFW_RELEASE=1" }

if ($Menu) { Invoke-Checked { idf.py @idf menuconfig }; exit 0 }
Invoke-Checked { idf.py @idf build }
if ($Size) { Invoke-Checked { idf.py @idf size } }

# ---- artefacts ----------------------------------------------------------------
$B = $Dirs.BuildFiles
$A = $Dirs.Artefacts
Copy-Item "$B\led_strip_fw.bin", "$B\led_strip_fw.elf", "$B\led_strip_fw.map" $A -Force
Copy-Item "$B\bootloader\bootloader.bin" $A -Force
Copy-Item "$B\partition_table\partition-table.bin" $A -Force
Copy-Item "$B\ota_data_initial.bin" $A -Force

# Merged first-flash images (bootloader + partition table + otadata + app at their offsets).
$merge = @("--chip", "esp32c3", "merge_bin", "--flash_mode", "dio", "--flash_size", "4MB", "--flash_freq", "80m")
foreach ($i in $FlashImages) { $merge += @($i.Offset, "$A\$($i.File)") }
Invoke-Checked { python -m esptool @merge --format raw -o "$A\led_strip_fw_full.bin" }
Invoke-Checked { python -m esptool @merge --format hex -o "$A\led_strip_fw_full.hex" }

$app = Get-Item "$A\led_strip_fw.bin"
Write-Host ""
Write-Host "$Variant build OK: led_strip_fw.bin $($app.Length) bytes"
Write-Host "artefacts: $A"
Get-ChildItem $A | ForEach-Object { Write-Host ("  {0,-26} {1,9} bytes" -f $_.Name, $_.Length) }
