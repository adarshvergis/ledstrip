<#
  Host software tests: protocol end-to-end against the device simulator (debug and release flags).
  Usage (via Scripts\software\run_software_unit_tests.bat / .sh)
  Needs no hardware. Byte code goes to build\software\build_files\pycache.
#>
. "$PSScriptRoot\common.ps1"

$py = Find-Python
$exe = $py[0]; $pre = @($py | Select-Object -Skip 1)
Use-PycacheOutsideSource
Set-Location $Root
& $exe @pre software/tools/proto_test.py
exit $LASTEXITCODE
