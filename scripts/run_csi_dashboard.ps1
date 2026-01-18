<#
.SYNOPSIS
    Launches the ESP32 CSI motion detection demo with two FeatherS3D boards (AP + Client).

.DESCRIPTION
    This script starts:
    1. The CSI visualizer dashboard (PyQt6) that displays motion detection
    2. Serial monitors for both ESP32 boards
    
    Setup:
    - Board 1 (AP): Flash with active_ap firmware, creates "ESP32-AP" network
    - Board 2 (Client): Flash with active_client firmware, connects to AP and pings
    - Motion between the boards changes WiFi channel state, detected via CSI

.PARAMETER IdfPath
    Root of the ESP-IDF checkout that contains export.ps1.

.PARAMETER ProjectRoot
    Root of the ESP32-CSI-Collection-and-Display project.

.PARAMETER ApPort
    Serial port for the active_ap device (creates the WiFi network).

.PARAMETER ClientPort
    Serial port for the active_client device (connects and pings).

.PARAMETER HostBind
    IP address the visualizer should bind to (default 0.0.0.0).

.PARAMETER HostPort
    UDP port used by the visualizer (default 8848).

.PARAMETER LogLevel
    Logging level passed to the visualizer.
#>
[CmdletBinding()]
param(
    [string]$IdfPath     = "C:\Users\laser\esp-idf",
    [string]$ProjectRoot = "C:\Users\laser\Documents\RV2\rv demo\ESP32-CSI-Collection-and-Display",
    [string]$ApPort      = "COM19",
    [string]$ClientPort  = "COM15",
    [string]$HostBind    = "0.0.0.0",
    [int]   $HostPort    = 8848,
    [ValidateSet("DEBUG", "INFO", "WARNING", "ERROR")]
    [string]$LogLevel    = "INFO"
)

$exportScript = Join-Path $IdfPath "export.ps1"
if (-not (Test-Path $exportScript)) {
    throw "ESP-IDF export script not found at '$exportScript'. Update -IdfPath if ESP-IDF lives elsewhere."
}

$apDir     = Join-Path $ProjectRoot "active_ap"
$clientDir = Join-Path $ProjectRoot "active_client"

foreach ($dir in @($apDir, $clientDir)) {
    if (-not (Test-Path $dir)) {
        throw "Expected directory '$dir' was not found. Update -ProjectRoot or clone the repo."
    }
}

$logsRoot = Join-Path $ProjectRoot "logs\csi_dashboard"
if (-not (Test-Path $logsRoot)) {
    New-Item -ItemType Directory -Path $logsRoot | Out-Null
}
$sessionStamp   = Get-Date -Format "yyyyMMdd-HHmmss"
$sessionLogDir  = Join-Path $logsRoot $sessionStamp
New-Item -ItemType Directory -Path $sessionLogDir | Out-Null

function Start-Terminal {
    param(
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][string]$WorkingDir,
        [Parameter(Mandatory = $true)][string]$CommandBody,
        [string]$LogFile
    )

    $lines = @()
    $lines += "`$host.UI.RawUI.WindowTitle = '$Title'"
    $lines += "Set-Location '$WorkingDir'"
    if ($LogFile) {
        $lines += "Start-Transcript -Path '$LogFile' -Force | Out-Null"
    }
    $lines += ". '$exportScript'"
    $lines += $CommandBody

    $script = [string]::Join([Environment]::NewLine, $lines)

    Start-Process powershell -ArgumentList "-NoExit", "-Command", $script | Out-Null
}

Write-Host ""
Write-Host "=== ESP32 CSI Motion Detection Demo ==="
Write-Host "AP Board:     $ApPort"
Write-Host "Client Board: $ClientPort"
Write-Host "Dashboard:    $HostBind`:$HostPort"
Write-Host ""

Write-Host "Starting CSI visualizer dashboard..."
$visualizerCmd  = "python host_processing_pyqt.py --bind $HostBind --port $HostPort --log-level $LogLevel --detection --target-node 0 --ap-port $ApPort --client-port $ClientPort --udp-port `"`""
$visualizerLog  = Join-Path $sessionLogDir "visualizer.log"
Start-Terminal -Title "CSI Dashboard ($HostPort)" -WorkingDir $apDir -CommandBody $visualizerCmd -LogFile $visualizerLog

Write-Host "Opening active_ap monitor on $ApPort ..."
$apMonitorCmd = "idf.py -p $ApPort monitor"
$apLog        = Join-Path $sessionLogDir "active_ap_$ApPort.log"
Start-Terminal -Title "AP ($ApPort)" -WorkingDir $apDir -CommandBody $apMonitorCmd -LogFile $apLog

Write-Host "Opening active_client monitor on $ClientPort ..."
$clientMonitorCmd = "idf.py -p $ClientPort monitor"
$clientLog        = Join-Path $sessionLogDir "active_client_$ClientPort.log"
Start-Terminal -Title "Client ($ClientPort)" -WorkingDir $clientDir -CommandBody $clientMonitorCmd -LogFile $clientLog

Write-Host ""
Write-Host "=== Demo Running ==="
Write-Host "Log files: $sessionLogDir"
Write-Host ""
Write-Host "Usage:"
Write-Host "  - Move your hand between the two ESP32 boards"
Write-Host "  - Watch the CSI graph change in the dashboard"
Write-Host "  - Motion detection triggers when threshold is crossed"
Write-Host ""
Write-Host "To stop: Close each PowerShell window (Ctrl+] then q for monitors, Ctrl+C for Python)"

