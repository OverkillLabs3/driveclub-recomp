# DriveClub online: starts the game against a community server, as host or as guest.
# Put this folder's files next to DriveClub.exe and double-click Play-Online.bat.
#
# online.ini (asked for on the first start, delete it to change):
#   name=<your player name>
#   host=self            you host: the server runs on this PC
#   host=<IP of the host> you join someone (Tailscale 100.x, Hamachi 25.x, Radmin 26.x or LAN)
#   server=<folder of driveclubsrv.exe>   only for host=self, default: .\server
$root = $PSScriptRoot
$cfg = Join-Path $root 'online.ini'

$settings = @{}
if (Test-Path $cfg) {
    Get-Content $cfg | ForEach-Object { if ($_ -match '^(\w+)=(.*)$') { $settings[$Matches[1]] = $Matches[2].Trim() } }
}
while ($settings.name -notmatch '^[A-Za-z0-9_-]{3,16}$') {
    $settings.name = (Read-Host 'Player name (3-16: letters, digits, - or _)').Trim()
}
while ($settings.host -ne 'self' -and $settings.host -notmatch '^\d{1,3}(\.\d{1,3}){3}$') {
    Write-Host 'Are you hosting? Then "self". Otherwise the host''s IP.'
    $settings.host = (Read-Host 'host').Trim()
}
if (-not $settings.server) { $settings.server = Join-Path $root 'server' }
$settings.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" } |
    Set-Content $cfg -Encoding ascii

if ($settings.host -eq 'self') {
    $exe = Join-Path $settings.server 'driveclubsrv.exe'
    if (-not (Get-Process driveclubsrv -ErrorAction SilentlyContinue)) {
        if (-not (Test-Path $exe)) {
            Write-Host "Server not found: $exe (server= in online.ini)" -ForegroundColor Yellow
            Read-Host 'Enter'
            exit 1
        }
        Start-Process $exe -WorkingDirectory $settings.server -WindowStyle Minimized
        Start-Sleep 2
    }
    $server = '127.0.0.1:8080'
    Write-Host ''
    Write-Host 'Server is running. Your IP for other players:' -ForegroundColor Green
    Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notmatch '^(127\.|169\.254\.)' } |
        ForEach-Object {
            $kind = switch -Regex ($_.IPAddress) { '^100\.' { 'Tailscale' } '^25\.' { 'Hamachi' } '^26\.' { 'Radmin' } default { $_.InterfaceAlias } }
            Write-Host "  $($_.IPAddress)  ($kind)" -ForegroundColor Green
        }
    Write-Host ''
    Write-Host 'The game reads its friend list only when it starts: let the others get in first.'
    Read-Host 'Enter = start the game'
} else {
    $server = "$($settings.host):8080"
    try {
        $null = Invoke-WebRequest "http://$server/dccoop/pending/$($settings.name)" -UseBasicParsing -TimeoutSec 5
    } catch {
        Write-Host "Server $server not reachable." -ForegroundColor Yellow
        Write-Host 'VPN on? Host started Play-Online? Firewall rule added? (change IP: delete online.ini)'
        Read-Host 'Enter'
        exit 1
    }
}

# Every online address of the game goes to the community server.
$user = Join-Path $root 'data\user'
New-Item -ItemType Directory -Force $user | Out-Null
@"
{
  "hub.services.otg.scee.com": "http://$server",
  "Aegs.driveclub.online.scee.com": "http://$server",
  "aegs.driveclub.online.scee.com": "http://$server",
  "driveclub.api.wwsga.me": "http://$server",
  "*": "http://$server"
}
"@ | Set-Content (Join-Path $user 'host_overrides.json') -Encoding ascii

# The game's server settings (TSS): slot 0 from this folder, slots 1-6 empty.
$mods = Join-Path $root 'mods'
New-Item -ItemType Directory -Force $mods | Out-Null
if (Test-Path (Join-Path $PSScriptRoot 'override-0.tss')) {
    Copy-Item (Join-Path $PSScriptRoot 'override-0.tss') $mods -Force
}
1..6 | ForEach-Object {
    $f = Join-Path $mods "override-$_.tss"
    if (-not (Test-Path $f)) { '<< Config >>' | Set-Content $f -Encoding ascii }
}

$env:BB_ONLINE = '1'
$env:BB_NP_LOCAL = $settings.name
$env:BB_COOP_SERVER = $server
Start-Process (Join-Path $root 'DriveClub.exe') -WorkingDirectory $root
