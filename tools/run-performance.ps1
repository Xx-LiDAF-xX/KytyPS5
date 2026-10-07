param(
    [string]$GamePath = 'C:\ROMS\PPSA21567',
    [string]$EmulatorPath,
    [string]$RunDirectory,
    [string]$LogDirectory,
    [switch]$EnableRayTracing,
    [switch]$DisableRayTracing,
    [switch]$GpuZones,
    [switch]$CaptureShaders,
    [switch]$InputTrace,
    [switch]$CpuPhases,
    [string]$TesterId,
    [string]$Scene,
    [string]$Notes
)

$ErrorActionPreference = 'Stop'
function Protect-DiagnosticText([string]$Text) {
    $safe = $Text -replace '(?i)[a-z]:[\\/][^\r\n"<>|]*|\\\\[^\r\n"<>|]*|(?<!\S)/(?:[^\r\n"<>|]*)', '[path redacted]'
    $safe = $safe -replace '(?i)(?:https?|ftp)://\S+|[\w.+%-]+@[\w.-]+\.[a-z]{2,}|\b(?:\d{1,3}\.){3}\d{1,3}\b', '[network value redacted]'
    $safe = $safe -replace '(?i)(\b(?:username|user_name|hostname|password|token|cookie|authorization|email|serial)\s*[:=]\s*)[^\r\n]*', '$1[private value redacted]'
    foreach ($name in @($env:USERNAME, $env:USER, $env:COMPUTERNAME, $env:HOSTNAME)) {
        if ($name) { $safe = $safe -replace ('(?i)(?<!\w)' + [regex]::Escape($name) + '(?!\w)'), '[identity redacted]' }
    }
    return $safe
}
function Get-TestHardwareInfo {
    # Inventory is diagnostic only; its failure must not prevent the game launch.
    try {
        $processors = @(Get-CimInstance Win32_Processor -ErrorAction Stop | ForEach-Object {
            [ordered]@{ Name = $_.Name; Cores = $_.NumberOfCores; Threads = $_.NumberOfLogicalProcessors }
        })
        $adapters = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object {
            [ordered]@{ Name = $_.Name; DriverVersion = $_.DriverVersion }
        })
        $system = Get-CimInstance Win32_ComputerSystem -ErrorAction Stop
        $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
        return [ordered]@{
            Processors = $processors
            DisplayAdapters = $adapters
            PhysicalMemoryBytes = [uint64]$system.TotalPhysicalMemory
            OS = $os.Caption
            OSVersion = $os.Version
            OSBuild = $os.BuildNumber
        }
    } catch {
        return [ordered]@{ InventoryError = 'Hardware inventory unavailable' }
    }
}
if ($EnableRayTracing -and $DisableRayTracing) {
    throw 'Choose only one ray-tracing override.'
}
$repository = Split-Path -Parent $PSScriptRoot
if (-not $EmulatorPath) { $EmulatorPath = Join-Path $repository 'bin\kyty_emulator.exe' }
$emulator = (Resolve-Path -LiteralPath $EmulatorPath).Path
$game = (Resolve-Path -LiteralPath $GamePath).Path
if (-not $RunDirectory) { $RunDirectory = Split-Path -Parent $emulator }
$runPath = (Resolve-Path -LiteralPath $RunDirectory).Path
if (-not $LogDirectory) { $LogDirectory = Join-Path (Split-Path -Parent $repository) 'kyty logs' }
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
$logRoot = (Resolve-Path -LiteralPath $LogDirectory).Path
$logPath = Join-Path $logRoot ('performance-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.txt')
$arguments = @('--game', $game, '--drain-stats', '5', '--gpu-timestamp-scale', '100',
    '--auto-optimize', 'false', '--printf-direction', 'File', '--printf-output-file', $logPath)
if ($EnableRayTracing) { $arguments += @('--ray-tracing', 'true') }
if ($DisableRayTracing) { $arguments += @('--ray-tracing', 'false') }
if ($CaptureShaders) {
    $arguments += @('--graphics-debug-dump', 'true', '--shader-log-direction', 'File',
        '--shader-log-folder', ($logPath + '.shaders'))
}
$metadata = [ordered]@{
    SchemaVersion = 3
    TesterId = Protect-DiagnosticText $TesterId
    Scene = Protect-DiagnosticText $Scene
    Notes = Protect-DiagnosticText $Notes
    SHA256 = (Get-FileHash -LiteralPath $emulator -Algorithm SHA256).Hash
    TitleId = if ((Split-Path -Leaf $game) -match '^PPSA[0-9]{5}$') { Split-Path -Leaf $game } else { 'Unknown' }
    EbootSHA256 = if (Test-Path -LiteralPath (Join-Path $game 'eboot.bin') -PathType Leaf) {
        (Get-FileHash -LiteralPath (Join-Path $game 'eboot.bin') -Algorithm SHA256).Hash
    } else { $null }
    # Paths are needed for launch, but must never be serialized into tester metadata.
    Arguments = @($arguments | ForEach-Object {
        if ([IO.Path]::IsPathRooted($_)) { '[path redacted]' } else { Protect-DiagnosticText $_ }
    })
    GpuZones = [bool]$GpuZones
    CaptureShaders = [bool]$CaptureShaders
    InputTrace = [bool]$InputTrace
    CpuPhases = [bool]$CpuPhases
    Hardware = Get-TestHardwareInfo
    # Collect only numeric/boolean diagnostic settings, never raw INI contents.
    SettingsBeforeLaunch = if (Test-Path -LiteralPath (Join-Path $runPath 'kyty_settings.ini') -PathType Leaf) {
        $safeSettings = [ordered]@{}
        foreach ($line in (Get-Content -LiteralPath (Join-Path $runPath 'kyty_settings.ini'))) {
            if ($line -match '^\s*([a-z-]+)\s*=\s*(true|false|[0-9]+)\s*$' -and
                $Matches[1] -in @('ray-tracing', 'auto-optimize', 'gpu-timestamp-scale', 'aniso',
                    'res-scale', 'async-submit', 'pipeline-libraries', 'async-pipelines',
                    'relaxed-readback', 'speculative-draws', 'record-thread', 'hardware-buffer-bounds',
                    'vulkan-validation', 'shader-validation', 'drain-stats', 'motion-blur',
                    'depth-of-field', 'bloom', 'ambient-occlusion')) {
                $safeSettings[$Matches[1]] = $Matches[2]
            }
        }
        $safeSettings
    } else { $null }
    StartedUtc = [DateTime]::UtcNow.ToString('o')
}
$metadata | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath ($logPath + '.json')
$previousZones = $env:KYTY_GPU_ZONES
$previousTrace = $env:KYTY_TRACE_INPUT
$previousPhases = $env:KYTY_DEBUG_DRAW_PHASES
$env:KYTY_GPU_ZONES = if ($GpuZones) { '1' } else { '0' }
$env:KYTY_TRACE_INPUT = if ($InputTrace) { '1' } else { '0' }
$env:KYTY_DEBUG_DRAW_PHASES = if ($CpuPhases) { 'all' } else { $null }
Write-Output ('Performance log: ' + [IO.Path]::GetFileName($logPath))
Write-Output 'Play the scene to measure, then close the game to finish. GPU zone timestamps add profiling overhead.'
Push-Location -LiteralPath $runPath
$exitCode = $null
try {
    & $emulator @arguments
    $exitCode = $LASTEXITCODE
} finally {
    Pop-Location
    $env:KYTY_GPU_ZONES = $previousZones
    $env:KYTY_TRACE_INPUT = $previousTrace
    $env:KYTY_DEBUG_DRAW_PHASES = $previousPhases
    $metadata['FinishedUtc'] = [DateTime]::UtcNow.ToString('o')
    $metadata['ExitCode'] = $exitCode
    $metadata | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath ($logPath + '.json')
}
if ($exitCode -ne 0) { throw "Emulator exited with code $exitCode. See the dated performance log." }
