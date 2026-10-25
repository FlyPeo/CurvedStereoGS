[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string] $Scene,

  [string] $Renderer = ".\_bin\Release\CurvedStereoGS.exe",
  [ValidateSet(0, 1, 2)]
  [int] $StereoOutputMode = 2,
  [ValidateSet(0, 1)]
  [int] $GpuInterop = 1,
  [ValidateRange(1, 2147483647)]
  [int] $FrameCount = 300,
  [ValidateRange(0, 2147483647)]
  [int] $StatsInterval = 300,
  [ValidateRange(1, 16384)]
  [int] $Width = 1920,
  [ValidateRange(1, 16384)]
  [int] $Height = 1080,
  [string] $OutputDirectory = ".\artifacts"
)

$ErrorActionPreference = "Stop"

$rendererPath = [System.IO.Path]::GetFullPath($Renderer)
$scenePath = [System.IO.Path]::GetFullPath($Scene)
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)

if(-not (Test-Path -LiteralPath $rendererPath -PathType Leaf)) {
  throw "Renderer executable was not found: $rendererPath"
}
if(-not (Test-Path -LiteralPath $scenePath -PathType Leaf)) {
  throw "Scene file was not found: $scenePath"
}

New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$csvPath = Join-Path $outputPath "stereo-$timestamp.csv"
$logPath = Join-Path $outputPath "stereo-$timestamp.log"

$arguments = @(
  "--scene", $scenePath,
  "--size", "$Width", "$Height",
  "--benchmark", "1",
  "--stereoOutputMode", "$StereoOutputMode",
  "--activeStereoGpuInterop", "$GpuInterop",
  "--stereoStatsCsv", $csvPath,
  "--stereoStatsInterval", "$StatsInterval",
  "--headlessFrameCount", "$FrameCount"
)

Write-Host "Running CurvedStereoGS benchmark"
Write-Host "  Scene:  $scenePath"
Write-Host "  Mode:   $StereoOutputMode"
Write-Host "  Frames: $FrameCount"
Write-Host "  CSV:    $csvPath"

& $rendererPath @arguments 2>&1 | Tee-Object -FilePath $logPath
if($LASTEXITCODE -ne 0) {
  throw "CurvedStereoGS exited with code $LASTEXITCODE. See $logPath"
}

Write-Host "Benchmark completed."
Write-Host "Log: $logPath"
Write-Host "CSV: $csvPath"
