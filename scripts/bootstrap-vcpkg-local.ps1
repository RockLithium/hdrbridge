param(
    [string]$Ref = "ddd110b8a05cda14b8f1b0333a1d80c4fb6f16cd"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$Tools = Join-Path $RepoRoot ".tools"
$Vcpkg = Join-Path $Tools "vcpkg"

New-Item -ItemType Directory -Force -Path $Tools | Out-Null

if (-not (Test-Path $Vcpkg)) {
    git clone https://github.com/microsoft/vcpkg.git $Vcpkg
}

Push-Location $Vcpkg
try {
    git fetch --tags --prune
    git checkout $Ref
    .\bootstrap-vcpkg.bat -disableMetrics
} finally {
    Pop-Location
}

Write-Host "Local vcpkg ready at $Vcpkg"
