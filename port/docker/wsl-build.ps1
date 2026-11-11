# Builds mkgp2-gfx.exe on a Windows PC through WSL and docker, and puts it in
# build\pc-gfx-win\dist of this clone. See docs/port/build.md,
# "Building the Windows executable on a Windows PC".
#
#   .\port\docker\wsl-build.ps1                      # origin/main
#   .\port\docker\wsl-build.ps1 -Ref win/my-branch   # a branch of this clone (committed work only)
#   .\port\docker\wsl-build.ps1 -Distro Ubuntu-24.04
param(
  [string]$Ref = 'origin/main',
  [string]$Distro = '',
  [string]$Dest = ''
)
$ErrorActionPreference = 'Stop'

function ConvertTo-WslPath([string]$Path) {
  $full = [IO.Path]::GetFullPath($Path)
  if ($full -notmatch '^([A-Za-z]):\\(.*)$') { throw "not a drive path: $full" }
  '/mnt/' + $Matches[1].ToLower() + '/' + ($Matches[2] -replace '\\', '/')
}

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Dest) { $Dest = Join-Path $repo 'build\pc-gfx-win\dist' }
New-Item -ItemType Directory -Force $Dest | Out-Null

$wslArgs = @()
if ($Distro) { $wslArgs += @('-d', $Distro) }
$wslArgs += @('--', 'bash', (ConvertTo-WslPath (Join-Path $PSScriptRoot 'wsl-build.sh')),
              $Ref, (ConvertTo-WslPath $Dest), (ConvertTo-WslPath $repo))
& wsl.exe @wslArgs
if ($LASTEXITCODE -ne 0) { throw "the build failed (exit code $LASTEXITCODE)" }
Write-Host "done: $Dest"
