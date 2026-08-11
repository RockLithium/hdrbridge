param(
  [string]$FixtureRoot = (Join-Path $PSScriptRoot "..\..\..\private-fixtures\golden-inputs"),
  [string]$OutputRoot = (Join-Path $PSScriptRoot "..\..\..\private-fixtures\test-output\web-regression"),
  [string]$Cli = (Join-Path $PSScriptRoot "..\..\build-vs\bin\Release\hdrbridge-cli.exe"),
  [string]$CoreModule = (Join-Path $PSScriptRoot "..\public\codecs\heif-png\hdrbridge-core.mjs")
)

$ErrorActionPreference = "Stop"
$converter = Join-Path $PSScriptRoot "convert-heif.mjs"
$hasher = Join-Path $PSScriptRoot "png-pixel-hash.mjs"

foreach ($required in @($Cli, $CoreModule, $converter, $hasher)) {
  if (-not (Test-Path -LiteralPath $required)) { throw "Missing test dependency: $required" }
}
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

$cases = @(
  @{ Name = "canon-pq"; Input = "0U2A0009.HIF"; Primaries = 9; Transfer = 16; Gamut = "rec2020"; TransferName = "pq" },
  @{ Name = "canon-p3"; Input = "0U2A0009.HIF"; Primaries = 12; Transfer = 16; Gamut = "p3"; TransferName = "pq" },
  @{ Name = "nikon-pq"; Input = "FGD_9511.HIF"; Primaries = 9; Transfer = 16; Gamut = "rec2020"; TransferName = "pq" },
  @{ Name = "nikon-hlg"; Input = "FGD_9511.HIF"; Primaries = 9; Transfer = 18; Gamut = "rec2020"; TransferName = "hlg" }
)

$created = [System.Collections.Generic.List[string]]::new()
try {
  foreach ($case in $cases) {
    $input = Join-Path $FixtureRoot $case.Input
    if (-not (Test-Path -LiteralPath $input)) { throw "Missing private fixture: $input" }
    $webOutput = Join-Path $OutputRoot "$($case.Name)-web.png"
    $desktopOutput = Join-Path $OutputRoot "$($case.Name)-desktop.png"
    $created.Add($webOutput)
    $created.Add($desktopOutput)

    & node $converter $CoreModule $input $webOutput $case.Primaries $case.Transfer 1
    if ($LASTEXITCODE -ne 0) { throw "$($case.Name) Web conversion failed" }
    & $Cli convert $input $desktopOutput --mode=png-pq16 "--gamut=$($case.Gamut)" `
      "--transfer=$($case.TransferName)" --no-icc --png-compression=1 --overwrite | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "$($case.Name) desktop reference failed" }

    $hashLines = @(& node $hasher $CoreModule $webOutput $desktopOutput)
    if ($LASTEXITCODE -ne 0 -or $hashLines.Count -ne 2) {
      throw "$($case.Name) pixel hash failed"
    }
    $webCrc = ($hashLines[0] -split "\s+")[0]
    $desktopCrc = ($hashLines[1] -split "\s+")[0]
    if ($webCrc -ne $desktopCrc) {
      throw "$($case.Name) pixel mismatch: Web $webCrc, desktop $desktopCrc"
    }
    Write-Host "PASS $($case.Name) RGB16 CRC-32 $webCrc"
  }
} finally {
  foreach ($path in $created) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
  }
}
