param(
  [string]$Sdk = $env:ANDROID_HOME,
  [string]$Java = $env:JAVA_HOME,
  [string]$Gradle,
  [ValidateSet('Preview','Release')][string]$Configuration = 'Preview',
  [string]$SigningProperties = $env:HDRBRIDGE_ANDROID_SIGNING_PROPERTIES,
  [switch]$RebuildDependencies
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot '../android/scripts/build.ps1') @PSBoundParameters
