param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\..\setup\Install-Launcher.ps1') -LibraryOnly
$root=Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('setup-'+[guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $root
$script:checks=0
function Check([bool]$Ok,[string]$Name) { if (!$Ok) { throw $Name }; $script:checks++; Write-Output ('PASS '+$Name) }
function Reject([scriptblock]$Action,[string]$Name) { $failed=$false; try { & $Action } catch { $failed=$true }; Check $failed $Name }
function Fixture([string]$Path,[uint16]$Machine=0x8664) {
    $null=New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path)
    $bytes=[byte[]]::new(160);$bytes[0]=0x4d;$bytes[1]=0x5a
    [BitConverter]::GetBytes([int]128).CopyTo($bytes,60);[BitConverter]::GetBytes([uint32]0x4550).CopyTo($bytes,128)
    [BitConverter]::GetBytes($Machine).CopyTo($bytes,132);[IO.File]::WriteAllBytes($Path,$bytes)
}
$app=Join-Path $root 'app';$null=New-Item -ItemType Directory -Path $app
$runtime=Join-Path $root 'runtime'
Check (!(Get-SetupRuntimeState -Directory $app -Roots @($runtime)).Available) 'missing runtime is detected without network'
Fixture (Join-Path $runtime 'shared\Microsoft.NETCore.App\8.0.31\coreclr.dll')
Check (!(Get-SetupRuntimeState -Directory $app -Roots @($runtime)).Available) 'Core runtime alone does not satisfy WPF'
Fixture (Join-Path $runtime 'shared\Microsoft.WindowsDesktop.App\8.0.31\PresentationNative_cor3.dll') 0x014c
Check (!(Get-SetupRuntimeState -Directory $app -Roots @($runtime)).Available) 'x86 Desktop runtime is rejected'
Fixture (Join-Path $runtime 'shared\Microsoft.WindowsDesktop.App\8.0.31\PresentationNative_cor3.dll')
Check ((Get-SetupRuntimeState -Directory $app -Roots @($runtime)).Available) 'matching x64 Core and Desktop 8 are accepted'
Fixture (Join-Path $runtime 'shared\Microsoft.WindowsDesktop.App\8.0.32\PresentationNative_cor3.dll')
Check (!(Get-SetupRuntimeState -Directory $app -Roots @($runtime)).Available) 'Desktop newer than available Core is rejected'
Fixture (Join-Path $runtime 'shared\Microsoft.NETCore.App\8.0.32\coreclr.dll')
Check ((Get-SetupRuntimeState -Directory $app -Roots @($runtime)).Available) 'compatible newer Core patch is accepted'
Fixture (Join-Path $app 'coreclr.dll');Fixture (Join-Path $app 'PresentationNative_cor3.dll')
Fixture (Join-Path $app 'hostfxr.dll');Fixture (Join-Path $app 'hostpolicy.dll')
[IO.File]::WriteAllText((Join-Path $app 'DriveClub.Launcher.runtimeconfig.json'),'{"runtimeOptions":{"includedFrameworks":[{"name":"Microsoft.WindowsDesktop.App","version":"8.0.31"}]}}')
Check ((Get-SetupRuntimeState -Directory $app -Roots @()).Bundled) 'self-contained package needs no system install'
foreach ($url in @('http://builds.dotnet.microsoft.com/a.exe','https://example.com/a.exe','https://builds.dotnet.microsoft.com:444/a.exe','https://user@builds.dotnet.microsoft.com/a.exe','file:///C:/test.exe')) {
    Reject { Assert-MicrosoftDownload $url } ('untrusted URL rejected: '+$url)
}
Assert-MicrosoftDownload 'https://builds.dotnet.microsoft.com/dotnet/a.exe';Check $true 'official HTTPS endpoint accepted'
$file=[pscustomobject]@{name='windowsdesktop-runtime-win-x64.exe';rid='win-x64';url='https://builds.dotnet.microsoft.com/dotnet/a.exe';hash=('a'*128)}
$metadata=[pscustomobject]@{releases=@([pscustomobject]@{windowsdesktop=[pscustomobject]@{version='8.0.31';files=@($file)}})}
Check ((Select-DesktopInstaller $metadata).Version -eq '8.0.31') 'stable Desktop x64 installer selected'
$file.rid='win-arm64';Reject { Select-DesktopInstaller $metadata } 'wrong installer architecture rejected';$file.rid='win-x64'
$file.hash='invalid';Reject { Select-DesktopInstaller $metadata } 'malformed SHA-512 rejected';$file.hash='a'*128
$metadata.releases[0].windowsdesktop.version='8.0.32-preview.1';Reject { Select-DesktopInstaller $metadata } 'preview installer rejected'
$fake=Join-Path $root 'fake.exe';[IO.File]::WriteAllText($fake,'fixture')
Reject { Assert-Installer $fake ('a'*128) } 'download corruption rejected before execution'
$hash=(Get-FileHash -LiteralPath $fake -Algorithm SHA512).Hash
function Get-AuthenticodeSignature { param($LiteralPath) [pscustomobject]@{Status='Valid';SignerCertificate=[pscustomobject]@{Subject='CN=Other Publisher'}} }
Reject { Assert-Installer $fake $hash } 'non-Microsoft signature rejected'
function Get-AuthenticodeSignature { param($LiteralPath) [pscustomobject]@{Status='NotTrusted';SignerCertificate=[pscustomobject]@{Subject='CN=Microsoft Corporation'}} }
Reject { Assert-Installer $fake $hash } 'untrusted Microsoft-looking signature rejected'
function Get-AuthenticodeSignature { param($LiteralPath) [pscustomobject]@{Status='Valid';SignerCertificate=[pscustomobject]@{Subject='CN=Microsoft Corporation, O=Microsoft Corporation'}} }
Assert-Installer $fake $hash;Check $true 'valid hash and trusted Microsoft publisher accepted'
function Get-AuthenticodeSignature { param($LiteralPath) [pscustomobject]@{Status='Valid';SignerCertificate=[pscustomobject]@{Subject='CN=.NET, O=Microsoft Corporation'}} }
Assert-Installer $fake $hash;Check $true 'official .NET signing certificate accepted'
function Get-AuthenticodeSignature { param($LiteralPath) [pscustomobject]@{Status='Valid';SignerCertificate=[pscustomobject]@{Subject='CN=Microsoft Corporation, O=Other Publisher'}} }
Reject { Assert-Installer $fake $hash } 'lookalike common name without Microsoft organization rejected'
[pscustomobject]@{Passed=$script:checks;Fixtures=$root;NetworkUsed=$false;RuntimeInstalled=$false} | ConvertTo-Json -Compress
