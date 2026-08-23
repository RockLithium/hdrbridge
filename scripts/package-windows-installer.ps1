param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [string]$SourceDirectory = (Join-Path $PSScriptRoot "..\dist\HDRBridge-v$Version-Windows-x64"),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "..\dist"),
    [string]$IsccPath = "",
    [string]$OutputBaseName = "HDRBridge-v$Version-Windows-x64-Setup"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
$dist = [System.IO.Path]::GetFullPath((Join-Path $repo "dist"))
if ($output -ne $dist) {
    throw "Installer output directory must be $dist"
}
if (-not (Test-Path -LiteralPath (Join-Path $source "hdrbridge.exe"))) {
    throw "Prepared portable directory does not contain hdrbridge.exe: $source"
}

if (-not $IsccPath) {
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
        (Join-Path $env:ProgramFiles "Inno Setup 7\ISCC.exe"),
        (Join-Path $repo ".tools\innosetup-6.7.3\ISCC.exe")
    )
    $IsccPath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $IsccPath -or -not (Test-Path -LiteralPath $IsccPath)) {
    throw "Inno Setup command-line compiler was not found"
}

$script = Join-Path $repo "installer\hdrbridge.iss"
$agreement = Join-Path $output "HDRBridge-v$Version-Installer-Agreement.txt"
$paragraphs = [System.Collections.Generic.List[string]]::new()
$current = [System.Collections.Generic.List[string]]::new()
foreach ($line in Get-Content -LiteralPath (Join-Path $repo "LICENSE")) {
    $trimmed = $line.Trim()
    if (-not $trimmed) {
        if ($current.Count) {
            $paragraphs.Add(($current -join " "))
            $current.Clear()
        }
    } else {
        $current.Add($trimmed)
    }
}
if ($current.Count) { $paragraphs.Add(($current -join " ")) }
[System.IO.File]::WriteAllText(
    $agreement,
    (($paragraphs -join [Environment]::NewLine) + [Environment]::NewLine),
    [System.Text.UTF8Encoding]::new($false))

try {
    & $IsccPath `
        "/DAppVersion=$Version" `
        "/DSourceDir=$source" `
        "/DOutputDir=$output" `
        "/DRepoRoot=$repo" `
        "/DLicenseFile=$agreement" `
        "/DSetupBaseName=$OutputBaseName" `
        $script
    if ($LASTEXITCODE -ne 0) {
        throw "Inno Setup compilation failed with exit code $LASTEXITCODE"
    }
} finally {
    if (Test-Path -LiteralPath $agreement) {
        [System.IO.File]::Delete($agreement)
    }
}

$installer = Join-Path $output "$OutputBaseName.exe"
if (-not (Test-Path -LiteralPath $installer)) {
    throw "Expected installer was not produced: $installer"
}
Write-Host "Installer: $installer"
Get-FileHash -LiteralPath $installer -Algorithm SHA256
