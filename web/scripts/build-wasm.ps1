param(
  [string]$EmsdkRoot = "D:\emsdk",
  [string]$VcpkgRoot = "",
  [string]$NinjaPath = "D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
)

$ErrorActionPreference = "Stop"
$webRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $webRoot
$cacheRoot = Join-Path $webRoot ".cache"
$buildRoot = Join-Path $cacheRoot "build\hdrbridge-full"
$installRoot = Join-Path $cacheRoot "vcpkg_installed"
$prefix = Join-Path $installRoot "wasm32-emscripten"
$outputRoot = Join-Path $webRoot "public\codecs\hdrbridge"
$licenseRoot = Join-Path $webRoot "public\licenses"
$emcmake = Join-Path $EmsdkRoot "upstream\emscripten\emcmake.bat"

if (-not $VcpkgRoot) { $VcpkgRoot = Join-Path $repoRoot ".tools\vcpkg" }
$vcpkg = Join-Path $VcpkgRoot "vcpkg.exe"
if (-not (Test-Path -LiteralPath $emcmake)) { throw "Emscripten was not found at $emcmake" }
if (-not (Test-Path -LiteralPath $vcpkg)) { throw "vcpkg was not found at $vcpkg" }
if (-not (Test-Path -LiteralPath $NinjaPath)) { throw "Ninja was not found at $NinjaPath" }

New-Item -ItemType Directory -Force -Path $cacheRoot, $outputRoot, $licenseRoot | Out-Null
$env:EMSDK = $EmsdkRoot
$env:EMSCRIPTEN_ROOT = Join-Path $EmsdkRoot "upstream\emscripten"

foreach ($variant in @('scalar', 'threaded')) {
$triplet = if ($variant -eq 'threaded') { 'wasm32-emscripten-pthreads' } else { 'wasm32-emscripten' }
$threaded = if ($variant -eq 'threaded') { 'ON' } else { 'OFF' }
$prefix = Join-Path $installRoot $triplet
$buildRoot = Join-Path $cacheRoot "build\hdrbridge-$variant"
& $vcpkg install `
  "libheif[core,aom]:$triplet" "libavif[core,aom]:$triplet" `
  "libpng:$triplet" "tiff[core,jpeg,zip]:$triplet" `
  "lcms:$triplet" "liblzma:$triplet" "libjxl:$triplet" `
  "jxrlib:$triplet" "nlohmann-json:$triplet" `
  --classic "--x-install-root=$installRoot" `
  "--overlay-ports=$(Join-Path $webRoot 'vcpkg-overlay')" "--overlay-triplets=$(Join-Path $webRoot 'vcpkg-triplets')"
if ($LASTEXITCODE -ne 0) { throw "WASM codec dependency build failed" }

& $emcmake cmake -S (Join-Path $webRoot "wasm") -B $buildRoot -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$NinjaPath" `
  "-DCMAKE_BUILD_TYPE=Release" `
  "-DHDRBRIDGE_PTHREADS=$threaded" `
  "-DCMAKE_PREFIX_PATH=$prefix" `
  "-DCMAKE_FIND_ROOT_PATH=$prefix" `
  "-Dlibheif_DIR=$(Join-Path $prefix 'share\libheif')" `
  "-Dlibavif_DIR=$(Join-Path $prefix 'share\libavif')" `
  "-Dnlohmann_json_DIR=$(Join-Path $prefix 'share\nlohmann_json')" `
  "-Dlcms2_DIR=$(Join-Path $prefix 'share\lcms2')" `
  "-DHDRBRIDGE_VCPKG_PREFIX=$prefix"
if ($LASTEXITCODE -ne 0) { throw "HDR Bridge WASM configure failed" }
cmake --build $buildRoot --parallel
if ($LASTEXITCODE -ne 0) { throw "HDR Bridge WASM build failed" }

$dest = if ($variant -eq 'threaded') { Join-Path $outputRoot 'threaded' } else { $outputRoot }
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item -Force (Join-Path $buildRoot 'dist\*') $dest
}
Copy-Item -Force (Join-Path $repoRoot "THIRD_PARTY_NOTICES.md") $licenseRoot
Get-ChildItem $outputRoot | Select-Object Name, Length
