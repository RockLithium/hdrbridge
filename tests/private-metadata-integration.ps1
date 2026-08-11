param(
    [string]$Cli = (Join-Path $PSScriptRoot "..\build-vs\bin\Release\hdrbridge-cli.exe"),
    [string]$Corpus = (Join-Path $PSScriptRoot "..\..\private-fixtures\golden-inputs"),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "..\..\private-fixtures\test-output\metadata-current")
)

$ErrorActionPreference = "Stop"

function Assert-True($condition, [string]$name) {
    if (-not $condition) { throw "Assertion failed: $name" }
}

function Inspect([string]$path) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $text = & $Cli inspect $path 2>$null | Out-String
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $saved
    if ($exitCode -ne 0) { throw "Inspect failed: $path" }
    return ($text | ConvertFrom-Json)
}

function Convert-Pair([string]$source, [string]$firstName, [string]$firstMode,
                      [string]$secondName, [string]$secondMode) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $Cli benchmark-cache $source (Join-Path $OutputDirectory $firstName) $firstMode `
        (Join-Path $OutputDirectory $secondName) $secondMode `
        2> (Join-Path $OutputDirectory "$firstName.log") | Out-Null
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $saved
    if ($exitCode -ne 0) { throw "Conversion failed: $firstName / $secondName" }
}

if (-not (Test-Path -LiteralPath $Cli)) { throw "CLI not found: $Cli" }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$canon = Inspect (Join-Path $Corpus "0U2A0009.HIF")
$nikon = Inspect (Join-Path $Corpus "FGD_9511.HIF")
$p3png = Inspect (Join-Path $Corpus "IMG_20260119_203043_HDR.PNG")
$jxl = Inspect (Join-Path $Corpus "acceptance_pq16.jxl")
$jxr = Inspect (Join-Path $Corpus "acceptance_scrgb-fp16.jxr")
$vivo = Inspect (Join-Path $Corpus "IMG_20260418_170733.jpg")
$xiaomi = Inspect (Join-Path $Corpus "IMG_20260811_190633.jpg")
$appleHeic = Inspect (Join-Path $Corpus "IMG_9506.HEIC")
$appleJpeg = Inspect (Join-Path $Corpus "IMG_9507 2.JPG")
$adobeTiff = Inspect (Join-Path $Corpus "IMG_20260119_203043_HDR.TIF")
Assert-True ($canon.color.transfer -eq 16 -and $canon.bitDepth -eq 10) "Canon source representation"
Assert-True ($nikon.color.transfer -eq 18 -and $nikon.bitDepth -eq 10) "Nikon source representation"
Assert-True ($p3png.color.primaries -eq 12 -and $p3png.metadata.icc -eq "present") "P3/PQ PNG metadata"
Assert-True ($jxl.colorSignalKind -like "CICP equivalent*") "JXL equivalent signaling label"
Assert-True ($jxl.metadata.exif -eq "present" -and $jxl.metadata.xmp -eq "present") "JXL metadata boxes"
Assert-True ($jxr.pixelFormat -eq "GUID_WICPixelFormat64bppRGBAHalf" -and
             $jxr.color.transfer -eq 8 -and $jxr.color.primaries -eq 1) "FP16 JXR linear scRGB source representation"
Assert-True ($vivo.gainMapLayout.channels -eq 3 -and $vivo.metadata.exif -eq "present") "vivo RGB UHDR metadata"
Assert-True ($xiaomi.gainMapLayout.channels -eq 1 -and $xiaomi.metadata.xmp -eq "present") "Xiaomi UHDR metadata"
Assert-True ($appleHeic.gainMapFamily -eq "apple-auxiliary-tmap" -and
             $appleHeic.metadata.exif -eq "present" -and $appleHeic.metadata.xmp -eq "present" -and
             $appleHeic.metadata.icc -eq "present") "Apple HEIC source metadata"
Assert-True ($appleJpeg.gainMapFamily -eq "apple-mpf-iso" -and
             $appleJpeg.metadata.exif -eq "present" -and $appleJpeg.metadata.icc -eq "present") "Apple JPEG source metadata"
Assert-True ($adobeTiff.gainMapFamily -eq "adobe-iso-tiff" -and
             $adobeTiff.metadata.xmp -eq "present" -and $adobeTiff.metadata.icc -eq "present") "Adobe TIFF source metadata"

$adobe = Join-Path $Corpus "IMG_20260119_203043_HDR.avif"
Convert-Pair $adobe "out-uhdr.jpg" "ultrahdr" "out-png.png" "png-pq16"
Convert-Pair $adobe "out-jxl.jxl" "jxl-pq16" "out-avif.avif" "avif-pq10"
Convert-Pair $adobe "out-tiff.tif" "tiff-pq16" "out-jxr.jxr" "jxr-scrgb-fp16"

$uhdrOut = Inspect (Join-Path $OutputDirectory "out-uhdr.jpg")
$pngOut = Inspect (Join-Path $OutputDirectory "out-png.png")
$jxlOut = Inspect (Join-Path $OutputDirectory "out-jxl.jxl")
$avifOut = Inspect (Join-Path $OutputDirectory "out-avif.avif")
$tiffOut = Inspect (Join-Path $OutputDirectory "out-tiff.tif")
$jxrOut = Inspect (Join-Path $OutputDirectory "out-jxr.jxr")
Assert-True ($uhdrOut.metadata.exif -eq "present") "UHDR Exif preservation"
Assert-True ($pngOut.metadata.exif -eq "present" -and $pngOut.metadata.xmp -eq "present") "PNG Exif/XMP preservation"
Assert-True ($jxlOut.metadata.exif -eq "present" -and $jxlOut.metadata.xmp -eq "present") "JXL Exif/XMP preservation"
Assert-True ($avifOut.metadata.exif -eq "present" -and $avifOut.metadata.xmp -eq "present") "AVIF Exif/XMP preservation"
Assert-True ($tiffOut.metadata.exif -eq "absent" -and $tiffOut.metadata.xmp -eq "present") "TIFF current metadata boundary"
Assert-True ($jxrOut.color.transfer -eq 8 -and $jxrOut.metadata.exif -eq "absent") "JXR current metadata boundary"

Write-Host "PASS: source representation, metadata parsing, and supported metadata preservation"
