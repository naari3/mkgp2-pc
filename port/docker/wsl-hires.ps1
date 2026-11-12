# Makes the high-resolution 2D texture folder of the PC port on a Windows PC
# through WSL and docker, from your own disc image, and puts it in
# build\pc-gfx-win\dist\hires of this clone (next to mkgp2-gfx.exe).
# See docs/port/hires_textures.md.
#
#   .\port\docker\wsl-hires.ps1 -Iso 'C:\path\Mario Kart Arcade GP 2 (Japan).iso'
#   .\port\docker\wsl-hires.ps1 -Iso ... -Scale 4            # for windows larger than 1280x960
#   .\port\docker\wsl-hires.ps1 -Iso ... -Distro Ubuntu-24.04 -Ref win/my-branch
param(
  [Parameter(Mandatory = $true)][string]$Iso,
  [ValidateSet(2, 4)][int]$Scale = 2,
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
if (-not $Dest) { $Dest = Join-Path $repo 'build\pc-gfx-win\dist\hires' }
$isoPath = (Resolve-Path -LiteralPath $Iso).Path

$wslArgs = @()
if ($Distro) { $wslArgs += @('-d', $Distro) }
$wslArgs += @('--', 'bash', (ConvertTo-WslPath (Join-Path $PSScriptRoot 'wsl-hires.sh')),
              $Ref, (ConvertTo-WslPath $isoPath), (ConvertTo-WslPath $Dest), "$Scale", (ConvertTo-WslPath $repo))
& wsl.exe @wslArgs
if ($LASTEXITCODE -ne 0) { throw "making the textures failed (exit code $LASTEXITCODE)" }
Write-Host "done: $Dest"
