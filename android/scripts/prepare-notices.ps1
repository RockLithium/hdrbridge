param(
  [string]$Installed = (Join-Path $PSScriptRoot '..\vcpkg_installed\arm64-android-hdr'),
  [string]$UltraHdr = (Join-Path $PSScriptRoot '..\..\.tools\libultrahdr')
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$dest = Join-Path $repo 'android\app\src\main\assets\licenses'
$assetRoot = Join-Path $repo 'android\app\src\main\assets'
$frameLogoSource = Join-Path $repo 'assets\frame-logos'
$ports = @('aom','brotli','expat','highway','jxrlib','lcms','libavif','libde265','libheif','libjpeg-turbo','libjxl','liblzma','libpng','libyuv','nlohmann-json','tiff','zlib')
$files = @{}
$files['HDRBridge-LICENSE.txt'] = Join-Path $repo 'LICENSE'
$files['stb.txt'] = Join-Path $repo 'third_party\stb\LICENSE'
$files['nanosvg.txt'] = Join-Path $repo 'third_party\nanosvg\LICENSE.txt'
foreach ($port in $ports) { $files["$port.txt"] = Join-Path $Installed "share\$port\copyright" }
foreach ($name in @('LICENSE','LICENSE-APACHE','LICENSE-MIT')) { $files["libultrahdr-$name.txt"] = Join-Path $UltraHdr $name }
foreach ($file in $files.Values) { if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing license text: $file" } }
New-Item -ItemType Directory -Force -Path $dest | Out-Null
foreach ($entry in $files.GetEnumerator()) { Copy-Item -LiteralPath $entry.Value -Destination (Join-Path $dest $entry.Key) -Force }
$logoDest = Join-Path $assetRoot 'frame-logos'
if (-not (Test-Path -LiteralPath $frameLogoSource -PathType Container)) { throw "Missing frame logo assets: $frameLogoSource" }
if (-not (Test-Path -LiteralPath (Join-Path $frameLogoSource 'SOURCE.md') -PathType Leaf)) {
  throw 'Missing frame logo notice/source file: SOURCE.md'
}
$logos = @(Get-ChildItem -LiteralPath $frameLogoSource -File)
if (-not ($logos | Where-Object { $_.Extension -eq '.svg' })) { throw 'No SVG frame logos found.' }
if (@($logos | Where-Object { $_.Extension -notin @('.svg', '.md', '.txt') }).Count) { throw 'Unexpected frame logo asset type.' }
New-Item -ItemType Directory -Force -Path $logoDest | Out-Null
Get-ChildItem -LiteralPath $logoDest -File | Remove-Item -Force
Copy-Item -Path (Join-Path $frameLogoSource '*') -Destination $logoDest -Force
Write-Host "Copied $($files.Count) license texts and $($logos.Count) frame logo assets."
