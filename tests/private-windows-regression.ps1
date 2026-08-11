param(
    [string]$Cli = (Join-Path $PSScriptRoot "..\build-vs\bin\Release\hdrbridge-cli.exe"),
    [string]$Fixture = (Join-Path $PSScriptRoot "..\..\private-fixtures\golden-inputs\0U2A0009.HIF"),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "..\..\private-fixtures\test-output")
)

$ErrorActionPreference = "Stop"

function Assert-Equal($actual, $expected, [string]$name) {
    if ($actual -ne $expected) { throw "$name expected '$expected', got '$actual'" }
}

if (-not (Test-Path -LiteralPath $Fixture)) {
    Write-Host "SKIPPED: private fixture unavailable"
    exit 0
}
if (-not (Test-Path -LiteralPath $Cli)) { throw "CLI not found: $Cli" }

$expectedHash = "B72ACD7E4908B8B1EE62942B084299C3A9AB9EBC57351A3D47852EBAD3042B60"
$actualHash = (Get-FileHash -LiteralPath $Fixture -Algorithm SHA256).Hash
Assert-Equal $actualHash $expectedHash "fixture SHA-256"

$inspection = (& $Cli inspect $Fixture | Out-String) | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw "source inspection failed" }
Assert-Equal $inspection.width 6960 "width"
Assert-Equal $inspection.height 4640 "height"
Assert-Equal $inspection.isGrid $true "grid flag"
Assert-Equal $inspection.gridColumns 4 "grid columns"
Assert-Equal $inspection.gridRows 5 "grid rows"
Assert-Equal $inspection.tileWidth 1792 "tile width"
Assert-Equal $inspection.tileHeight 960 "tile height"
Assert-Equal $inspection.profile "HEVC Range Extensions" "HEVC profile"
Assert-Equal $inspection.chroma "4:2:2" "chroma"
Assert-Equal $inspection.bitDepth 10 "source bit depth"
Assert-Equal $inspection.color.primaries 9 "CICP primaries"
Assert-Equal $inspection.color.transfer 16 "CICP transfer"
Assert-Equal $inspection.color.matrix 9 "CICP matrix"
Assert-Equal $inspection.range "full" "range"
Assert-Equal $inspection.exifPresent $true "Exif"
Assert-Equal $inspection.xmpPresent $true "XMP"

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$cases = @(
    @{ Mode = "jxl-pq16"; Name = "acceptance_pq16.jxl" },
    @{ Mode = "jxr-scrgb-fp16"; Name = "acceptance_scrgb-fp16.jxr" },
    @{ Mode = "ultrahdr"; Name = "acceptance_ultrahdr.jpg" },
    @{ Mode = "jxr-rgb10-experimental"; Name = "acceptance_rgb10-experimental.jxr" }
)

$summary = @()
foreach ($case in $cases) {
    $output = Join-Path $OutputDirectory $case.Name
    $stderr = Join-Path $OutputDirectory ($case.Name + ".log")
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $jsonText = & $Cli convert $Fixture $output "--mode=$($case.Mode)" --overwrite 2> $stderr | Out-String
    $nativeExitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedPreference
    if ($nativeExitCode -ne 0) { throw "$($case.Mode) conversion failed; see $stderr" }
    $result = $jsonText | ConvertFrom-Json
    Assert-Equal $result.success $true "$($case.Mode) success"
    Assert-Equal $result.verification.passed $true "$($case.Mode) verification"
    Assert-Equal $result.verification.width 6960 "$($case.Mode) width"
    Assert-Equal $result.verification.height 4640 "$($case.Mode) height"
    if ($case.Mode -eq "jxl-pq16") {
        Assert-Equal $result.verification.bitDepth 16 "JXL bit depth"
        Assert-Equal $result.verification.colorEncoding "Rec.2020/PQ" "JXL color encoding"
        Assert-Equal $result.verification.exactRoundtrip $true "JXL exact roundtrip"
    }
    if ($case.Mode -eq "jxr-scrgb-fp16") {
        Assert-Equal $result.verification.pixelFormat "GUID_WICPixelFormat64bppRGBAHalf" "JXR FP16 pixel format"
        Assert-Equal $result.verification.exactRoundtrip $true "JXR FP16 exact roundtrip"
    }
    if ($case.Mode -eq "jxr-rgb10-experimental") {
        Assert-Equal $result.verification.pixelFormat "GUID_WICPixelFormat32bppBGR101010" "JXR RGB10 pixel format"
        Assert-Equal $result.verification.exactRoundtrip $true "JXR RGB10 exact roundtrip"
    }
    $summary += [pscustomobject]@{
        mode = $case.Mode
        path = $output
        bytes = $result.outputBytes
        sha256 = $result.sha256
        passed = $result.verification.passed
    }
    Write-Host "PASS: $($case.Mode)"
}

$summary | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $OutputDirectory "summary.json")
Write-Host "PASS: private Windows integration suite"
