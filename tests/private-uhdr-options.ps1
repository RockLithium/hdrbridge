param(
    [string]$Cli = (Join-Path $PSScriptRoot "..\build-vs\bin\Release\hdrbridge-cli.exe"),
    [string]$Corpus = (Join-Path $PSScriptRoot "..\..\private-fixtures\golden-inputs"),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "..\..\private-fixtures\test-output\uhdr-options-current")
)

$ErrorActionPreference = "Stop"

function Assert-True($condition, [string]$name) {
    if (-not $condition) { throw "Assertion failed: $name" }
}

if (-not (Test-Path -LiteralPath $Cli)) { throw "CLI not found: $Cli" }
$vivo = Join-Path $Corpus "IMG_20260418_170733.jpg"
$xiaomi = Join-Path $Corpus "IMG_20260811_190633.jpg"
if (-not (Test-Path -LiteralPath $vivo)) {
    Write-Host "SKIPPED: private vivo Ultra HDR fixture unavailable"
    exit 0
}
Assert-True ((Get-FileHash -LiteralPath $vivo -Algorithm SHA256).Hash -eq
    "DAC4BDED78E0370C7A0DCE487E6F74239999307B7EDB895E6181CB3F8A8DDB41") "vivo fixture SHA-256"

$vivoInfo = (& $Cli inspect $vivo | Out-String) | ConvertFrom-Json
Assert-True ($vivoInfo.gainMapFamily -eq "iso-ultrahdr-jpeg") "vivo UHDR family"
Assert-True ($vivoInfo.baseRendition.width -eq 3072 -and $vivoInfo.baseRendition.height -eq 4096 -and $vivoInfo.baseRendition.bitDepth -eq 8) "vivo base rendition"
Assert-True ($vivoInfo.gainMapSize.width -eq 1536 -and $vivoInfo.gainMapSize.height -eq 2048) "vivo gain-map dimensions"
Assert-True ($vivoInfo.gainMapLayout.channels -eq 3) "vivo RGB gain map"
Assert-True ($vivoInfo.gainMapLayout.scaleX -eq 2 -and $vivoInfo.gainMapLayout.scaleY -eq 2) "vivo gain-map scale"
Assert-True $vivoInfo.xmpPresent "vivo XMP marker"

$xiaomiInfo = (& $Cli inspect $xiaomi | Out-String) | ConvertFrom-Json
Assert-True ($xiaomiInfo.gainMapFamily -eq "iso-ultrahdr-jpeg" -and $xiaomiInfo.gainMapLayout.channels -eq 1) "Xiaomi mono UHDR inspector"

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$cases = @(
    @{ Name = "default-half-mono"; Scale = 2; Channels = 1; Flags = @() },
    @{ Name = "quarter-mono"; Scale = 4; Channels = 1; Flags = @("--gainmap-scale=4", "--single-channel-gainmap") },
    @{ Name = "full-mono"; Scale = 1; Channels = 1; Flags = @("--gainmap-scale=1", "--single-channel-gainmap") },
    @{ Name = "quarter-rgb"; Scale = 4; Channels = 3; Flags = @("--gainmap-scale=4", "--rgb-gainmap") },
    @{ Name = "half-rgb"; Scale = 2; Channels = 3; Flags = @("--gainmap-scale=2", "--rgb-gainmap") },
    @{ Name = "full-rgb"; Scale = 1; Channels = 3; Flags = @("--gainmap-scale=1", "--rgb-gainmap") }
)
$summary = @()
foreach ($case in $cases) {
    $output = Join-Path $OutputDirectory "$($case.Name).jpg"
    $arguments = @("convert", $vivo, $output, "--mode=ultrahdr", "--overwrite") + $case.Flags
    $savedErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $text = & $Cli @arguments 2> "$output.log" | Out-String
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorActionPreference
    if ($exitCode -ne 0) { throw "$($case.Name) conversion failed; see $output.log" }
    $result = $text | ConvertFrom-Json
    $diagnostics = $result.verification.hdrDiagnostics
    Assert-True ($result.success -and $result.verification.passed) "$($case.Name) verified"
    Assert-True ($diagnostics.gainMapWidth -eq [math]::Floor(3072 / $case.Scale) -and
                 $diagnostics.gainMapHeight -eq [math]::Floor(4096 / $case.Scale)) "$($case.Name) map resolution"
    Assert-True ($diagnostics.gainMapChannels -eq $case.Channels) "$($case.Name) channel count"
    if ($case.Channels -eq 3) {
        Assert-True ($diagnostics.gainMapChannelDifferenceMax -gt 0.01) "$($case.Name) real per-channel gain data"
    }
    $summary += [pscustomobject]@{
        name = $case.Name
        mapWidth = $diagnostics.gainMapWidth
        mapHeight = $diagnostics.gainMapHeight
        channels = $diagnostics.gainMapChannels
        channelDifferenceMax = $diagnostics.gainMapChannelDifferenceMax
        reconstructionRmse = $diagnostics.reconstructionRmse
        passed = $result.verification.passed
    }
    Write-Host "PASS: $($case.Name)"
}
$summary | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 (Join-Path $OutputDirectory "summary.json")
Write-Host "PASS: UHDR resolution/channel options and vivo/Xiaomi inspector"
