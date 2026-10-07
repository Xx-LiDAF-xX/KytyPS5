param(
    [ValidateSet('PPSA01325', 'PPSA21567')]
    [string]$TitleId = 'PPSA21567',
    [string]$RomRoot = 'C:\ROMS',
    [string]$EmulatorPath,
    [string]$RunDirectory
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
if (-not $EmulatorPath) {
    $EmulatorPath = Join-Path $repository 'bin\kyty_emulator.exe'
}
$emulator = (Resolve-Path -LiteralPath $EmulatorPath).Path
$game = (Resolve-Path -LiteralPath (Join-Path $RomRoot $TitleId)).Path
$patchPath = (Resolve-Path -LiteralPath (Join-Path $repository "patches\rt-native\$TitleId.json")).Path
$patch = Get-Content -LiteralPath $patchPath -Raw | ConvertFrom-Json
$gameExecutable = Join-Path $game 'eboot.bin'
$actualHash = (Get-FileHash -LiteralPath $gameExecutable -Algorithm SHA256).Hash
if ($actualHash -ne $patch.source_sha256) {
    throw "This profile does not match $gameExecutable. Expected $($patch.source_sha256), found $actualHash."
}
if (-not $RunDirectory) {
    # Use the existing installed emulator's save directory by default.
    $RunDirectory = Split-Path -Parent $emulator
}
New-Item -ItemType Directory -Path $RunDirectory -Force | Out-Null
$runPath = (Resolve-Path -LiteralPath $RunDirectory).Path
$emulatorArguments = @('--game', $game, '--game-patch', $patchPath,
    '--preset', 'Quality', '--auto-optimize', 'false', '--ray-tracing', 'true',
    '--async-pipelines', 'false', '--relaxed-readback', 'false', '--speculative-draws', 'false',
    '--gpu-timestamp-scale', '100', '--shader-validation', 'true', '--vulkan-validation', 'true')
Write-Output "Native RT/GI profile: $TitleId. RT is experimental; later rendering failures remain under investigation."
Write-Output "Emulator SHA256: $((Get-FileHash -LiteralPath $emulator -Algorithm SHA256).Hash)"
Push-Location -LiteralPath $runPath
try {
    & $emulator @emulatorArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Emulator exited with code $LASTEXITCODE. Run directory: $runPath"
    }
} finally {
    Pop-Location
}
