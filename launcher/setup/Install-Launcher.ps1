param([switch]$CheckOnly, [switch]$LibraryOnly, [switch]$InstallOnly)
$ErrorActionPreference = 'Stop'
function Test-X64Image([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $stream = [IO.File]::OpenRead($Path)
    try {
        $reader = [IO.BinaryReader]::new($stream)
        if ($reader.ReadUInt16() -ne 0x5a4d -or $stream.Length -lt 64) { return $false }
        $stream.Position = 60; $offset = $reader.ReadInt32()
        if ($offset -lt 64 -or $offset + 6 -gt $stream.Length) { return $false }
        $stream.Position = $offset
        return $reader.ReadUInt32() -eq 0x4550 -and $reader.ReadUInt16() -eq 0x8664
    } catch { return $false } finally { $stream.Dispose() }
}
function Get-RuntimeRoots {
    $roots = @($env:DOTNET_ROOT_X64, $env:DOTNET_ROOT)
    foreach ($hive in @([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryHive]::CurrentUser)) {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, [Microsoft.Win32.RegistryView]::Registry64)
        try {
            $key = $base.OpenSubKey('SOFTWARE\dotnet\Setup\InstalledVersions\x64')
            if ($key) { try { $roots += $key.GetValue('InstallLocation') } finally { $key.Dispose() } }
        } finally { $base.Dispose() }
    }
    if ($env:ProgramW6432) { $roots += Join-Path $env:ProgramW6432 'dotnet' }
    if ($env:ProgramFiles) { $roots += Join-Path $env:ProgramFiles 'dotnet' }
    @($roots | Where-Object { $_ } | Select-Object -Unique)
}
function Get-StableEight([string]$Path, [string]$NativeFile) {
    if (!(Test-Path -LiteralPath $Path -PathType Container)) { return }
    Get-ChildItem -LiteralPath $Path -Directory | ForEach-Object {
        if ($_.Name -match '^8\.0\.\d+$' -and (Test-X64Image (Join-Path $_.FullName $NativeFile))) {
            [version]$_.Name
        }
    } | Sort-Object -Descending
}
function Get-SetupRuntimeState([string]$Directory = $PSScriptRoot, [string[]]$Roots = (Get-RuntimeRoots)) {
    $config = Join-Path $Directory 'DriveClub.Launcher.runtimeconfig.json'
    if (Test-Path -LiteralPath $config) {
        try {
            $included = @((Get-Content -LiteralPath $config -Raw | ConvertFrom-Json).runtimeOptions.includedFrameworks)
            if (@($included | Where-Object { $_.name -eq 'Microsoft.WindowsDesktop.App' -and $_.version -match '^8\.' }).Count -gt 0 -and
                (Test-X64Image (Join-Path $Directory 'coreclr.dll')) -and
                (Test-X64Image (Join-Path $Directory 'PresentationNative_cor3.dll')) -and
                (Test-X64Image (Join-Path $Directory 'hostfxr.dll')) -and
                (Test-X64Image (Join-Path $Directory 'hostpolicy.dll'))) {
                return [pscustomobject]@{ Available=$true; Bundled=$true; Root=$Directory }
            }
        } catch {}
    }
    foreach ($root in $Roots) {
        $core = @(Get-StableEight (Join-Path $root 'shared\Microsoft.NETCore.App') 'coreclr.dll')
        $desktop = @(Get-StableEight (Join-Path $root 'shared\Microsoft.WindowsDesktop.App') 'PresentationNative_cor3.dll')
        if ($core.Count -gt 0 -and $desktop.Count -gt 0 -and $core[0] -ge $desktop[0]) {
            return [pscustomobject]@{ Available=$true; Bundled=$false; Root=$root }
        }
    }
    [pscustomobject]@{ Available=$false; Bundled=$false; Root='' }
}
function Assert-MicrosoftDownload([string]$Url) {
    $uri = $null
    if (![uri]::TryCreate($Url, [UriKind]::Absolute, [ref]$uri) -or $uri.Scheme -ne 'https' -or
        !$uri.IsDefaultPort -or $uri.UserInfo -or $uri.Fragment -or
        $uri.DnsSafeHost -notin @('builds.dotnet.microsoft.com','download.visualstudio.microsoft.com','dotnetcli.azureedge.net')) {
        throw 'The .NET download URL is not an approved Microsoft HTTPS endpoint.'
    }
}
function Select-DesktopInstaller($Metadata) {
    $releases = @($Metadata.releases | Where-Object { $_.windowsdesktop.version -match '^8\.0\.\d+$' } |
        Sort-Object { [version]$_.windowsdesktop.version } -Descending)
    foreach ($release in $releases) {
        $files = @($release.windowsdesktop.files | Where-Object { $_.rid -eq 'win-x64' -and $_.name -eq 'windowsdesktop-runtime-win-x64.exe' })
        if ($files.Count -eq 1) {
            $file = $files[0]; Assert-MicrosoftDownload $file.url
            if ($file.hash -notmatch '^[a-fA-F0-9]{128}$') { throw 'Invalid .NET installer SHA-512 in release metadata.' }
            return [pscustomobject]@{ Version=$release.windowsdesktop.version; Url=$file.url; Hash=$file.hash }
        }
    }
    throw 'Microsoft release metadata does not contain a stable .NET 8 Desktop x64 installer.'
}
function Assert-Installer([string]$Path, [string]$Hash) {
    if ($Hash -notmatch '^[a-fA-F0-9]{128}$' -or (Get-FileHash -LiteralPath $Path -Algorithm SHA512).Hash -ne $Hash) {
        throw 'The .NET installer download is incomplete or its SHA-512 does not match.'
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid' -or !$signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
        throw 'The .NET installer does not have a valid Microsoft Corporation signature.'
    }
}
function Receive-SetupFile([string]$Url, [string]$Path, $Form, $Label, $Progress) {
    Assert-MicrosoftDownload $Url
    $client = [Net.WebClient]::new()
    $id = 'DriveClubSetup.' + [guid]::NewGuid().ToString('N')
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $null = Register-ObjectEvent $client DownloadFileCompleted -SourceIdentifier $id
    try {
        $client.DownloadFileAsync([uri]$Url, $Path)
        while ($true) {
            $event = Get-Event -SourceIdentifier $id -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($event) {
                if ($event.SourceEventArgs.Cancelled) { throw 'Download cancelled.' }
                if ($event.SourceEventArgs.Error) { throw $event.SourceEventArgs.Error }
                break
            }
            if ($script:SetupCancelled) { $client.CancelAsync(); throw 'Setup cancelled.' }
            if ($watch.Elapsed.TotalMinutes -gt 5) { $client.CancelAsync(); throw 'Download timed out. Please start DriveClub.exe again.' }
            if ($Form) { [Windows.Forms.Application]::DoEvents() }
            Start-Sleep -Milliseconds 50
        }
    } finally {
        Unregister-Event -SourceIdentifier $id -ErrorAction SilentlyContinue
        Remove-Event -SourceIdentifier $id -ErrorAction SilentlyContinue
        $client.Dispose()
    }
}
function Start-ConfiguredLauncher($State) {
    if (!$State.Bundled) { $env:DOTNET_ROOT_X64=$State.Root; $env:DOTNET_ROOT=$State.Root }
    Start-Process -FilePath (Join-Path $PSScriptRoot 'DriveClub.Launcher.exe') -WorkingDirectory $PSScriptRoot
}
if ($LibraryOnly) { return }
try {
    $state = Get-SetupRuntimeState
    if ($CheckOnly) { $state | ConvertTo-Json -Compress; if ($state.Available) { exit 0 } else { exit 2 } }
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    [Windows.Forms.Application]::EnableVisualStyles()
    if (!(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'DriveClub.Launcher.exe'))) { throw 'Extract the complete launcher package before running DriveClub.exe.' }
    if ($state.Available) { if (!$InstallOnly) { Start-ConfiguredLauncher $state }; exit 0 }
    $answer = [Windows.Forms.MessageBox]::Show(
        'The launcher needs .NET 8 Desktop Runtime (x64). Download and install it from Microsoft now? Windows may ask for administrator permission.',
        'DRIVECLUB Launcher Setup', 'YesNo', 'Question')
    if ($answer -ne 'Yes') { exit 0 }
    $script:SetupCancelled=$false
    $form=[Windows.Forms.Form]::new(); $form.Text='DRIVECLUB Launcher Setup'; $form.ClientSize=[Drawing.Size]::new(450,125)
    $form.StartPosition='CenterScreen'; $form.FormBorderStyle='FixedDialog'; $form.MaximizeBox=$false; $form.MinimizeBox=$false
    $label=[Windows.Forms.Label]::new(); $label.SetBounds(18,18,414,35); $label.Text='Reading Microsoft release information...'
    $progress=[Windows.Forms.ProgressBar]::new(); $progress.SetBounds(18,64,414,22); $progress.Style='Marquee'
    $form.Controls.AddRange(@($label,$progress)); $form.Add_FormClosing({ $script:SetupCancelled=$true })
    $form.Show(); [Windows.Forms.Application]::DoEvents()
    $temp=Join-Path ([IO.Path]::GetTempPath()) ('DriveClubSetup-'+[guid]::NewGuid().ToString('N'))
    $null=New-Item -ItemType Directory -Path $temp
    $metadata=Join-Path $temp 'releases.json'; $installer=Join-Path $temp 'windowsdesktop-runtime.exe'
    try {
        [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
        Receive-SetupFile 'https://builds.dotnet.microsoft.com/dotnet/release-metadata/8.0/releases.json' $metadata $form $label $progress
        $release=Select-DesktopInstaller (Get-Content -LiteralPath $metadata -Raw | ConvertFrom-Json)
        $label.Text='Downloading .NET '+$release.Version+' Desktop Runtime (x64)...'
        Receive-SetupFile $release.Url $installer $form $label $progress
        $label.Text='Verifying Microsoft installer...'; [Windows.Forms.Application]::DoEvents()
        Assert-Installer $installer $release.Hash
        if ($script:SetupCancelled) { throw 'Setup cancelled.' }
        $label.Text='Installing .NET. Please approve the Windows administrator prompt.'
        $process=Start-Process -FilePath $installer -ArgumentList @('/install','/quiet','/norestart') -Verb RunAs -PassThru
        $form.ControlBox=$false
        while (!$process.HasExited) { [Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 100; $process.Refresh() }
        if ($process.ExitCode -notin @(0,3010)) { throw ('Microsoft installer returned error '+$process.ExitCode+'.') }
        $state=Get-SetupRuntimeState
        if ($process.ExitCode -eq 3010) {
            $null=[Windows.Forms.MessageBox]::Show('Setup completed. Windows requested a restart; restart and then open DriveClub.exe.','DRIVECLUB Launcher Setup','OK','Information')
            exit 3010
        } elseif (!$state.Available) { throw '.NET installation finished but the x64 Desktop runtime could not be found. Restart Windows and open DriveClub.exe again.' }
        elseif (!$InstallOnly) { Start-ConfiguredLauncher $state }
    } finally {
        if ($form) { $form.Dispose() }
        foreach ($file in @($metadata,$installer)) { if ($file -and (Test-Path -LiteralPath $file)) { Remove-Item -LiteralPath $file -Force } }
        if ($temp -and (Test-Path -LiteralPath $temp) -and [IO.Directory]::GetFileSystemEntries($temp).Count -eq 0) { Remove-Item -LiteralPath $temp }
    }
} catch {
    if ($CheckOnly) { Write-Error $_; exit 1 }
    if ($script:SetupCancelled -or ($_.Exception -is [ComponentModel.Win32Exception] -and $_.Exception.NativeErrorCode -eq 1223)) { exit 0 }
    Add-Type -AssemblyName System.Windows.Forms
    $null=[Windows.Forms.MessageBox]::Show($_.Exception.Message,'DRIVECLUB Launcher Setup','OK','Error')
    exit 1
}
