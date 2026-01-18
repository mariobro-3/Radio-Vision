# Fix ESP32-AP Network Profile to use Static IP
# Run as Administrator

$interfaceName = "Wi-Fi 2"
$networkName = "ESP32-AP"
$ipAddress = "192.168.4.2"
$gateway = "192.168.4.1"
$subnet = "255.255.255.0"

Write-Host "=== Fixing ESP32-AP Network Profile ===" -ForegroundColor Cyan

# Step 1: Clear global adapter settings (this is what's causing the conflict)
Write-Host ""
Write-Host "1. Clearing global adapter IP settings..." -ForegroundColor Yellow
Set-NetIPInterface -InterfaceAlias $interfaceName -Dhcp Enabled -AddressFamily IPv4 -ErrorAction SilentlyContinue

# Step 2: Remove any existing static IPs on the adapter
Write-Host "2. Removing existing IP addresses..." -ForegroundColor Yellow
Get-NetIPAddress -InterfaceAlias $interfaceName -AddressFamily IPv4 -ErrorAction SilentlyContinue | Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue

# Step 3: Use netsh to set IP specifically for this network profile (persistent)
Write-Host "3. Setting static IP for ESP32-AP network profile..." -ForegroundColor Green
netsh interface ipv4 set address name="$interfaceName" source=static addr=$ipAddress mask=$subnet gateway=$gateway store=persistent

# Step 4: Disable DHCP for this adapter
Write-Host "4. Disabling DHCP..." -ForegroundColor Yellow
Set-NetIPInterface -InterfaceAlias $interfaceName -Dhcp Disabled -AddressFamily IPv4

# Step 5: Set DNS
Write-Host "5. Setting DNS servers..." -ForegroundColor Yellow
Set-DnsClientServerAddress -InterfaceAlias $interfaceName -ServerAddresses "8.8.8.8","8.8.4.4"

Write-Host ""
Write-Host "=== Verification ===" -ForegroundColor Green
Write-Host ""
Write-Host "Current IP Configuration:" -ForegroundColor Cyan
ipconfig | Select-String -Pattern "Wi-Fi 2" -Context 0,6

Write-Host ""
Write-Host "Configuration complete!" -ForegroundColor Green
Write-Host ""
Write-Host "Now test by disconnecting and reconnecting to ESP32-AP." -ForegroundColor Yellow
Write-Host "The IP should stay at 192.168.4.2" -ForegroundColor Yellow

Write-Host ""
Read-Host "Press Enter to close"
