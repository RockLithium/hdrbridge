param(
    [string]$Cli = (Join-Path $PSScriptRoot "..\build-vs\bin\Release\hdrbridge-cli.exe"),
    [string]$Camera = (Join-Path $PSScriptRoot "..\..\private-fixtures\golden-inputs"),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "..\..\private-fixtures\test-output\mobile-current")
)

$ErrorActionPreference = "Stop"

function Assert-True($condition, [string]$name) {
    if (-not $condition) { throw "Assertion failed: $name" }
}

function Assert-Near([double]$actual, [double]$expected, [double]$tolerance, [string]$name) {
    if ([math]::Abs($actual - $expected) -gt $tolerance) {
        throw "$name expected $expected +/- $tolerance, got $actual"
    }
}

function Invoke-Inspect([string]$path) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $text = & $Cli inspect $path 2>$null | Out-String
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    if ($code -ne 0) { throw "inspect failed: $path" }
    return $text | ConvertFrom-Json
}

function Invoke-Conversion([string]$source, [string]$output, [string]$mode) {
    $log = "$output.log"
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $text = & $Cli convert $source $output "--mode=$mode" --overwrite 2> $log | Out-String
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    if ($code -ne 0) { throw "$mode conversion failed for $source; see $log" }
    $result = $text | ConvertFrom-Json
    Assert-True ($result.success -and $result.verification.passed) "$mode verified: $source"
    Assert-True ($result.verification.width -eq 4284 -and $result.verification.height -eq 5712) "portrait canonical dimensions: $mode"
    Assert-True ($result.verification.hdrDiagnostics.maxChannelNits -gt 500) "real HDR range reconstructed: $mode"
    if ($source -eq $iphoneHeic) {
        Assert-Near $result.verification.hdrDiagnostics.maxChannelNits 551.35 1.0 "Apple HEIC Adaptive HDR peak: $mode"
        Assert-True ($result.timings.gainMapMs -lt 1500) "Apple HEIC reconstruction performance: $mode"
    }
    if ($source -eq $iphoneJpeg) {
        Assert-Near $result.verification.hdrDiagnostics.maxChannelNits 1089.57 1.0 "Apple JPEG reconstruction remains stable: $mode"
        Assert-True ($result.timings.gainMapMs -lt 1500) "Apple JPEG post-reconstruction performance: $mode"
    }
    if ($mode -eq "jxr-scrgb-fp16") {
        Assert-True ($result.verification.minValue -gt -0.02) "FP16 scRGB negative excursion is bounded: $source"
    }
    return $result
}

if (-not (Test-Path -LiteralPath $Cli)) { throw "CLI not found: $Cli" }
if (-not (Test-Path -LiteralPath $Camera)) { Write-Host "SKIPPED: private Camera corpus unavailable"; exit 0 }

$iphoneHeic = Join-Path $Camera "GM_HEIC_Apple.HEIC"
$iphoneJpeg = Join-Path $Camera "GM_JPEG_Apple.JPG"
$xiaomiHdrJpeg = Join-Path $Camera "GM_JPEG_Android.jpg"
$xiaomiSdrHeic = Join-Path $Camera "SDR_HEIC.HEIC"

$expectedHashes = @{
    $iphoneHeic = "639ED94EA6DEFD5F6A708CB85436B9407C09CACC8B5F73B72D4E7035CBB10FB2"
    $iphoneJpeg = "89DF075A4F03907AF24F1FAEA00C59484F39D2994FC6CEEF70E587CBBFE9E39E"
    $xiaomiHdrJpeg = "DAE7029A624E4B7C73BED469E100B9B524FA3EB20D901894D0888870DBE283E4"
    $xiaomiSdrHeic = "BC416D59D293B3AA09771D212606ECC505FFA6A083BC9A6A6710F21146E2B963"
}
foreach ($entry in $expectedHashes.GetEnumerator()) {
    Assert-True ((Get-FileHash -LiteralPath $entry.Key -Algorithm SHA256).Hash -eq $entry.Value) "private mobile golden SHA-256: $($entry.Key)"
}

$heicInfo = Invoke-Inspect $iphoneHeic
Assert-True ($heicInfo.assetKind -eq "gain-map-hdr" -and $heicInfo.gainMapFamily -eq "apple-auxiliary-tmap") "Apple HEIC isolated adapter family"
Assert-True ($heicInfo.gainMapItems.base -eq 46 -and $heicInfo.gainMapItems.gainMap -eq 62 -and $heicInfo.gainMapItems.toneMap -eq 122) "Apple HEIC item relationship graph"
Assert-True ($heicInfo.width -eq 4284 -and $heicInfo.height -eq 5712 -and $heicInfo.orientation.source -eq 6) "Apple HEIC orientation"
Assert-True ($heicInfo.gainMapSize.width -eq 2142 -and $heicInfo.gainMapSize.height -eq 2856) "Apple HEIC gain-map dimensions"
Assert-Near $heicInfo.gainMapMetadata.alternateHdrHeadroom 2.462891 0.000001 "Apple HEIC ISO headroom"
Assert-Near ([math]::Pow(2, $heicInfo.gainMapMetadata.alternateHdrHeadroom)) 5.513203 0.00001 "Apple HEIC XMP/ISO linear headroom agreement"
Assert-True $heicInfo.xmpPresent "Apple HEIC auxiliary XMP extracted"

$jpegInfo = Invoke-Inspect $iphoneJpeg
Assert-True ($jpegInfo.assetKind -eq "gain-map-hdr" -and $jpegInfo.gainMapFamily -eq "apple-mpf-iso") "Apple JPEG isolated adapter family"
Assert-True ($jpegInfo.width -eq 4284 -and $jpegInfo.height -eq 5712 -and $jpegInfo.orientation.source -eq 8) "Apple JPEG orientation"
Assert-True ($jpegInfo.gainMapSize.width -eq 2856 -and $jpegInfo.gainMapSize.height -eq 2142) "Apple JPEG MPF gain-map dimensions"
Assert-Near ([math]::Pow(2, $jpegInfo.gainMapMetadata.alternateHdrHeadroom)) 5.365930 0.00002 "Apple JPEG XMP/ISO linear headroom agreement"

$xiaomiPositive = Invoke-Inspect $xiaomiHdrJpeg
Assert-True ($xiaomiPositive.assetKind -eq "gain-map-hdr" -and $xiaomiPositive.gainMapPresent) "Xiaomi Ultra HDR JPEG positive fixture"
$xiaomiNegative = Invoke-Inspect $xiaomiSdrHeic
Assert-True ($xiaomiNegative.assetKind -eq "non-PQ/unknown" -and -not $xiaomiNegative.gainMapPresent -and $xiaomiNegative.color.transfer -ne 16) "Xiaomi HEIC SDR negative fixture"

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$cases = @(
    @{ Mode = "ultrahdr"; Extension = "jpg" },
    @{ Mode = "png-pq16"; Extension = "png" },
    @{ Mode = "jxl-pq16"; Extension = "jxl" },
    @{ Mode = "jxr-scrgb-fp16"; Extension = "jxr" },
    @{ Mode = "avif-pq10"; Extension = "avif" },
    @{ Mode = "tiff-pq16"; Extension = "tif" }
)
$results = @()
foreach ($sourceCase in @(
    @{ Path = $iphoneHeic; Prefix = "iphone-heic" },
    @{ Path = $iphoneJpeg; Prefix = "iphone-jpeg" }
)) {
    foreach ($case in $cases) {
        $output = Join-Path $OutputDirectory "$($sourceCase.Prefix)-$($case.Mode).$($case.Extension)"
        $result = Invoke-Conversion $sourceCase.Path $output $case.Mode
        $results += [pscustomobject]@{
            source = $sourceCase.Prefix
            mode = $case.Mode
            output = $result.outputPath
            sha256 = $result.sha256
            maxChannelNits = $result.verification.hdrDiagnostics.maxChannelNits
            percentile99_99Nits = $result.verification.hdrDiagnostics.percentile99_99Nits
        }
    }
}
$results | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 (Join-Path $OutputDirectory "summary.json")
Write-Host "PASS: Apple HEIC/JPEG reconstruction, Xiaomi positive/negative, and all output paths"
