# Publishes a GitHub release: clean build + tests, then Visualizer.exe, a zip (exe + README) and
# Visualizer.exe.sha256 (which the app's auto-updater requires before it will install an update).
#
#   1. Bump the version in CMakeLists.txt and res\app.rc, commit.
#   2. .\tools\release.ps1 -Notes path\to\notes.md
#
# Pushes main, tags v<version> and creates the release on GitHub (needs the gh CLI, logged in).
param(
    [Parameter(Mandatory)][string]$Notes,
    [string]$Title
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$version = [regex]::Match((Get-Content CMakeLists.txt -Raw), 'project\(Visualizer VERSION (\d+\.\d+\.\d+)').Groups[1].Value
if (-not $version) { throw 'Could not read the version from CMakeLists.txt' }
if ((Get-Content res\app.rc -Raw) -notmatch [regex]::Escape("""$version""")) { throw "res\app.rc doesn't say $version" }
if (git status --porcelain) { throw 'Commit your changes first' }
$tag = "v$version"
if (git tag --list $tag) { throw "$tag already exists" }

.\build.ps1 -Clean -Test
$exe = Join-Path $root 'bin\Visualizer.exe'
if ((Get-Item $exe).VersionInfo.FileVersion -ne $version) { throw "bin\Visualizer.exe isn't version $version" }

$out = Join-Path $env:TEMP "visualizer-release-$version"
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory $out | Out-Null
Copy-Item $exe, (Join-Path $root 'README.md') $out
$zip = Join-Path $out "old-school-visualizer-$tag-win64.zip"
Compress-Archive -Path (Join-Path $out 'Visualizer.exe'), (Join-Path $out 'README.md') -DestinationPath $zip
$hash = (Get-FileHash (Join-Path $out 'Visualizer.exe') -Algorithm SHA256).Hash.ToLower()
$sum = Join-Path $out 'Visualizer.exe.sha256'
[IO.File]::WriteAllText($sum, "$hash  Visualizer.exe`n")

git push origin main
git tag -a $tag -m "$tag"
git push origin $tag
if (-not $Title) { $Title = $tag }
gh release create $tag (Join-Path $out 'Visualizer.exe') $zip $sum --title $Title --notes-file $Notes --verify-tag
Write-Host "Released $tag (Visualizer.exe sha256 $hash)"
