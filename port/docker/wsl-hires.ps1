# Makes the high-resolution 2D texture folder of the PC port on a Windows PC
# through WSL and docker, from your own disc image, and puts it in
# build\pc-gfx-win\dist\hires of this clone (next to mkgp2-gfx.exe).
# See docs/port/hires_textures.md.
#
#   .\port\docker\wsl-hires.ps1 -Iso 'C:\path\Mario Kart Arcade GP 2 (Japan).iso'
#   .\port\docker\wsl-hires.ps1 -Iso ... -Scale 4            # for windows larger than 1280x960
#   .\port\docker\wsl-hires.ps1 -Iso ... -Distro Ubuntu-24.04 -Ref win/my-branch
#
# -Ai has the sprites enlarged by Real-ESRGAN on this PC's GPU instead of the
# built-in filter (the text fonts are made the built-in way either way):
#
#   .\port\docker\wsl-hires.ps1 -Iso ... -Ai                       # model realesrgan-x4plus-anime
#   .\port\docker\wsl-hires.ps1 -Iso ... -Ai -Model realesrgan-x4plus
#
# The program (realesrgan-ncnn-vulkan, BSD 3-clause, its models included) is
# downloaded once from the project's GitHub releases into build\hires-ai\tool;
# nothing is installed. -Tool names a folder that already holds it. The
# enlarged pictures in build\hires-ai\up take several GB; -Tile sets the tile
# size (0 = the program chooses from the free video memory).
param(
  [Parameter(Mandatory = $true)][string]$Iso,
  [ValidateSet(2, 4)][int]$Scale = 2,
  [string]$Ref = 'origin/main',
  [string]$Distro = '',
  [string]$Dest = '',
  [switch]$Ai,
  [ValidateSet('realesrgan-x4plus-anime', 'realesrgan-x4plus')][string]$Model = 'realesrgan-x4plus-anime',
  [string]$Tool = '',
  [int]$Tile = 0
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

function Invoke-Hires([string]$Mode, [string]$Work) {
  $a = @()
  if ($Distro) { $a += @('-d', $Distro) }
  $a += @('--', 'bash', (ConvertTo-WslPath (Join-Path $PSScriptRoot 'wsl-hires.sh')),
          $Ref, (ConvertTo-WslPath $isoPath), (ConvertTo-WslPath $Dest), "$Scale", (ConvertTo-WslPath $repo))
  if ($Mode -ne 'make') { $a += @($Mode, (ConvertTo-WslPath $Work)) }
  & wsl.exe @a
  if ($LASTEXITCODE -ne 0) { throw "making the textures failed ($Mode, exit code $LASTEXITCODE)" }
}

if (-not $Ai) {
  Invoke-Hires 'make' ''
  Write-Host "done: $Dest"
  return
}

$work = Join-Path $repo 'build\hires-ai'
New-Item -ItemType Directory -Force $work | Out-Null
if (-not $Tool) {
  $Tool = Join-Path $work 'tool'
  if (-not (Get-ChildItem -Path $Tool -Recurse -Filter 'realesrgan-ncnn-vulkan.exe' -ErrorAction SilentlyContinue)) {
    $url = 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip'
    $zip = Join-Path $work 'realesrgan.zip'
    Write-Host "downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $zip
    Expand-Archive -Path $zip -DestinationPath $Tool -Force
    Remove-Item $zip
  }
}
$exe = Get-ChildItem -Path $Tool -Recurse -Filter 'realesrgan-ncnn-vulkan.exe' | Select-Object -First 1
if (-not $exe) { throw "no realesrgan-ncnn-vulkan.exe under $Tool" }
$models = Join-Path $exe.DirectoryName 'models'

Invoke-Hires 'export' $work
$started = Get-Date
foreach ($kind in 'rgb', 'alpha') {
  $src = Join-Path $work "src\$kind"
  $up = Join-Path $work "up\$kind"
  if (Test-Path $up) { Remove-Item -Recurse -Force $up }
  New-Item -ItemType Directory -Force $up | Out-Null
  Write-Host "enlarging $kind with $Model"
  & $exe.FullName -i $src -o $up -n $Model -m $models -s 4 -f png -t $Tile
  if ($LASTEXITCODE -ne 0) { throw "the enlarger failed on $kind (exit code $LASTEXITCODE)" }
}
Write-Host ("enlarged in {0:n0} s" -f ((Get-Date) - $started).TotalSeconds)
Invoke-Hires 'import' $work
Write-Host "done: $Dest (the pictures in $work\src and $work\up may be deleted)"
