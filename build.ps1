<#
  Builds Visualizer.exe into .\bin
    .\build.ps1                 # Release
    .\build.ps1 -Config Debug   # Debug (bin\Visualizer-debug.exe)
    .\build.ps1 -Clean -Run     # wipe build dir, build, then launch
    .\build.ps1 -Test           # build and run the analyzer tests
#>
param(
    [ValidateSet('Release', 'Debug')][string]$Config = 'Release',
    [switch]$Clean,
    [switch]$Run,
    [switch]$Test
)
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio C++ tools not found (install "Desktop development with C++").' }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'

$root = $PSScriptRoot
$buildDir = Join-Path $root "build\$Config"
if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }
New-Item -ItemType Directory -Force $buildDir | Out-Null

# Run through a temp .cmd so vcvars' environment applies to cmake (avoids PowerShell quoting issues).
$script = Join-Path $buildDir 'build.cmd'
@"
@echo off
set "PATH=%PATH%;$(Split-Path $vswhere)"
call "$vcvars" >nul || exit /b 1
cmake -S "$root" -B "$buildDir" -G Ninja -DCMAKE_BUILD_TYPE=$Config || exit /b 1
cmake --build "$buildDir" || exit /b 1
"@ | Set-Content -Encoding ascii $script

& cmd.exe /c $script
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

$exe = Join-Path $root ($(if ($Config -eq 'Debug') { 'bin\Visualizer-debug.exe' } else { 'bin\Visualizer.exe' }))
Write-Host "Built $exe ($([math]::Round((Get-Item $exe).Length / 1KB)) KB)"
if ($Test) {
    & (Join-Path $buildDir 'analyzer_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
}
if ($Run) { Start-Process $exe }
