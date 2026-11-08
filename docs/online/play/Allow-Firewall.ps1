# Lets other players reach this PC for DriveClub online (Windows Firewall, inbound).
# Double-click Allow-Firewall.bat; Windows asks once for admin rights.
# The game (UDP, peer-to-peer races) always; the server (TCP 8080) when driveclubsrv.exe is found.
# Only from the home network and the VPNs people play over (Tailscale 100.64.0.0/10, Hamachi 25.0.0.0/8,
# Radmin 26.0.0.0/8): nobody on the internet reaches the game or the server. Add more with -Allow.
param([string]$Root = $PSScriptRoot, [string[]]$Allow = @())

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Root `"$Root`""
    exit
}

$remote = @('LocalSubnet', '100.64.0.0/10', '25.0.0.0/8', '26.0.0.0/8') + $Allow
$game = Join-Path $Root 'runtime\DriveClubGame.exe'
Get-NetFirewallRule -DisplayName 'DriveClub Online*' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName 'DriveClub Online (game UDP)' -Direction Inbound -Action Allow `
    -Program $game -Protocol UDP -Profile Any -RemoteAddress $remote | Out-Null
Write-Host "Game allowed: $game"

$serverDir = Join-Path $Root 'server'
$cfg = Join-Path $Root 'online.ini'
if (Test-Path $cfg) {
    $line = Get-Content $cfg | Where-Object { $_ -match '^server=(.+)$' } | Select-Object -First 1
    if ($line -match '^server=(.+)$') { $serverDir = $Matches[1].Trim() }
}
if (Test-Path (Join-Path $serverDir 'driveclubsrv.exe')) {
    New-NetFirewallRule -DisplayName 'DriveClub Online (server TCP 8080)' -Direction Inbound -Action Allow `
        -Protocol TCP -LocalPort 8080 -Profile Any -RemoteAddress $remote | Out-Null
    Write-Host 'Server allowed: TCP 8080'
}

# Rules Windows made when asked "allow access?" for these programs let in every address and cannot be narrowed:
# switch them off and allow each program with the same limits instead.
$n = 0
$programs = @{}
foreach ($f in Get-NetFirewallApplicationFilter | Where-Object { $_.Program -match '(driveclubsrv|DriveClubGame)\.exe$' }) {
    $r = $f | Get-NetFirewallRule
    if ($r.Direction -ne 'Inbound' -or $r.Action -ne 'Allow' -or $r.DisplayName -like 'DriveClub Online*') { continue }
    Disable-NetFirewallRule -Name $r.Name
    Write-Host "Switched off: $($r.DisplayName) ($($f.Program))"
    $programs[$f.Program] = $true
}
foreach ($p in $programs.Keys) {
    if ($p -eq $game) { continue }
    $n++
    New-NetFirewallRule -DisplayName "DriveClub Online (program $n)" -Direction Inbound -Action Allow `
        -Program $p -Profile Any -RemoteAddress $remote | Out-Null
}
Start-Sleep 4
