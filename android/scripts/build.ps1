param(
  [string]$Sdk = $env:ANDROID_HOME,
  [string]$Java = $env:JAVA_HOME,
  [string]$Gradle,
  [ValidateSet('Preview','Release')][string]$Configuration = 'Preview',
  [string]$SigningProperties = $env:HDRBRIDGE_ANDROID_SIGNING_PROPERTIES,
  [switch]$RebuildDependencies
)
$ErrorActionPreference = 'Stop'
$androidRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$repo = (Resolve-Path (Join-Path $androidRoot '..')).Path
if ($Configuration -eq 'Release') {
  if (-not $SigningProperties) { $SigningProperties = Join-Path $androidRoot '.local-signing/release.properties' }
  if (-not (Test-Path -LiteralPath $SigningProperties -PathType Leaf)) {
    throw 'Release builds require persistent signing credentials. See android/signing/README.md.'
  }
  $SigningProperties = (Resolve-Path -LiteralPath $SigningProperties).Path
}
if (-not $Sdk -or -not (Test-Path -LiteralPath $Sdk)) { throw 'Pass -Sdk or set ANDROID_HOME.' }
if (-not $Java -or -not (Test-Path -LiteralPath (Join-Path $Java 'bin/javac.exe'))) { throw 'Pass -Java or set JAVA_HOME to JDK 17+.' }
$javaVersion = & (Join-Path $Java 'bin/javac.exe') -version 2>&1
if ($LASTEXITCODE -ne 0 -or "$javaVersion" -notmatch 'javac (\d+)' -or [int]$Matches[1] -lt 17) {
  throw "JDK 17+ is required: $javaVersion"
}
$Sdk = (Resolve-Path -LiteralPath $Sdk).Path
$env:JAVA_HOME = (Resolve-Path -LiteralPath $Java).Path
$env:ANDROID_HOME = $Sdk
$env:ANDROID_NDK_HOME = Join-Path $Sdk 'ndk/30.0.16248370'
$buildTools = Join-Path $Sdk 'build-tools/35.0.0'
foreach ($required in @(
  'platforms/android-36/android.jar', 'build-tools/35.0.0/apksigner.bat',
  'build-tools/35.0.0/zipalign.exe', 'cmake/3.31.6/bin/cmake.exe',
  'ndk/30.0.16248370/build/cmake/android.toolchain.cmake'
)) {
  if (-not (Test-Path -LiteralPath (Join-Path $Sdk $required))) { throw "Missing Android SDK component: $required" }
}
if (-not $Gradle) { $Gradle = Join-Path $androidRoot 'gradlew.bat' }
$version = (Get-Content -LiteralPath (Join-Path $repo 'VERSION') -First 1).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'VERSION must contain major.minor.patch.' }
$nativePrefix = Join-Path $androidRoot 'vcpkg_installed/arm64-android-hdr'
$aomManifest = Get-Content -LiteralPath (Join-Path $repo 'ports-overlay/aom/vcpkg.json') -Raw | ConvertFrom-Json
$aomVersion = "$($aomManifest.'version-semver')#$($aomManifest.'port-version')"
$aomSbom = Join-Path $nativePrefix 'share/aom/vcpkg.spdx.json'
$installedAomVersion = if (Test-Path -LiteralPath $aomSbom -PathType Leaf) {
  $installedAom = Get-Content -LiteralPath $aomSbom -Raw | ConvertFrom-Json
  ($installedAom.packages | Where-Object { $_.name -eq 'aom' }).versionInfo
}
if ($RebuildDependencies -or $installedAomVersion -ne $aomVersion -or
    -not (Test-Path -LiteralPath (Join-Path $nativePrefix 'lib/libavif.a')) -or
    -not (Test-Path -LiteralPath (Join-Path $nativePrefix 'lib/libexpat.a'))) {
  $vcpkg = Join-Path $repo '.tools/vcpkg/vcpkg.exe'
  if (-not (Test-Path -LiteralPath $vcpkg)) {
    & (Join-Path $repo 'scripts/bootstrap-vcpkg-local.ps1')
    if (-not (Test-Path -LiteralPath $vcpkg)) { throw 'vcpkg bootstrap failed.' }
  }
  & (Join-Path $PSScriptRoot 'build-deps.ps1') -Ndk $env:ANDROID_NDK_HOME -Vcpkg $vcpkg
}
& (Join-Path $PSScriptRoot 'prepare-ultrahdr.ps1')
& (Join-Path $PSScriptRoot 'prepare-notices.ps1')
$sdkProperty = $Sdk.Replace('\', '/').Replace(':', '\:')
$sdkProperty = [regex]::Replace($sdkProperty, '[^\x00-\x7f]', { param($m) '\u{0:x4}' -f [int][char]$m.Value })
Set-Content -LiteralPath (Join-Path $androidRoot 'local.properties') -Encoding ASCII -Value ('sdk.dir=' + $sdkProperty)
Push-Location $androidRoot
$previousSigningProperties = $env:HDRBRIDGE_ANDROID_SIGNING_PROPERTIES
try {
  if ($Configuration -eq 'Release') { $env:HDRBRIDGE_ANDROID_SIGNING_PROPERTIES = $SigningProperties }
  & $Gradle --console=plain ":app:assemble$Configuration" ":app:lint$Configuration" :app:testDebugUnitTest
  if ($LASTEXITCODE -ne 0) { throw "Android build failed: $LASTEXITCODE" }
  $variant = $Configuration.ToLowerInvariant()
  $sourceApk = Join-Path $androidRoot "app/build/outputs/apk/$variant/app-$variant.apk"
  $metadata = Get-Content -LiteralPath "app/build/outputs/apk/$variant/output-metadata.json" -Raw | ConvertFrom-Json
  if ($metadata.elements[0].versionName -ne $version) { throw 'APK version does not match VERSION.' }
  if ($Configuration -eq 'Release') {
    $signedApk = Join-Path $androidRoot 'app/build/outputs/apk/release/app-release-verified.apk'
    & (Join-Path $PSScriptRoot 'sign-release.ps1') -Apk $sourceApk -Output $signedApk -SigningProperties $SigningProperties -Sdk $Sdk
    $sourceApk = $signedApk
  }
  & (Join-Path $buildTools 'apksigner.bat') verify $sourceApk
  if ($LASTEXITCODE -ne 0) { throw 'APK signature verification failed.' }
  & (Join-Path $buildTools 'zipalign.exe') -c -P 16 4 $sourceApk
  if ($LASTEXITCODE -ne 0) { throw 'APK 16 KB alignment verification failed.' }
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $archive = [IO.Compression.ZipFile]::OpenRead($sourceApk)
  try {
    $expectedAssets = @{}
    $expectedAssets['assets/fonts/Roboto-Bold.ttf'] = Join-Path $repo 'assets/fonts/Roboto-Bold.ttf'
    $expectedAssets['assets/fonts/Roboto-LICENSE.txt'] = Join-Path $repo 'assets/fonts/Roboto-LICENSE.txt'
    foreach ($logo in Get-ChildItem -LiteralPath (Join-Path $repo 'assets/frame-logos') -File) {
      $expectedAssets["assets/frame-logos/$($logo.Name)"] = $logo.FullName
    }
    foreach ($entry in $expectedAssets.GetEnumerator()) {
      if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) { throw "Missing expected asset: $($entry.Value)" }
    }
    $unexpected = @($archive.Entries | Where-Object {
      $_.FullName -match '(^|/)(private-fixtures|private-inputs|test-output|manual-test-assets|androidTest)(/|$)' -or
      ($_.FullName.StartsWith('assets/') -and $_.FullName -notmatch '^assets/licenses/[^/]+\.txt$' -and
       -not $expectedAssets.ContainsKey($_.FullName))
    })
    if ($unexpected.Count) { throw "Unexpected APK payload: $($unexpected.FullName -join ', ')" }
    $assetEntries = @($archive.Entries | Where-Object { $expectedAssets.ContainsKey($_.FullName) })
    if ($assetEntries.Count -ne $expectedAssets.Count) { throw 'APK is missing an explicitly allowed font or frame logo asset.' }
    foreach ($entry in $assetEntries) {
      $stream = $entry.Open()
      $sha = [Security.Cryptography.SHA256]::Create()
      try { $actualHash = [Convert]::ToHexString($sha.ComputeHash($stream)) }
      finally { $sha.Dispose(); $stream.Dispose() }
      $expectedHash = (Get-FileHash -LiteralPath $expectedAssets[$entry.FullName] -Algorithm SHA256).Hash
      if ($actualHash -ne $expectedHash) { throw "APK asset does not match approved source: $($entry.FullName)" }
    }
    if (@($archive.Entries | Where-Object { $_.FullName -like 'assets/licenses/*.txt' }).Count -lt 20) {
      throw 'APK third-party license texts are incomplete.'
    }
  } finally { $archive.Dispose() }
  $output = Join-Path $androidRoot 'dist'
  New-Item -ItemType Directory -Path $output -Force | Out-Null
  $filename = if ($Configuration -eq 'Release') { "HDRBridge-v$version-Android-arm64.apk" } else { "HDRBridge-v$version-Android-arm64-preview.apk" }
  $apk = Join-Path $output $filename
  Copy-Item -LiteralPath $sourceApk -Destination $apk -Force
  $hash = (Get-FileHash -LiteralPath $apk -Algorithm SHA256).Hash.ToLowerInvariant()
  Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding ASCII -Value "$hash  $([IO.Path]::GetFileName($apk))"
  if ($Configuration -eq 'Release') {
    Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS-Android.txt') -Encoding ASCII -Value "$hash  $([IO.Path]::GetFileName($apk))"
  }
  Write-Host "APK: $apk"
  Write-Host "SHA-256: $hash"
} finally { $env:HDRBRIDGE_ANDROID_SIGNING_PROPERTIES = $previousSigningProperties; Pop-Location }
