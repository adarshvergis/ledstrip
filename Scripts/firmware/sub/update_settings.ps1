<#
  Write the WiFi credentials (settings partition at 0x3D8000) over the Xiao USB-C port.
  Usage (via Scripts\firmware\update_settings.bat / update_settings.sh):
      -ssid <name> -password <pw> [-name <device name>] [-COMx]
      -file <settings.json> [-COMx]
  Replaces ALL stored settings (others fall back to defaults); firmware and calibration are kept.
  Works for debug and release firmware.
#>
[CmdletBinding(PositionalBinding = $false)]
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Words = @())
. "$PSScriptRoot\common.ps1"

$opt = @{}
for ($n = 0; $n -lt $Words.Count; $n++) {
    if ($Words[$n] -match '^-{1,2}(ssid|password|name|file)$') {
        $key = $Matches[1]
        if ($n + 1 -ge $Words.Count) { throw "-$key needs a value" }
        $opt[$key] = $Words[++$n]
    }
}
if (-not $opt.ContainsKey("ssid") -and -not $opt.ContainsKey("file")) {
    throw "usage: update_settings -ssid <name> -password <pw> [-name <device>] [-COMx]   or   -file <settings.json> [-COMx]"
}

$outDir = Join-Path $BuildRoot "settings_image"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$img = Join-Path $outDir "settings.bin"
$py = @("$PSScriptRoot\make_settings_image.py")
if ($opt.ContainsKey("file")) { $py += $opt["file"] }
if ($opt.ContainsKey("ssid")) { $py += @("--ssid", $opt["ssid"]) }
if ($opt.ContainsKey("password")) { $py += @("--password", $opt["password"]) }
if ($opt.ContainsKey("name")) { $py += @("--name", $opt["name"]) }
$py += @("-o", $img)

Import-IdfEnv
Invoke-Checked { python @py }
$Port = Resolve-Port (Get-PortArg $Words)
Invoke-Checked { python -m esptool --chip esp32c3 -p $Port write_flash $SettingsAddr $img }
Write-Host "settings written to $Port. The device restarts and joins the WiFi."
