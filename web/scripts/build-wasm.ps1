param(
  [string]$EmsdkRoot = "D:\emsdk",
  [string]$NinjaPath = "D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
)

$ErrorActionPreference = "Stop"

$webRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $webRoot
$cacheRoot = Join-Path $webRoot ".cache"
$sourceRoot = Join-Path $cacheRoot "src"
$buildRoot = Join-Path $cacheRoot "build"
$prefixRoot = Join-Path $cacheRoot "prefix"
$outputRoot = Join-Path $webRoot "public\codecs\heif-png"
$licenseRoot = Join-Path $webRoot "public\licenses"
$emcmake = Join-Path $EmsdkRoot "upstream\emscripten\emcmake.bat"

if (-not (Test-Path -LiteralPath $emcmake)) {
  throw "Emscripten was not found at $emcmake"
}
if (-not (Test-Path -LiteralPath $NinjaPath)) {
  throw "Ninja was not found at $NinjaPath"
}

New-Item -ItemType Directory -Force -Path $sourceRoot, $buildRoot, $prefixRoot, $outputRoot, $licenseRoot | Out-Null

function Get-PinnedSource {
  param([string]$Name, [string]$Repository, [string]$Tag)
  $destination = Join-Path $sourceRoot $Name
  if (-not (Test-Path -LiteralPath (Join-Path $destination ".git"))) {
    git clone --depth 1 --branch $Tag $Repository $destination
    if ($LASTEXITCODE -ne 0) { throw "Failed to clone $Name" }
  }
  $actual = (git -C $destination describe --tags --exact-match).Trim()
  if ($LASTEXITCODE -ne 0 -or $actual -ne $Tag) {
    throw "$Name cache is not pinned to $Tag"
  }
  return $destination
}

$de265Source = Get-PinnedSource "libde265" "https://github.com/strukturag/libde265.git" "v1.0.16"
$heifSource = Get-PinnedSource "libheif" "https://github.com/strukturag/libheif.git" "v1.23.1"
$de265Build = Join-Path $buildRoot "libde265"
$heifBuild = Join-Path $buildRoot "libheif"
$bridgeBuild = Join-Path $buildRoot "hdrbridge"
$de265Prefix = Join-Path $prefixRoot "libde265"
$heifPrefix = Join-Path $prefixRoot "libheif"

& $emcmake cmake -S $de265Source -B $de265Build -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$NinjaPath" `
  "-DCMAKE_BUILD_TYPE=Release" `
  "-DCMAKE_INSTALL_PREFIX=$de265Prefix" `
  "-DBUILD_SHARED_LIBS=OFF" `
  "-DENABLE_SDL=OFF" `
  "-DENABLE_DECODER=OFF" `
  "-DENABLE_ENCODER=OFF"
if ($LASTEXITCODE -ne 0) { throw "libde265 configure failed" }
cmake --build $de265Build --target install --parallel
if ($LASTEXITCODE -ne 0) { throw "libde265 build failed" }

$de265Library = Join-Path $de265Prefix "lib\libde265.a"
& $emcmake cmake -S $heifSource -B $heifBuild -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$NinjaPath" `
  "-DCMAKE_BUILD_TYPE=Release" `
  "-DCMAKE_INSTALL_PREFIX=$heifPrefix" `
  "-DBUILD_SHARED_LIBS=OFF" `
  "-DENABLE_PLUGIN_LOADING=OFF" `
  "-DENABLE_MULTITHREADING_SUPPORT=OFF" `
  "-DENABLE_PARALLEL_TILE_DECODING=OFF" `
  "-DBUILD_TESTING=OFF" `
  "-DBUILD_DOCUMENTATION=OFF" `
  "-DWITH_EXAMPLES=OFF" `
  "-DWITH_GDK_PIXBUF=OFF" `
  "-DWITH_LIBSHARPYUV=OFF" `
  "-DWITH_LIBDE265=ON" `
  "-DWITH_LIBDE265_PLUGIN=OFF" `
  "-DLIBDE265_INCLUDE_DIR=$de265Prefix\include" `
  "-DLIBDE265_LIBRARY=$de265Library" `
  "-DWITH_X265=OFF" `
  "-DWITH_X264=OFF" `
  "-DWITH_OpenH264_DECODER=OFF" `
  "-DWITH_AOM_DECODER=OFF" `
  "-DWITH_AOM_ENCODER=OFF" `
  "-DWITH_DAV1D=OFF" `
  "-DWITH_JPEG_DECODER=OFF" `
  "-DWITH_JPEG_ENCODER=OFF" `
  "-DWITH_OpenJPEG_DECODER=OFF" `
  "-DWITH_OpenJPEG_ENCODER=OFF"
if ($LASTEXITCODE -ne 0) { throw "libheif configure failed" }
cmake --build $heifBuild --target install --parallel
if ($LASTEXITCODE -ne 0) { throw "libheif build failed" }

& $emcmake cmake -S (Join-Path $webRoot "wasm") -B $bridgeBuild -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$NinjaPath" `
  "-DCMAKE_BUILD_TYPE=Release" `
  "-DHDRBRIDGE_HEIF_ROOT=$heifPrefix" `
  "-DHDRBRIDGE_DE265_ROOT=$de265Prefix"
if ($LASTEXITCODE -ne 0) { throw "HDR Bridge WASM configure failed" }
cmake --build $bridgeBuild --parallel
if ($LASTEXITCODE -ne 0) { throw "HDR Bridge WASM build failed" }

Copy-Item -Force (Join-Path $bridgeBuild "dist\hdrbridge-core.mjs") $outputRoot
Copy-Item -Force (Join-Path $bridgeBuild "dist\hdrbridge-core.wasm") $outputRoot
Copy-Item -Force (Join-Path $heifSource "COPYING") (Join-Path $licenseRoot "libheif-LGPL-3.0.txt")
Copy-Item -Force (Join-Path $de265Source "COPYING") (Join-Path $licenseRoot "libde265-LGPL-3.0.txt")

Get-ChildItem $outputRoot | Select-Object Name, Length
