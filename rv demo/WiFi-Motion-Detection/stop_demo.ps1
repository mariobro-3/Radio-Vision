<#
.SYNOPSIS
    Stops all ESP32 CSI Demo processes.
#>

Write-Host ""
Write-Host "Stopping ESP32 CSI Demo..." -ForegroundColor Yellow

# Stop Python processes (dashboard)
$pythonProcs = Get-Process python* -ErrorAction SilentlyContinue
if ($pythonProcs) {
    $pythonProcs | Stop-Process -Force -ErrorAction SilentlyContinue
    Write-Host "  Stopped $($pythonProcs.Count) Python process(es)" -ForegroundColor Green
} else {
    Write-Host "  No Python processes running" -ForegroundColor Gray
}

Write-Host ""
Write-Host "Demo stopped." -ForegroundColor Green
