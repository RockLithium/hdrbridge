param(
    [string]$Cli = (Join-Path $PSScriptRoot "..\build-vs\bin\Release\hdrbridge-cli.exe"),
    [string]$Corpus = (Join-Path $PSScriptRoot "..\..\private-fixtures\golden-inputs"),
    [string]$ReferenceOutputs = (Join-Path $PSScriptRoot "..\..\private-fixtures\golden-inputs"),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "..\..\private-fixtures\test-output\format-current"),
    [string]$CompatibilityDirectory = (Join-Path $PSScriptRoot "..\..\private-fixtures\test-output\compatibility-png-icc")
)

$ErrorActionPreference = "Stop"

function Assert-True($condition, [string]$name) {
    if (-not $condition) { throw "Assertion failed: $name" }
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

function Invoke-Conversion([string]$sourcePath, [string]$outputPath, [string]$mode, [string[]]$extra = @()) {
    $log = "$outputPath.log"
    $arguments = @("convert", $sourcePath, $outputPath, "--mode=$mode", "--overwrite") + $extra
    Write-Host "RUN: $mode -> $outputPath"
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $text = & $Cli @arguments 2> $log | Out-String
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    if ($code -ne 0) { throw "$mode conversion failed; see $log" }
    $result = $text | ConvertFrom-Json
    Assert-True ($result.success -and $result.verification.passed) "$mode verified"
    return $result
}

if (-not (Test-Path -LiteralPath $Cli)) { throw "CLI not found: $Cli" }
if (-not (Test-Path -LiteralPath $Corpus)) { Write-Host "SKIPPED: private format corpus unavailable"; exit 0 }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
New-Item -ItemType Directory -Force -Path $CompatibilityDirectory | Out-Null

$bright = Join-Path $Corpus "PQ_HIF.HIF"
$brightInfo = Invoke-Inspect $bright
Assert-True ($brightInfo.width -eq 6960 -and $brightInfo.height -eq 4640) "PQ HIF dimensions"
Assert-True ($brightInfo.color.primaries -eq 9 -and $brightInfo.color.transfer -eq 16 -and $brightInfo.color.matrix -eq 9) "bright Canon CICP"
Assert-True ($brightInfo.assetKind -eq "direct-hdr") "bright Canon direct-HDR classification"

$acrTiff = Invoke-Inspect (Join-Path $Corpus "GM_TIF.TIF")
Assert-True ($acrTiff.assetKind -eq "gain-map-hdr" -and $acrTiff.gainMapPresent) "ACR gain-map TIFF classification"
Assert-True ($acrTiff.gainMapFamily -eq "adobe-iso-tiff" -and $acrTiff.gainMapSize.width -gt 0) "Adobe TIFF gain-map SubIFD adapter"
$acrAvif = Invoke-Inspect (Join-Path $Corpus "GM_AVIF.AVIF")
Assert-True ($acrAvif.assetKind -eq "gain-map-hdr" -and $acrAvif.gainMapPresent) "ACR tmap AVIF classification"
Assert-True ($acrAvif.gainMapFamily -eq "adobe-iso-tmap") "Adobe tmap family is not conflated with Apple/Ultra HDR"
Assert-True ($acrAvif.gainMapItems.base -gt 0 -and $acrAvif.gainMapItems.gainMap -gt 0 -and $acrAvif.gainMapItems.toneMap -gt 0) "Adobe item relationship graph"
Assert-True ($acrAvif.gainMapMetadata.alternateHdrHeadroom -gt 1) "Adobe gain-map headroom metadata"
Assert-True ([math]::Abs($acrTiff.gainMapMetadata.alternateHdrHeadroom - $acrAvif.gainMapMetadata.alternateHdrHeadroom) -lt 0.000001) "TIFF and AVIF ISO headroom metadata agree"

foreach ($negativeJpeg in @("SDR_JPEG.jpg")) {
    $negativePath = Join-Path $Corpus $negativeJpeg
    $negativeInfo = Invoke-Inspect $negativePath
    Assert-True (($negativeInfo.assetKind -eq "non-HDR" -or
                  $negativeInfo.assetKind -eq "non-PQ/unknown") -and
        -not $negativeInfo.gainMapPresent) "ordinary JPEG is inspectable but not misclassified as Ultra HDR: $negativeJpeg"
}

$results = @()
$adobeModes = @(
    @{ Mode = "ultrahdr"; Name = "adobe-tmap_faithful-auto.jpg" },
    @{ Mode = "png-pq16"; Name = "adobe-tmap_rec2020-pq16.png" },
    @{ Mode = "jxl-pq16"; Name = "adobe-tmap_master.jxl" },
    @{ Mode = "jxr-scrgb-fp16"; Name = "adobe-tmap_master-fp16.jxr" },
    @{ Mode = "avif-pq10"; Name = "adobe-tmap_direct-pq10.avif" },
    @{ Mode = "tiff-pq16"; Name = "adobe-tmap_direct-hdr.tif" }
)
foreach ($case in $adobeModes) {
    $result = Invoke-Conversion (Join-Path $Corpus "GM_AVIF.AVIF") `
        (Join-Path $OutputDirectory $case.Name) $case.Mode
    $gainDiagnostics = $result.verification.hdrDiagnostics
    Assert-True ($gainDiagnostics.inputGainMapFormulaMaxError -gt 0 -and $gainDiagnostics.inputGainMapFormulaMaxError -le (2.5 / 65535.0)) "Adobe independent gain-map formula check: $($case.Mode)"
    Assert-True ($gainDiagnostics.maxChannelNits -gt 500 -and $gainDiagnostics.percentile99_99Nits -gt 300) "gain-map AVIF reconstruction has real HDR range: $($case.Mode)"
    $results += $result
}

foreach ($case in $adobeModes) {
    $result = Invoke-Conversion (Join-Path $Corpus "GM_TIF.TIF") `
        (Join-Path $OutputDirectory ($case.Name -replace '^adobe-tmap_', 'adobe-tiff_')) $case.Mode
    $gainDiagnostics = $result.verification.hdrDiagnostics
    Assert-True ($gainDiagnostics.maxChannelNits -gt 500 -and $gainDiagnostics.percentile99_99Nits -gt 300) "Adobe TIFF reconstruction has real HDR range: $($case.Mode)"
    $results += $result
}

$pngA = Invoke-Conversion (Join-Path $Corpus "GM_AVIF.AVIF") `
    (Join-Path $CompatibilityDirectory "PNG-A-cICP-plus-legacy-ICC.png") "png-pq16" @("--png-icc-name=HDR Bridge Rec.2100 PQ")
$pngB = Invoke-Conversion (Join-Path $Corpus "GM_AVIF.AVIF") `
    (Join-Path $CompatibilityDirectory "PNG-B-cICP-only.png") "png-pq16" @("--no-icc")
$pngC = Invoke-Conversion (Join-Path $Corpus "GM_AVIF.AVIF") `
    (Join-Path $CompatibilityDirectory "PNG-C-Rec.2100-PQ.png") "png-pq16"
$pngD = Invoke-Conversion (Join-Path $Corpus "GM_AVIF.AVIF") `
    (Join-Path $CompatibilityDirectory "PNG-D-Rec.2100-PQ-BT.2020.png") "png-pq16" @("--png-icc-name=Rec.2100 PQ (BT.2020)")
Assert-True ($pngA.verification.checks -contains "compatible HDR PQ ICC profile embedded") "PNG A embeds ICC"
Assert-True ($pngB.verification.checks -contains "cICP-only A/B variant contains no ICC profile") "PNG B omits ICC"
Assert-True ($pngC.verification.exactRoundtrip -and $pngD.verification.exactRoundtrip) "PNG C/D pixels retain exact RGB16 master"
$results += $pngA, $pngB, $pngC, $pngD

$uhdr = Invoke-Conversion $bright (Join-Path $OutputDirectory "bright_faithful-auto_ultrahdr.jpg") "ultrahdr"
$d = $uhdr.verification.hdrDiagnostics
Assert-True ($d.maxChannelNits -gt 0 -and $d.percentile99_99Nits -gt 0) "Ultra HDR source luminance diagnostics"
Assert-True ($d.chosenTargetPeakNits -ge 203 -and $d.chosenTargetPeakNits -lt 10000) "Faithful/Auto does not use blind 10000-nit ceiling"
Assert-True ([math]::Abs($d.hdrCapacityMax - ($d.chosenTargetPeakNits / 203.0)) -le [math]::Max(0.02, ($d.chosenTargetPeakNits / 203.0) * 0.02)) "Ultra HDR capacity matches chosen peak"
$results += $uhdr

$png = Invoke-Conversion $bright (Join-Path $OutputDirectory "bright_rec2020-pq16.png") "png-pq16"
Assert-True ($png.verification.bitDepth -eq 16 -and $png.verification.exactRoundtrip) "PNG RGB16 exact roundtrip"
Assert-True ($png.verification.colorEncoding -match "9/16/0/1") "PNG Rec.2020/PQ cICP"
$results += $png

$generatedTiff = Join-Path $OutputDirectory "bright_rec2020-pq16.tif"
$tiff = Invoke-Conversion $bright $generatedTiff "tiff-pq16"
Assert-True ($tiff.verification.bitDepth -eq 16 -and $tiff.verification.exactRoundtrip) "TIFF RGB16 exact roundtrip"
Assert-True ($tiff.verification.colorEncoding -match "Rec.2020/PQ") "TIFF direct PQ ICC"
$results += $tiff

$generatedAvif = Join-Path $OutputDirectory "bright_direct-rec2020-pq10.avif"
$avif = Invoke-Conversion $bright $generatedAvif "avif-pq10" @("--quality=90")
Assert-True ($avif.verification.bitDepth -eq 10 -and $avif.verification.pixelFormat -match "4:4:4") "direct AVIF 10-bit 4:4:4"
Assert-True ($avif.verification.colorEncoding -match "9/16/9") "direct AVIF Rec.2020/PQ CICP"
$results += $avif

$interchange = @(
    @{ Input = (Join-Path $ReferenceOutputs "PQ_JXL.jxl"); Output = "from-reference-jxl.png"; Mode = "png-pq16" },
    @{ Input = (Join-Path $ReferenceOutputs "SCRGB_JXR.jxr"); Output = "from-reference-fp16-jxr.png"; Mode = "png-pq16" },
    @{ Input = (Join-Path $ReferenceOutputs "GM_JPEG_ACR.JPG"); Output = "from-reference-ultrahdr.png"; Mode = "png-pq16" },
    @{ Input = $generatedTiff; Output = "from-direct-pq-tiff.jxl"; Mode = "jxl-pq16" },
    @{ Input = $generatedAvif; Output = "from-direct-pq-avif.png"; Mode = "png-pq16" }
)
foreach ($case in $interchange) {
    Assert-True (Test-Path -LiteralPath $case.Input) "interchange input exists: $($case.Input)"
    $result = Invoke-Conversion $case.Input (Join-Path $OutputDirectory $case.Output) $case.Mode
    $results += $result
}

$summary = $results | ForEach-Object {
    [pscustomobject]@{
        mode = $_.mode
        path = $_.outputPath
        bytes = $_.outputBytes
        sha256 = $_.sha256
        verification = $_.verification
    }
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 (Join-Path $OutputDirectory "summary.json")
Write-Host "PASS: private format conversion and interchange suite"
