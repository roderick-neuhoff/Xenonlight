# One-time setup for XeLL network boot over a direct PC <-> Xbox Ethernet cable.
# Run in an elevated PowerShell:  powershell -ExecutionPolicy Bypass -File scripts\netboot-setup.ps1
#
# - Gives the Ethernet adapter the static IP XeLL falls back to looking for (192.168.1.98/24).
# - Allows inbound UDP for Python so TFTP requests and ACKs get through the firewall.
# Undo:  netsh interface ipv4 set address name="Ethernet 2" source=dhcp
#        Remove-NetFirewallRule -DisplayName "XeLL TFTP (Python)"
param(
    [string]$Adapter = "Ethernet 2",
    [string]$IP = "192.168.1.98"
)

$ErrorActionPreference = "Stop"

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this from an elevated (Administrator) PowerShell."
}

# Static IP on the wired adapter (no gateway: this link only reaches the Xbox).
# netsh, unlike New-NetIPAddress, also works while the cable is unplugged.
netsh interface ipv4 set address name="$Adapter" source=static address=$IP mask=255.255.255.0
if ($LASTEXITCODE -ne 0) { throw "netsh failed to set $IP on '$Adapter'" }
Write-Host "Set $Adapter to $IP/24"

# Firewall: the TFTP data transfer uses random ports, so allow the Python interpreter itself.
$python = (Get-Command python).Source
Get-NetFirewallRule -DisplayName "XeLL TFTP (Python)" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName "XeLL TFTP (Python)" -Direction Inbound -Protocol UDP `
    -Program $python -Action Allow -Profile Any | Out-Null
Write-Host "Firewall: allowed inbound UDP for $python"
Write-Host "Done. Plug the cable into the Xbox, then run: python scripts\tftp_server.py"
