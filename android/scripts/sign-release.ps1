param(
  [Parameter(Mandatory)][string]$Apk,
  [Parameter(Mandatory)][string]$Output,
  [Parameter(Mandatory)][string]$SigningProperties,
  [string]$Sdk = $env:ANDROID_HOME
)
$ErrorActionPreference = 'Stop'
$androidRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$properties = @{}
foreach ($line in [IO.File]::ReadAllLines((Resolve-Path -LiteralPath $SigningProperties))) {
  if ($line -match '^\s*[#!]' -or -not $line.Trim()) { continue }
  if ($line -notmatch '^([A-Za-z]+)\s*=\s*(.*)$') { throw 'Invalid signing properties format.' }
  $name = $Matches[1]; $value = $Matches[2]
  if ($properties.ContainsKey($name)) { throw "Duplicate signing property: $name" }
  $properties[$name] = [regex]::Replace($value, '\\(u[0-9a-fA-F]{4}|.)', {
    param($m)
    $v = $m.Groups[1].Value
    if ($v.StartsWith('u') -and $v.Length -eq 5) { return [string][char][Convert]::ToInt32($v.Substring(1), 16) }
    switch -CaseSensitive ($v) { 'n' { "`n" } 'r' { "`r" } 't' { "`t" } 'f' { "`f" } default { $v } }
  })
}
foreach ($name in @('storeFile','storePassword','keyAlias','keyPassword')) {
  if (-not $properties[$name]) { throw "Missing signing property: $name" }
}
foreach ($name in @('storeFile')) {
  if (-not (Test-Path -LiteralPath $properties[$name] -PathType Leaf)) { throw "Signing file is missing: $name" }
}
$signer = Join-Path $Sdk 'build-tools/35.0.0/apksigner.bat'
$releasePin = (Get-Content -LiteralPath (Join-Path $androidRoot 'signing/release-cert-sha256.txt') -Raw).Trim()
$environment = @{}
try {
  foreach ($name in @('storePassword','keyPassword')) {
    $envName = 'HDRBRIDGE_SIGN_' + $name
    $environment[$envName] = [Environment]::GetEnvironmentVariable($envName)
    [Environment]::SetEnvironmentVariable($envName, $properties[$name])
  }
  & $signer sign --ks $properties.storeFile --ks-key-alias $properties.keyAlias `
    --ks-pass env:HDRBRIDGE_SIGN_storePassword --key-pass env:HDRBRIDGE_SIGN_keyPassword `
    --v4-signing-enabled false `
    --out $Output $Apk
  if ($LASTEXITCODE -ne 0) { throw 'Production APK signing failed.' }
  foreach ($platform in @(@{min=26;max=27}, @{min=28;max=32}, @{min=33;max=36})) {
    $report = & $signer verify --verbose --print-certs --min-sdk-version $platform.min --max-sdk-version $platform.max $Output 2>&1
    if ($LASTEXITCODE -ne 0 -or -not ($report -match "Signer #1 certificate SHA-256 digest: $releasePin")) {
      throw "APK certificate verification failed for Android API $($platform.min)-$($platform.max)."
    }
  }
  Write-Host 'Production signature verified with the same release certificate (Android API 26-36).'
} finally {
  foreach ($entry in $environment.GetEnumerator()) { [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value) }
}
