param(
    [string]$Cli = (Join-Path $PSScriptRoot "..\build-vs\bin\Release\hdrbridge-cli.exe"),
    [string]$Workspace = (Join-Path $PSScriptRoot "..\.."),
    [string]$Corpus = (Join-Path $PSScriptRoot "..\..\private-fixtures\golden-inputs"),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "..\..\private-fixtures\golden-inputs")
)

$ErrorActionPreference = "Stop"

function Assert-True($condition, [string]$name) {
    if (-not $condition) { throw "Assertion failed: $name" }
}

function Inspect([string]$path) {
    $text = & $Cli inspect $path 2>$null | Out-String
    if ($LASTEXITCODE -ne 0) { throw "inspect failed: $path" }
    return $text | ConvertFrom-Json
}

if (-not (Test-Path -LiteralPath $Cli)) { throw "CLI not found: $Cli" }

$realAvifPath = Join-Path $Workspace "189A4328.hdr.jxl.B3ywEeRH.avif"
if (Test-Path -LiteralPath $realAvifPath) {
    $realAvif = Inspect $realAvifPath
    Assert-True ($realAvif.assetKind -eq "direct-hdr") "real ICC AVIF is direct HDR"
    Assert-True (-not $realAvif.nativeSignal.present) "real ICC AVIF has no native NCLX"
    Assert-True ($realAvif.iccSignal.present -and $realAvif.iccSignal.cicpPresent) "real ICC AVIF ICC CICP parses"
    Assert-True ($realAvif.iccSignal.primaries -eq 1 -and $realAvif.iccSignal.transfer -eq 16) "real ICC AVIF resolves BT.709/PQ"
    Assert-True ($realAvif.resolvedColor.source -eq "ICC") "real ICC AVIF signaling source"
}

$sdrJxlPath = Join-Path $Workspace "browserImageTestJXL-untagged.jxl"
if (Test-Path -LiteralPath $sdrJxlPath) {
    $sdrJxl = Inspect $sdrJxlPath
    Assert-True ($sdrJxl.assetKind -eq "non-HDR") "untagged browser JXL is SDR"
    Assert-True ($sdrJxl.nativeSignal.primaries -eq 1 -and $sdrJxl.nativeSignal.transfer -eq 13) "untagged browser JXL is BT.709/sRGB"
    Assert-True (-not $sdrJxl.iccSignal.present -and -not $sdrJxl.gainMapPresent) "untagged browser JXL has no HDR representation"
}

$gainJxl = Inspect (Join-Path $Corpus "GM_JXL.JXL")
Assert-True ($gainJxl.assetKind -eq "gain-map-hdr" -and $gainJxl.gainMapFamily -eq "iso-jxl-jhgm") "ISO jhgm JXL remains gain-map HDR"

$fixtures = @(
    @{ Name = "PQ_PNG_ICC.png"; Format = "PNG"; Transfer = 16 },
    @{ Name = "PQ_JXL_ICC.jxl"; Format = "JPEG XL"; Transfer = 16 },
    @{ Name = "PQ_AVIF_ICC.avif"; Format = "AVIF"; Transfer = 16 }
)
foreach ($fixture in $fixtures) {
    $info = Inspect (Join-Path $OutputDirectory $fixture.Name)
    Assert-True ($info.format -eq $fixture.Format -and $info.assetKind -eq "direct-hdr") "$($fixture.Name) format/classification"
    Assert-True (-not $info.nativeSignal.present) "$($fixture.Name) is not native-signaled"
    Assert-True ($info.iccSignal.present -and $info.iccSignal.cicpPresent) "$($fixture.Name) embeds actual ICC+CICP"
    Assert-True ($info.iccSignal.primaries -eq 9 -and $info.iccSignal.transfer -eq $fixture.Transfer) "$($fixture.Name) ICC CICP matches expected HDR signal"
    Assert-True ($info.resolvedColor.source -eq "ICC") "$($fixture.Name) resolves only from ICC"
}

$tiff = Inspect (Join-Path $Corpus "PQ_TIF.tif")
Assert-True ($tiff.assetKind -eq "direct-hdr" -and $tiff.iccSignal.cicpPresent) "existing TIFF is ICC-signaled HDR"
Assert-True (-not $tiff.nativeSignal.present -and $tiff.resolvedColor.source -eq "ICC") "TIFF HDR interpretation comes from ICC"

Write-Host "PASS: private HDR color signaling / ICC audit"
