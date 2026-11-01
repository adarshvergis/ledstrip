# Shared helpers for the software (host app) scripts (dot-sourced, not run directly).

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))   # repo root
$BuildRoot = Join-Path $Root "build\software"
$BuildFiles = Join-Path $BuildRoot "build_files"
$Artefacts = Join-Path $BuildRoot "artefacts"

function Find-Python {
    # Python 3.9+ with tkinter (the host app needs it; tests and checks do not).
    foreach ($cand in @(@("py", "-3"), @("python"), @("python3"))) {
        if (-not (Get-Command $cand[0] -ErrorAction SilentlyContinue)) { continue }
        $extra = @($cand | Select-Object -Skip 1)
        & $cand[0] @extra -c "import sys, tkinter; sys.exit(sys.version_info < (3, 9))" 2>$null
        if ($LASTEXITCODE -eq 0) { return , $cand }
    }
    throw "Python 3.9+ with tkinter not found (see Docs/setup.md)"
}

function Use-PycacheOutsideSource {
    # Keep __pycache__ out of the source tree: byte code goes to build\software\build_files\pycache.
    New-Item -ItemType Directory -Force -Path (Join-Path $BuildFiles "pycache") | Out-Null
    $env:PYTHONPYCACHEPREFIX = Join-Path $BuildFiles "pycache"
}
