# Disable Windows Network Connectivity Check for ESP32-AP
# Run as Administrator

Write-Host "=== Disabling Network Connectivity Checks ===" -ForegroundColor Cyan

# Option 1: Disable NCSI (Network Connectivity Status Indicator) completely
Write-Host "`n1. Disabling Windows Network Connectivity Status Indicator (NCSI)..." -ForegroundColor Yellow
$ncsiPath = "HKLM:\SYSTEM\CurrentControlSet\Services\NlaSvc\Parameters\Internet"
if (-not (Test-Path $ncsiPath)) {
    New-Item -Path $ncsiPath -Force | Out-Null
}
Set-ItemProperty -Path $ncsiPath -Name "EnableActiveProbing" -Value 0 -Type DWord

# Option 2: Set ESP32-AP network to Private (faster, no checks)
Write-Host "2. Setting ESP32-AP network profile to Private..." -ForegroundColor Yellow
try {
    $profile = Get-NetConnectionProfile -InterfaceAlias "Wi-Fi 2" -ErrorAction SilentlyContinue
    if ($profile) {
        Set-NetConnectionProfile -InterfaceAlias "Wi-Fi 2" -NetworkCategory Private
        Write-Host "   Network set to Private" -ForegroundColor Green
    } else {
        Write-Host "   Wi-Fi 2 not currently connected" -ForegroundColor Yellow
    }
} catch {
    Write-Host "   Could not set network category: $_" -ForegroundColor Red
}

# Option 3: Disable "Identify Network" for all networks
Write-Host "3. Disabling 'Identify' checks for new networks..." -ForegroundColor Yellow
$nlaSvcPath = "HKLM:\SYSTEM\CurrentControlSet\Services\NlaSvc\Parameters"
Set-ItemProperty -Path $nlaSvcPath -Name "ShowDomainEndpointInterfaces" -Value 0 -Type DWord -ErrorAction SilentlyContinue

Write-Host "`n=== Configuration Complete ===" -ForegroundColor Green
Write-Host "`nYou may need to:" -ForegroundColor Yellow
Write-Host "  1. Disconnect and reconnect to ESP32-AP" -ForegroundColor Cyan
Write-Host "  2. Restart the computer for full effect" -ForegroundColor Cyan
Write-Host "`nThe connection should be instant next time!" -ForegroundColor Green

Read-Host "`nPress Enter to close"




