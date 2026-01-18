@echo off
echo Fixing Wi-Fi 2 static IP to 192.168.4.2...
powershell -Command "Start-Process powershell -Verb RunAs -ArgumentList '-Command', 'Remove-NetIPAddress -InterfaceAlias ''Wi-Fi 2'' -AddressFamily IPv4 -Confirm:$false; Start-Sleep 1; New-NetIPAddress -InterfaceAlias ''Wi-Fi 2'' -IPAddress 192.168.4.2 -PrefixLength 24 -DefaultGateway 192.168.4.1; Write-Host ''Done! IP set to 192.168.4.2'' -ForegroundColor Green; ipconfig /all; timeout 10'"

