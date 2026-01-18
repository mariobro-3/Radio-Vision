<#
.SYNOPSIS
    Unified startup script for ESP32 CSI Motion Detection Demo.
    
.DESCRIPTION
    This script properly sequences the startup of all components:
    1. Stops any existing dashboard processes
    2. Resets the AP device first (creates WiFi network)
    3. Waits for AP to initialize
    4. Resets client devices (connect to AP)
    5. Waits for clients to connect
    6. Starts the web dashboard
    
.PARAMETER ApPort
    Serial port for the AP device (default: COM19)
    
.PARAMETER ClientPorts
    Array of serial ports for client devices (default: COM15, COM17)
    
.PARAMETER DashboardOnly
    Only restart the dashboard, don't reset devices
    
.PARAMETER ResetOnly
    Only reset devices, don't start dashboard
#>
[CmdletBinding()]
param(
    [string]$ApPort = "COM19",
    [string[]]$ClientPorts = @("COM15", "COM17"),
    [switch]$DashboardOnly,
    [switch]$ResetOnly
)

$ErrorActionPreference = "Continue"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  ESP32 CSI Motion Detection Demo" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Configuration:" -ForegroundColor Yellow
Write-Host "  AP Port:      $ApPort"
Write-Host "  Client Ports: $($ClientPorts -join ', ')"
Write-Host ""

# Function to reset a device via serial DTR toggle
function Reset-ESP32 {
    param([string]$Port)
    
    try {
        $serial = New-Object System.IO.Ports.SerialPort $Port, 115200
        $serial.DtrEnable = $false
        $serial.RtsEnable = $false
        $serial.Open()
        
        # Toggle DTR to reset
        $serial.DtrEnable = $true
        Start-Sleep -Milliseconds 100
        $serial.DtrEnable = $false
        Start-Sleep -Milliseconds 100
        $serial.DtrEnable = $true
        
        $serial.Close()
        return $true
    }
    catch {
        Write-Host "  Warning: Could not reset $Port - $($_.Exception.Message)" -ForegroundColor Yellow
        return $false
    }
}

# Step 1: Stop existing processes
Write-Host "Step 1: Stopping existing processes..." -ForegroundColor Green
$pythonProcs = Get-Process python* -ErrorAction SilentlyContinue
if ($pythonProcs) {
    $pythonProcs | Stop-Process -Force -ErrorAction SilentlyContinue
    Write-Host "  Stopped $($pythonProcs.Count) Python process(es)"
    Start-Sleep -Seconds 1
} else {
    Write-Host "  No Python processes running"
}

if ($DashboardOnly) {
    Write-Host ""
    Write-Host "Skipping device reset (DashboardOnly mode)" -ForegroundColor Yellow
} else {
    # Step 2: Reset the AP first
    Write-Host ""
    Write-Host "Step 2: Resetting AP ($ApPort)..." -ForegroundColor Green
    if (Reset-ESP32 -Port $ApPort) {
        Write-Host "  AP reset successful" -ForegroundColor Green
    }
    
    # Step 3: Wait for AP to initialize WiFi
    Write-Host ""
    Write-Host "Step 3: Waiting for AP to initialize (3 seconds)..." -ForegroundColor Green
    Start-Sleep -Seconds 3
    Write-Host "  AP should be broadcasting 'ESP32-AP' network"
    
    # Step 4: Reset clients
    Write-Host ""
    Write-Host "Step 4: Resetting clients..." -ForegroundColor Green
    foreach ($clientPort in $ClientPorts) {
        if (Reset-ESP32 -Port $clientPort) {
            Write-Host "  $clientPort reset successful" -ForegroundColor Green
        }
    }
    
    # Step 5: Wait for clients to connect
    Write-Host ""
    Write-Host "Step 5: Waiting for clients to connect (2 seconds)..." -ForegroundColor Green
    Start-Sleep -Seconds 2
    Write-Host "  Clients should now be connected and pinging"
    
    # Step 6: Wait for calibration
    Write-Host ""
    Write-Host "Step 6: Waiting for baseline calibration (3 seconds)..." -ForegroundColor Green
    Write-Host "  KEEP THE AREA STILL during calibration!" -ForegroundColor Yellow
    Start-Sleep -Seconds 3
}

if ($ResetOnly) {
    Write-Host ""
    Write-Host "Skipping dashboard start (ResetOnly mode)" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "  Devices Reset Complete!" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Cyan
    exit 0
}

# Step 7: Start dashboard
Write-Host ""
Write-Host "Step 7: Starting web dashboard..." -ForegroundColor Green

# Check if PC is connected to ESP32-AP
$esp32Interface = Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -like "192.168.4.*" }
if ($esp32Interface) {
    Write-Host "  PC connected to ESP32-AP network: $($esp32Interface.IPAddress)" -ForegroundColor Green
} else {
    Write-Host "  WARNING: PC not connected to ESP32-AP network!" -ForegroundColor Red
    Write-Host "  Connect to 'ESP32-AP' WiFi (password: esp32-ap) for dashboard to receive data" -ForegroundColor Yellow
}

# Start dashboard in background
$dashboardPath = Join-Path $ScriptDir "active_ap\web_dashboard.py"
if (-not (Test-Path $dashboardPath)) {
    $dashboardPath = Join-Path $ScriptDir "web_dashboard.py"
}

if (Test-Path $dashboardPath) {
    $process = Start-Process python -ArgumentList $dashboardPath -PassThru -WindowStyle Normal
    Write-Host "  Dashboard started (PID: $($process.Id))"
} else {
    Write-Host "  ERROR: Could not find web_dashboard.py" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Demo Started Successfully!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Dashboard: http://localhost:8080" -ForegroundColor Cyan
Write-Host ""
Write-Host "LED Colors:" -ForegroundColor Yellow
Write-Host "  Red    = Waiting for clients"
Write-Host "  Yellow = Calibrating (keep still!)"
Write-Host "  Blue   = Ready, no motion"
Write-Host "  Green  = Motion detected"
Write-Host ""
Write-Host "To stop: Close this window or press Ctrl+C" -ForegroundColor Gray
Write-Host ""

# Keep script running to show it's active
Write-Host "Press any key to stop the demo..." -ForegroundColor Gray
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")

# Cleanup
Write-Host ""
Write-Host "Stopping dashboard..." -ForegroundColor Yellow
Get-Process python* -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Write-Host "Done." -ForegroundColor Green
