<#
  Serial console of the board (exit with Ctrl+]).
  Usage (via Scripts\firmware\monitor.bat / monitor.sh):  [-COMx] [-release | -debug]
#>
[CmdletBinding(PositionalBinding = $false)]
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Words = @())
. "$PSScriptRoot\common.ps1"

$Dirs = Get-VariantDirs (Get-Variant $Words)
if (-not (Test-Path "$($Dirs.BuildFiles)\led_strip_fw.elf")) { throw "no build in $($Dirs.BuildFiles): run Scripts\firmware\build.bat first" }
Import-IdfEnv
Set-Location $FwDir
$Port = Resolve-Port (Get-PortArg $Words)
Invoke-Checked { idf.py -B $Dirs.BuildFiles "-DSDKCONFIG=$($Dirs.BuildFiles)\sdkconfig" -p $Port monitor }
