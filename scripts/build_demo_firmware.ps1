<#
.SYNOPSIS
    Builds and flashes the CSI demo firmware to both FeatherS3D boards.

.DESCRIPTION
    This script builds the active_ap and active_client firmware projects
    and flashes them to the specified COM ports.

.PARAMETER IdfPath
    Root of the ESP-IDF checkout that contains export.ps1.

.PARAMETER ProjectRoot
    Root of the ESP32-CSI-Collection-and-Display project.

.PARAMETER ApPort
    Serial port for the active_ap device.

.PARAMETER ClientPort
    Serial port for the active_client device.

.PARAMETER SkipBuild
    Skip the build step and only flash.

.PARAMETER BuildOnly
    Only build, don't flash.
#>
[CmdletBinding()]
param(
    [string]$IdfPath     = "C:\Users\laser\esp-idf",
    [string]$ProjectRoot = "C:\Users\laser\Documents\RV2\rv demo\ESP32-CSI-Collection-and-Display",
    [string]$ApPort      = "COM19",
    [string]$ClientPort  = "COM15",
    [switch]$SkipBuild,
    [switch]$BuildOnly
)

$ErrorActionPreference = "Stop"

$exportScript = Join-Path $IdfPath "export.ps1"
if (-not (Test-Path $exportScript)) {
    throw "ESP-IDF export script not found at '$exportScript'. Update -IdfPath."
}

$apDir     = Join-Path $ProjectRoot "active_ap"
$clientDir = Join-Path $ProjectRoot "active_client"

Write-Host ""
Write-Host "=== ESP32 CSI Demo Firmware Builder ===" -ForegroundColor Cyan
Write-Host ""

# Source ESP-IDF environment
Write-Host "Loading ESP-IDF environment..." -ForegroundColor Yellow
. $exportScript

function Build-Project {
    param([string]$Name, [string]$Dir)
    
    Write-Host ""
    Write-Host "Building $Name..." -ForegroundColor Yellow
    Push-Location $Dir
    try {
        # Clean old sdkconfig to apply defaults
        if (Test-Path "sdkconfig") {
            Write-Host "  Removing old sdkconfig to apply sdkconfig.defaults..."
            Remove-Item "sdkconfig" -Force
        }
        
        # Set target and build
        idf.py set-target esp32s3
        if ($LASTEXITCODE -ne 0) { throw "set-target failed" }
        
        idf.py build
        if ($LASTEXITCODE -ne 0) { throw "build failed" }
        
        Write-Host "  $Name build complete!" -ForegroundColor Green
    }
    finally {
        Pop-Location
    }
}

function Flash-Project {
    param([string]$Name, [string]$Dir, [string]$Port)
    
    Write-Host ""
    Write-Host "Flashing $Name to $Port..." -ForegroundColor Yellow
    Push-Location $Dir
    try {
        idf.py -p $Port flash
        if ($LASTEXITCODE -ne 0) { throw "flash failed" }
        
        Write-Host "  $Name flashed to $Port!" -ForegroundColor Green
    }
    finally {
        Pop-Location
    }
}

# Build phase
if (-not $SkipBuild) {
    Build-Project -Name "active_ap" -Dir $apDir
    Build-Project -Name "active_client" -Dir $clientDir
}

# Flash phase
if (-not $BuildOnly) {
    Write-Host ""
    Write-Host "Ready to flash. Make sure both boards are connected:" -ForegroundColor Yellow
    Write-Host "  AP Board:     $ApPort"
    Write-Host "  Client Board: $ClientPort"
    Write-Host ""
    
    $confirm = Read-Host "Press Enter to flash, or 'n' to skip"
    if ($confirm -ne 'n') {
        Flash-Project -Name "active_ap" -Dir $apDir -Port $ApPort
        Flash-Project -Name "active_client" -Dir $clientDir -Port $ClientPort
    }
}

Write-Host ""
Write-Host "=== Done ===" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Run: .\scripts\run_csi_dashboard.ps1 -ApPort $ApPort -ClientPort $ClientPort"
Write-Host "  2. Move your hand between the two ESP32 boards"
Write-Host "  3. Watch the CSI waveform change in the dashboard"
Write-Host ""
