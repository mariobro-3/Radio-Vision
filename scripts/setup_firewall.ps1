<#
.SYNOPSIS
    Creates persistent Windows Firewall rules for the WiFi sonar collector UDP port.
.NOTES
    Aligns with Hernandez Thesis p. 189: UDP streaming beats serial when firewall latency is removed.
#>

param(
    [int]$Port = 40123,
    [string]$RulePrefix = "WiFiSonarCollector",
    [string]$Profile = "Private,Domain"
)

function Assert-Admin {
    if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)) {
        Write-Error "Run this script from an elevated PowerShell session."
        exit 1
    }
}

function Remove-IfExists {
    param([string]$Name)
    $existing = Get-NetFirewallRule -DisplayName $Name -ErrorAction SilentlyContinue
    if ($existing) {
        $existing | Remove-NetFirewallRule
        Write-Host "Removed existing rule '$Name'"
    }
}

Assert-Admin

$inboundName = "$RulePrefix UDP Inbound"
$outboundName = "$RulePrefix UDP Outbound"

Remove-IfExists -Name $inboundName
Remove-IfExists -Name $outboundName

New-NetFirewallRule -DisplayName $inboundName `
    -Direction Inbound `
    -Action Allow `
    -Protocol UDP `
    -LocalPort $Port `
    -Profile $Profile `
    -Description "WiFi sonar collector inbound UDP ($Port)"

New-NetFirewallRule -DisplayName $outboundName `
    -Direction Outbound `
    -Action Allow `
    -Protocol UDP `
    -LocalPort $Port `
    -Profile $Profile `
    -Description "WiFi sonar collector outbound UDP ($Port)"

Write-Host "Firewall rules created for UDP port $Port."

