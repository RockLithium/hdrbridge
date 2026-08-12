param(
    [string]$Cli = (Join-Path $PSScriptRoot "..\build-vs\bin\Release\hdrbridge-cli.exe"),
    [string]$Fixture = (Join-Path $PSScriptRoot "..\..\private-fixtures\golden-inputs\HLG_HIF.HIF"),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "..\..\private-fixtures\test-output\nikon-hlg-current")
)

$ErrorActionPreference = "Stop"

function Assert-True($condition, [string]$name) {
    if (-not $condition) { throw "Assertion failed: $name" }
}

if (-not (Test-Path -LiteralPath $Fixture)) {
    Write-Host "SKIPPED: private Nikon HLG fixture unavailable"
    exit 0
}
if (-not (Test-Path -LiteralPath $Cli)) { throw "CLI not found: $Cli" }

$expectedHash = "75DBE690E978B64745D7C75EF94545A5FAAA8CD1C35B51AC1B4B98AB1E7E04AF"
Assert-True ((Get-FileHash -LiteralPath $Fixture -Algorithm SHA256).Hash -eq $expectedHash) "Nikon fixture SHA-256"

$inspection = (& $Cli inspect $Fixture | Out-String) | ConvertFrom-Json
Assert-True ($LASTEXITCODE -eq 0) "Nikon inspection"
Assert-True ($inspection.assetKind -eq "direct-hdr") "Direct HDR classification"
Assert-True ($inspection.width -eq 8256 -and $inspection.height -eq 5504) "dimensions"
Assert-True ($inspection.bitDepth -eq 10 -and $inspection.range -eq "full") "10-bit full range"
Assert-True ($inspection.color.primaries -eq 9 -and $inspection.color.transfer -eq 18 -and $inspection.color.matrix -eq 9) "BT.2020 HLG CICP"
Assert-True ($inspection.color.transferName -eq "HLG / BT.2100") "named HLG transfer"

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$cases = @(
    @{ Name = "nikon-pq.png"; Mode = "png-pq16"; Transfer = "pq"; Color = "Rec.2020/PQ full-range cICP 9/16/0/1" },
    @{ Name = "nikon-hlg.png"; Mode = "png-pq16"; Transfer = "hlg"; Color = "Rec.2020/HLG full-range cICP 9/18/0/1" },
    @{ Name = "nikon-pq.jxl"; Mode = "jxl-pq16"; Transfer = "pq"; Color = "Rec.2020/PQ" },
    @{ Name = "nikon-hlg.jxl"; Mode = "jxl-pq16"; Transfer = "hlg"; Color = "Rec.2020/HLG" },
    @{ Name = "nikon-pq.avif"; Mode = "avif-pq10"; Transfer = "pq"; Color = "direct Rec.2020/PQ full-range CICP 9/16/9" },
    @{ Name = "nikon-hlg.avif"; Mode = "avif-pq10"; Transfer = "hlg"; Color = "direct Rec.2020/HLG full-range CICP 9/18/9" },
    @{ Name = "nikon-scrgb-fp16.jxr"; Mode = "jxr-scrgb-fp16"; Transfer = "pq"; Color = "linear scRGB; 1.0 = 80 cd/m2; sRGB/BT.709 primaries" },
    @{ Name = "nikon-ultrahdr.jpg"; Mode = "ultrahdr"; Transfer = "pq"; Color = "Ultra HDR SDR base + ISO 21496-1 gain map" }
)

$summary = @()
foreach ($case in $cases) {
    $output = Join-Path $OutputDirectory $case.Name
    $log = "$output.log"
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $text = & $Cli convert $Fixture $output "--mode=$($case.Mode)" "--transfer=$($case.Transfer)" --overwrite 2> $log | Out-String
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    if ($code -ne 0) { throw "$($case.Name) failed; see $log" }
    $result = $text | ConvertFrom-Json
    Assert-True ($result.success -and $result.verification.passed) "$($case.Name) verified"
    Assert-True ($result.verification.colorEncoding -eq $case.Color) "$($case.Name) transfer signaling"
    Assert-True ($result.verification.hdrDiagnostics.maxChannelNits -gt 300 -and $result.verification.hdrDiagnostics.maxChannelNits -lt 320) "$($case.Name) peak"
    if ($case.Mode -eq "ultrahdr") {
        Assert-True ($result.verification.width -eq 8192 -and $result.verification.height -eq 5461) "Ultra HDR codec-limit resize"
    } else {
        Assert-True ($result.verification.width -eq 8256 -and $result.verification.height -eq 5504) "$($case.Name) dimensions"
    }
    $summary += [pscustomobject]@{
        name = $case.Name
        mode = $case.Mode
        transfer = $case.Transfer
        colorEncoding = $result.verification.colorEncoding
        maxChannelNits = $result.verification.hdrDiagnostics.maxChannelNits
        maxLuminanceNits = $result.verification.hdrDiagnostics.maxLuminanceNits
        transferConversionRmse = $result.verification.hdrDiagnostics.transferConversionRmse
        transferConversionMaxAbsError = $result.verification.hdrDiagnostics.transferConversionMaxAbsError
        sourceToCanonicalRmse = $result.verification.hdrDiagnostics.sourceToCanonicalRmse
        sourceToCanonicalMaxAbsError = $result.verification.hdrDiagnostics.sourceToCanonicalMaxAbsError
        passed = $result.verification.passed
    }
    Write-Host "PASS: $($case.Name)"
}

$summary | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 (Join-Path $OutputDirectory "summary.json")
Write-Host "PASS: Nikon Direct HDR HLG private integration suite"
