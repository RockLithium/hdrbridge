param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "..\build-vs\bin\Release"),
    [string]$Version = "1.2.1",
    [string]$Destination = (Join-Path $PSScriptRoot "..\dist\HDRBridge-v$Version-Windows-x64")
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$distRoot = Join-Path $repo "dist"
$resolvedParent = [System.IO.Path]::GetFullPath((Split-Path -Parent $Destination))
if ($resolvedParent -ne [System.IO.Path]::GetFullPath($distRoot)) {
    throw "Package destination must be a direct child of $distRoot"
}
if (Test-Path -LiteralPath $Destination) {
    [System.IO.Directory]::Delete([System.IO.Path]::GetFullPath($Destination), $true)
}
[System.IO.Directory]::CreateDirectory($Destination) | Out-Null
[System.IO.Directory]::CreateDirectory((Join-Path $Destination "docs")) | Out-Null
[System.IO.Directory]::CreateDirectory((Join-Path $Destination "licenses")) | Out-Null

Copy-Item -LiteralPath (Join-Path $BuildDirectory "hdrbridge.exe") -Destination $Destination
Get-ChildItem -LiteralPath $BuildDirectory -Filter "*.dll" | Copy-Item -Destination $Destination

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw "vswhere.exe not found" }
$vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw "Visual Studio C++ workload not found" }
$crtRoot = Join-Path $vsInstall "VC\Redist\MSVC"
$crt = Get-ChildItem -LiteralPath $crtRoot -Directory | Sort-Object Name -Descending |
    ForEach-Object { Get-ChildItem -LiteralPath (Join-Path $_.FullName "x64") -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue } |
    Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $crt) { throw "Visual C++ x64 runtime payload not found" }
Get-ChildItem -LiteralPath $crt -Filter "*.dll" | Copy-Item -Destination $Destination

Copy-Item -LiteralPath (Join-Path $repo "LICENSE") -Destination (Join-Path $Destination "LICENSE.txt")
Copy-Item -LiteralPath (Join-Path $repo "THIRD_PARTY_NOTICES.md") -Destination $Destination
foreach ($document in "COMPATIBILITY_MATRIX.md", "METADATA.md", "CONVERSION_FIDELITY.md") {
    Copy-Item -LiteralPath (Join-Path $repo "docs\$document") -Destination (Join-Path $Destination "docs\$document")
}

foreach ($package in "libheif", "libavif", "libyuv", "libde265", "libjxl", "libjpeg-turbo", "brotli", "highway", "lcms", "libpng", "tiff", "zlib", "liblzma", "aom", "nlohmann-json") {
    $copyright = Join-Path $repo "vcpkg_installed\x64-windows\share\$package\copyright"
    if (Test-Path -LiteralPath $copyright) {
        Copy-Item -LiteralPath $copyright -Destination (Join-Path $Destination "licenses\$package.txt")
    }
}
$jsonFallback = Join-Path $repo "third_party\licenses\nlohmann-json.txt"
if (-not (Test-Path -LiteralPath (Join-Path $Destination "licenses\nlohmann-json.txt"))) {
    Copy-Item -LiteralPath $jsonFallback -Destination (Join-Path $Destination "licenses\nlohmann-json.txt")
}
$uhdrRoot = @(
    (Join-Path $repo ".tools\libultrahdr"),
    (Join-Path $repo "build-vs\_deps\libultrahdr-src")
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($uhdrRoot) {
    Get-ChildItem -LiteralPath $uhdrRoot -Filter "LICENSE*" | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $Destination ("licenses\libultrahdr-" + $_.Name + ".txt"))
    }
}

@"
HDR Bridge v$Version - portable Windows x64

Run hdrbridge.exe. Add or drop one or more supported HDR images, choose an
output, add tasks to the queue, then select Start all. Processing is local and
sequential. Output defaults
to the source directory; a custom directory, base name, suffix, auto-numbering
and explicit overwrite are available.

The Source Inspector reports encoded source, metadata and gain-map details.
Preview uses a verified Windows FP16 scRGB HDR swap-chain presentation path.
It is off by default and performs no preview decode until opened.

Primary outputs:
- Ultra HDR JPEG (Faithful/Auto)
- RGB16 PQ/HLG PNG
- RGB16 PQ/HLG JPEG XL
- RGB gain-map JPEG XL
- FP16 linear scRGB JPEG XR
- 10-bit 4:4:4 PQ/HLG AVIF
- RGB gain-map AVIF
- RGB16 PQ TIFF
- Embedded Gain Map extraction (original, PNG, TIFF or JPEG)

The application is unsigned. Windows may show a SmartScreen or unknown-
publisher warning. See docs/COMPATIBILITY_MATRIX.md for measured application
behavior and docs/CONVERSION_FIDELITY.md for fidelity/lossless terminology.
"@ | Set-Content -Encoding UTF8 (Join-Path $Destination "README.txt")

$zip = "$Destination.zip"
if (Test-Path -LiteralPath $zip) { [System.IO.File]::Delete($zip) }
Compress-Archive -LiteralPath $Destination -DestinationPath $zip -CompressionLevel Optimal
Write-Host "Portable directory: $Destination"
Write-Host "Archive: $zip"
