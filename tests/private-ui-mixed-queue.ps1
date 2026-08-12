param(
    [string]$Exe = (Join-Path $PSScriptRoot "..\build-vs\bin\Release\hdrbridge.exe"),
    [string]$Camera = (Join-Path $PSScriptRoot "..\..\private-fixtures\golden-inputs")
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName UIAutomationClient

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class HdrBridgeUiNative {
  [DllImport("user32.dll", SetLastError=true)] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW", SetLastError=true)]
  public static extern IntPtr SendMessageText(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr parent, int id);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder text, int count);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr GlobalAlloc(uint flags, UIntPtr bytes);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr GlobalLock(IntPtr memory);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool GlobalUnlock(IntPtr memory);
}
"@

function Assert-True($condition, [string]$name) {
    if (-not $condition) { throw "Assertion failed: $name" }
}

function Get-ControlText([IntPtr]$control) {
    $length = [int][HdrBridgeUiNative]::SendMessage($control, 0x000E, [IntPtr]::Zero, [IntPtr]::Zero)
    $text = New-Object System.Text.StringBuilder ([Math]::Max($length + 1, 2))
    [void][HdrBridgeUiNative]::SendMessageText($control, 0x000D, [IntPtr]$text.Capacity, $text)
    return $text.ToString()
}

function Get-AutomationText([IntPtr]$control) {
    $element = [Windows.Automation.AutomationElement]::FromHandle($control)
    $valuePattern = $null
    if ($element.TryGetCurrentPattern([Windows.Automation.ValuePattern]::Pattern, [ref]$valuePattern)) {
        return $valuePattern.Current.Value
    }
    $textPattern = $null
    if ($element.TryGetCurrentPattern([Windows.Automation.TextPattern]::Pattern, [ref]$textPattern)) {
        return $textPattern.DocumentRange.GetText(-1)
    }
    throw "Activity control exposes neither ValuePattern nor TextPattern"
}

function Send-FileDrop([IntPtr]$window, [string[]]$paths) {
    $payload = [Text.Encoding]::Unicode.GetBytes(($paths -join "`0") + "`0`0")
    $headerSize = 20
    $allocationSize = [UIntPtr]::new([uint64]($headerSize + $payload.Length))
    $memory = [HdrBridgeUiNative]::GlobalAlloc(0x42, $allocationSize)
    if ($memory -eq [IntPtr]::Zero) { throw "GlobalAlloc failed for WM_DROPFILES" }
    $pointer = [HdrBridgeUiNative]::GlobalLock($memory)
    if ($pointer -eq [IntPtr]::Zero) { throw "GlobalLock failed for WM_DROPFILES" }
    [Runtime.InteropServices.Marshal]::WriteInt32($pointer, 0, $headerSize)
    [Runtime.InteropServices.Marshal]::WriteInt32($pointer, 4, 0)
    [Runtime.InteropServices.Marshal]::WriteInt32($pointer, 8, 0)
    [Runtime.InteropServices.Marshal]::WriteInt32($pointer, 12, 0)
    [Runtime.InteropServices.Marshal]::WriteInt32($pointer, 16, 1)
    [Runtime.InteropServices.Marshal]::Copy($payload, 0, [IntPtr]::Add($pointer, $headerSize), $payload.Length)
    [void][HdrBridgeUiNative]::GlobalUnlock($memory)
    if (-not [HdrBridgeUiNative]::PostMessage($window, 0x0233, $memory, [IntPtr]::Zero)) {
        throw "PostMessage(WM_DROPFILES) failed"
    }
}

if (-not (Test-Path -LiteralPath $Exe)) { throw "GUI not found: $Exe" }
if (-not (Test-Path -LiteralPath $Camera)) { Write-Host "SKIPPED: private Camera corpus unavailable"; exit 0 }

$paths = @(
    (Join-Path $Camera "GM_HEIC_Apple.HEIC"),
    (Join-Path $Camera "SDR_HEIC.HEIC"),
    (Join-Path $Camera "corrupt-sample.jpg")
)
foreach ($path in $paths) { Assert-True (Test-Path -LiteralPath $path) "mixed queue input exists: $path" }

$process = Start-Process -FilePath $Exe -PassThru
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 100
        $process.Refresh()
    } until ($process.MainWindowHandle -ne 0 -or [DateTime]::UtcNow -gt $deadline)
    Assert-True ($process.MainWindowHandle -ne 0) "HDR Bridge main window created"
    $window = [IntPtr]$process.MainWindowHandle
    Send-FileDrop $window $paths

    $queue = [HdrBridgeUiNative]::GetDlgItem($window, 130)
    $convert = [HdrBridgeUiNative]::GetDlgItem($window, 109)
    $startAll = [HdrBridgeUiNative]::GetDlgItem($window, 110)
    $status = [HdrBridgeUiNative]::GetDlgItem($window, 114)
    $activity = [HdrBridgeUiNative]::GetDlgItem($window, 116)
    Assert-True ($queue -ne [IntPtr]::Zero -and $convert -ne [IntPtr]::Zero -and
        $startAll -ne [IntPtr]::Zero) "queue and task controls found"
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 100
        $count = [int][HdrBridgeUiNative]::SendMessage($queue, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
    } until ($count -eq 3 -or [DateTime]::UtcNow -gt $deadline)
    Assert-True ($count -eq 3) "three mixed queue items imported"

    [void][HdrBridgeUiNative]::SendMessage($queue, 0x0185, [IntPtr]1, [IntPtr](-1))
    [void][HdrBridgeUiNative]::SendMessage($convert, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    [void][HdrBridgeUiNative]::SendMessage($startAll, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    $deadline = [DateTime]::UtcNow.AddMinutes(3)
    do {
        Start-Sleep -Milliseconds 250
        $statusText = Get-ControlText $status
        $activityText = Get-ControlText $activity
    } until ($activityText -match "BATCH SUMMARY" -or [DateTime]::UtcNow -gt $deadline)
    $activityText = Get-ControlText $activity
    Write-Host "STATUS: $statusText"
    Write-Host "ACTIVITY TAIL: $($activityText.Substring([Math]::Max(0, $activityText.Length - 2000)))"
    Assert-True ($statusText -match "1 succeeded / 1 skipped / 1 failed") "mixed queue final counts"
    Assert-True ($activityText -match "SKIPPED.*No HDR data") "SDR item skipped and logged"
    Assert-True ($activityText -match "FAILED.*JPEG") "corrupt item failed and logged"
    Assert-True ($activityText -match "BATCH SUMMARY\s+1 succeeded / 1 skipped / 1 failed") "batch summary logged"
    Write-Host "PASS: real desktop mixed queue continued after SDR and corrupt inputs"
} finally {
    if (-not $process.HasExited) {
        [void][HdrBridgeUiNative]::PostMessage([IntPtr]$process.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        if (-not $process.WaitForExit(5000)) { $process.Kill() }
    }
}
