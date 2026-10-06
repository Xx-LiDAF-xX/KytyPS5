param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('PPSA01325', 'PPSA21567', 'PPSA01859')]
    [string]$TitleId,
    [string]$RomRoot = 'C:\ROMS',
    [string]$EmulatorPath,
    [switch]$Validation
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
if (-not $EmulatorPath) {
    $EmulatorPath = Join-Path $repository '_Build\rt\kyty_emulator.exe'
}
$emulator = (Resolve-Path -LiteralPath $EmulatorPath).Path
$game = Join-Path $RomRoot $TitleId
if (-not (Test-Path -LiteralPath $game -PathType Container)) {
    $game += '.zar'
}
$game = (Resolve-Path -LiteralPath $game).Path

# Per-title settings, caches, logs and saves belong to the run directory.
$runDirectory = Join-Path $repository "_Build\rt\runs\$TitleId"
New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null
$emulatorArguments = @('--game', $game, '--preset', 'Quality', '--auto-optimize', 'false',
    '--ray-tracing', 'true', '--relaxed-readback', 'false', '--speculative-draws', 'false',
    '--gpu-timestamp-scale', '100')
if ($Validation) {
    $emulatorArguments += @('--vulkan-validation', 'true', '--shader-validation', 'true',
        '--drain-stats', '5')
}
Push-Location -LiteralPath $runDirectory
try {
    & $emulator @emulatorArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Emulator exited with code $LASTEXITCODE. Logs: $runDirectory"
    }
} finally {
    Pop-Location
}
