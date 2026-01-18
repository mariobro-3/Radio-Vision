# Run this IMMEDIATELY after connecting to ESP32 WiFi
# Run as Administrator

$interface = "Wi-Fi 2"

Write-Host "Removing any APIPA addresses..." -ForegroundColor Yellow
Remove-NetIPAddress -InterfaceAlias $interface -AddressFamily IPv4 -Confirm:$false -ErrorAction SilentlyContinue

Write-Host "Disabling DHCP..." -ForegroundColor Yellow
Set-NetIPInterface -InterfaceAlias $interface -Dhcp Disabled -AddressFamily IPv4

Write-Host "Setting static IP 192.168.4.2..." -ForegroundColor Green
New-NetIPAddress -InterfaceAlias $interface -IPAddress 192.168.4.2 -PrefixLength 24 -DefaultGateway 192.168.4.1 -ErrorAction SilentlyContinue

Write-Host "`nCurrent IP Configuration:" -ForegroundColor Cyan
Get-NetIPAddress -InterfaceAlias $interface -AddressFamily IPv4 | Format-Table IPAddress, PrefixLength, AddressState

Write-Host "`nDHCP Status:" -ForegroundColor Cyan
Get-NetIPInterface -InterfaceAlias $interface -AddressFamily IPv4 | Format-Table Dhcp, ConnectionState

Write-Host "`nTesting connection to ESP32..." -ForegroundColor Cyan
Test-Connection -ComputerName 192.168.4.1 -Count 2 -ErrorAction SilentlyContinue

Read-Host "`nPress Enter to close"

