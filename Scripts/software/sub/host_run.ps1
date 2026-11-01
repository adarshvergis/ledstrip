<#
  Start the desktop host app (Python 3.9+ with tkinter, no other packages).
  Usage (via Scripts\software\host_run.bat / host_run.sh):  [-device <ip>] [-debug]
#>
[CmdletBinding(PositionalBinding = $false)]
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Words = @())
. "$PSScriptRoot\common.ps1"

$py = Find-Python
$exe = $py[0]; $pre = @($py | Select-Object -Skip 1)
Use-PycacheOutsideSource
# -device / -debug -> --device / --debug (what the app's argparse expects)
$appArgs = @($Words | ForEach-Object { if ($_ -match '^-[A-Za-z]{2,}$') { "-$_" } else { $_ } })
& $exe @pre "$Root\software\ledapp.py" @appArgs
exit $LASTEXITCODE
