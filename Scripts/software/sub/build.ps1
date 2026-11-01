<#
  "Build" of the host software. The host is plain Python (standard library only), so there is
  nothing to compile or package: this byte-compiles the host sources and tools to catch syntax errors.
  Usage (via Scripts\software\build.bat / build.sh):  [-clean]
  Output: build\software\build_files\pycache (no artefacts)
#>
[CmdletBinding(PositionalBinding = $false)]
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Words = @())
. "$PSScriptRoot\common.ps1"

if ($Words -match '^-{0,2}clean$') {
    if (Test-Path $BuildRoot) { Remove-Item -Recurse -Force $BuildRoot }
    Write-Host "removed $BuildRoot"
    exit 0
}
$py = Find-Python
$exe = $py[0]; $pre = @($py | Select-Object -Skip 1)
Use-PycacheOutsideSource
Set-Location $Root
& $exe @pre -m compileall -q -f Host Scripts/firmware
if ($LASTEXITCODE -ne 0) { Write-Host "software build FAILED"; exit $LASTEXITCODE }
Write-Host "software build OK (sources byte-compiled, nothing to package)"
