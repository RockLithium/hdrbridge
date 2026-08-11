$ErrorActionPreference = "Continue"

function Check-Command($name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) {
        Write-Host ("[OK] {0} -> {1}" -f $name, $cmd.Source) -ForegroundColor Green
        try { & $name --version 2>$null | Select-Object -First 2 | ForEach-Object { Write-Host "     $_" } } catch {}
        return $true
    }
    Write-Host ("[MISSING] {0}" -f $name) -ForegroundColor Yellow
    return $false
}

Write-Host "HDR converter prerequisite check" -ForegroundColor Cyan
Write-Host "This script does not install or modify anything.`n"

Check-Command git | Out-Null
Check-Command node | Out-Null
Check-Command npm | Out-Null
Check-Command rustc | Out-Null
Check-Command cargo | Out-Null
Check-Command cmake | Out-Null
Check-Command ninja | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vs) {
        Write-Host "[OK] Visual Studio / Build Tools C++ workload: $vs" -ForegroundColor Green
    } else {
        Write-Host "[MISSING] Visual Studio installation found, but the current x64/x86 C++ Build Tools component was not detected." -ForegroundColor Yellow
    }
} else {
    Write-Host "[UNKNOWN] vswhere not found; Visual Studio Build Tools may be missing." -ForegroundColor Yellow
}

$wv = Get-ItemProperty "HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F1E7E0A5-5E64-4B7E-A30B-11A0F1F134F9}" -ErrorAction SilentlyContinue
if ($wv) {
    Write-Host "[OK] WebView2 Runtime registry entry detected." -ForegroundColor Green
} else {
    Write-Host "[INFO] WebView2 registry key was not detected by this simple check. Windows 11 commonly already includes it." -ForegroundColor DarkYellow
}

Write-Host "`nRun from a Developer PowerShell if MSVC is installed but 'cl' is not on PATH."
