param(
  [string]$Ndk = $env:ANDROID_NDK_HOME,
  [string]$Vcpkg = (Join-Path $PSScriptRoot '..\..\.tools\vcpkg\vcpkg.exe')
)
$ErrorActionPreference = 'Stop'
$android = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$repo = Split-Path -Parent $android
if (-not $Ndk -or -not (Test-Path (Join-Path $Ndk 'build\cmake\android.toolchain.cmake'))) { throw 'Pass -Ndk or set ANDROID_NDK_HOME to an installed Android NDK.' }
if (-not (Test-Path $Vcpkg)) { throw "vcpkg executable not found: $Vcpkg" }
$env:ANDROID_NDK_HOME = (Resolve-Path $Ndk).Path
& $Vcpkg install --x-manifest-root=$android --triplet=arm64-android-hdr --overlay-triplets="$android\triplets" --overlay-ports="$android\ports-overlay" --overlay-ports="$repo\ports-overlay/aom" --x-install-root="$android\vcpkg_installed" --x-buildtrees-root="$android\build-deps\buildtrees" --x-packages-root="$android\build-deps\packages"
if ($LASTEXITCODE -ne 0) { throw "vcpkg failed with exit code $LASTEXITCODE" }
