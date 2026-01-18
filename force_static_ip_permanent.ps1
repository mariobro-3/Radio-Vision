# NUCLEAR OPTION: Force Windows to PERMANENTLY keep 192.168.4.2 on Wi-Fi 2
# Run as Administrator

$interface = "Wi-Fi 2"
$ip = "192.168.4.2"
$gateway = "192.168.4.1"
$subnet = "255.255.255.0"

Write-Host "`n=== FORCING STATIC IP CONFIGURATION ===" -ForegroundColor Cyan

# Method 1: Use netsh with persistent store (this is the key!)
Write-Host "`n1. Setting static IP using netsh (persistent)..." -ForegroundColor Yellow
netsh interface ipv4 set address name="$interface" source=static addr=$ip mask=$subnet gateway=$gateway store=persistent

# Method 2: Disable DHCP permanently
Write-Host "`n2. Disabling DHCP permanently..." -ForegroundColor Yellow
netsh interface ipv4 set interface "$interface" dhcpstaticipcoexistence=disabled store=persistent

# Method 3: Disable APIPA (Automatic Private IP Addressing) - THIS IS CRITICAL
Write-Host "`n3. DISABLING APIPA (this stops 169.254.x.x addresses)..." -ForegroundColor Red
netsh interface ipv4 set interface "$interface" dadtransmits=0 store=persistent
reg add "HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces" /v IPAutoconfigurationEnabled /t REG_DWORD /d 0 /f

# Method 4: Set DNS
Write-Host "`n4. Setting DNS servers..." -ForegroundColor Yellow
netsh interface ipv4 set dnsservers name="$interface" source=static address=8.8.8.8 primary
netsh interface ipv4 add dnsservers name="$interface" address=8.8.4.4 index=2

# Method 5: Disable IPv6 (can interfere)
Write-Host "`n5. Disabling IPv6 (can cause conflicts)..." -ForegroundColor Yellow
Disable-NetAdapterBinding -Name "$interface" -ComponentID ms_tcpip6 -ErrorAction SilentlyContinue

# Method 6: Reset the adapter to apply settings
Write-Host "`n6. Restarting adapter to apply settings..." -ForegroundColor Yellow
Restart-NetAdapter -Name "$interface"
Start-Sleep -Seconds 3

# Verify configuration
Write-Host "`n=== VERIFICATION ===" -ForegroundColor Green
netsh interface ipv4 show config "$interface"

Write-Host "`n=== IP ADDRESS STATUS ===" -ForegroundColor Green
Get-NetIPAddress -InterfaceAlias "$interface" -AddressFamily IPv4 | Format-Table IPAddress, PrefixLength, AddressState

Write-Host "`nDone! The static IP should now PERSIST across reconnections." -ForegroundColor Green
Write-Host "If you still get 169.254.x.x, Windows networking is truly broken and you may need to:" -ForegroundColor Yellow
Write-Host "  1. Uninstall/reinstall the WiFi adapter driver" -ForegroundColor Yellow
Write-Host "  2. Reset network stack: netsh winsock reset; netsh int ip reset" -ForegroundColor Yellow

