param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\..\launcher-preview'),
    [string]$Toolchain = 'C:\msys64\clang64\bin'
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path -Parent $PSScriptRoot
$taskOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $taskOutput | Out-Null
$env:PATH = "$Toolchain;$env:PATH"
Push-Location $taskRepo
try {
    $resource = Join-Path $taskOutput 'launcher.res.o'
    & (Join-Path $Toolchain 'windres.exe') -I launcher launcher/launcher.rc -O coff -o $resource
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed' }
    # The native updater now unpacks ZIPs in process. Build its miniz dependency
    # here too, so a standalone WPF build needs no pre-existing GPU build tree.
    $minizSource = Join-Path $taskRepo 'gpu\third_party\miniz'
    $minizBuild = Join-Path $taskOutput 'miniz'
    & (Join-Path $Toolchain 'cmake.exe') -S $minizSource -B $minizBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DINSTALL_PROJECT=OFF -DCMAKE_C_COMPILER=clang
    if ($LASTEXITCODE -ne 0) { throw 'Miniz configuration failed' }
    & (Join-Path $Toolchain 'cmake.exe') --build $minizBuild --target miniz
    if ($LASTEXITCODE -ne 0) { throw 'Miniz compilation failed' }
    & (Join-Path $Toolchain 'clang++.exe') -O2 -std=c++20 -municode -mwindows -static "-I$minizSource" "-I$minizBuild" -DMINIZ_STATIC_DEFINE -o (Join-Path $taskOutput 'DriveClub.exe') launcher/driveclub_launcher.cpp launcher/prepare.cpp launcher/gpu_check.cpp launcher/datx.cpp launcher/unlock.cpp launcher/graphics.cpp launcher/carswitch.cpp launcher/updater.cpp $resource (Join-Path $minizBuild 'libminiz.a') -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -ldwmapi -lwinhttp -lbcrypt -lcomctl32 -loleaut32 -lwbemuuid -liphlpapi
    if ($LASTEXITCODE -ne 0) { throw 'Launcher compilation failed' }
} finally { Pop-Location }
