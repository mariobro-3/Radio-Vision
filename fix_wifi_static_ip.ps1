# Fix Wi-Fi 2 Static IP Configuration
# Run this script as Administrator

Write-Host "Removing all IP addresses from Wi-Fi 2..." -ForegroundColor Yellow
Remove-NetIPAddress -InterfaceAlias "Wi-Fi 2" -Confirm:$false -ErrorAction SilentlyContinue

Write-Host "Removing all default gateways from Wi-Fi 2..." -ForegroundColor Yellow
Remove-NetRoute -InterfaceAlias "Wi-Fi 2" -Confirm:$false -ErrorAction SilentlyContinue

Write-Host "Setting static IP 192.168.4.2 with subnet mask 255.255.255.0..." -ForegroundColor Green
New-NetIPAddress -InterfaceAlias "Wi-Fi 2" -IPAddress 192.168.4.2 -PrefixLength 24 -DefaultGateway 192.168.4.1

Write-Host "Disabling DHCP on Wi-Fi 2..." -ForegroundColor Green
Set-NetIPInterface -InterfaceAlias "Wi-Fi 2" -Dhcp Disabled

Write-Host "Setting DNS (optional - using Google DNS)..." -ForegroundColor Cyan
Set-DnsClientServerAddress -InterfaceAlias "Wi-Fi 2" -ServerAddresses "8.8.8.8","8.8.4.4"

Write-Host "`nConfiguration complete! Checking status..." -ForegroundColor Green
Get-NetIPAddress -InterfaceAlias "Wi-Fi 2" -AddressFamily IPv4 | Format-Table IPAddress, PrefixLength
Get-NetIPInterface -InterfaceAlias "Wi-Fi 2" | Format-Table InterfaceAlias, Dhcp, ConnectionState

Write-Host "`nIf the APIPA address (169.254.x.x) comes back, run these additional commands:" -ForegroundColor Yellow
Write-Host "netsh interface ipv4 set interface `"Wi-Fi 2`" dadtransmits=0 store=persistent" -ForegroundColor Cyan
Write-Host "Disable-NetAdapterBinding -Name `"Wi-Fi 2`" -ComponentID ms_tcpip6" -ForegroundColor Cyan

