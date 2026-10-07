# Collecting tester logs

Run from the repository root in PowerShell. Use a stable nickname for each
tester and describe the exact scene and problem. For example:

```powershell
.\tools\run-performance.ps1 -EmulatorPath 'C:\Kytytest\kyty_emulator.exe' -GamePath 'C:\ROMS\PPSA21567' -LogDirectory 'C:\Kyty-test-logs' -TesterId 'tester-01' -Scene 'Aztec Trail, frog gloves at gold door' -Notes 'Frame drops while punching' -DisableRayTracing -GpuZones -CpuPhases
```

Play the affected scene, then close the game. Send both the dated `.txt` log
and its `.txt.json` metadata. Metadata includes executable SHA256, CPU/core
counts, display adapters/drivers, RAM, Windows version, numeric/boolean diagnostic
settings, sanitized arguments, scene, and exit code. Hardware inventory failure is recorded without
preventing launch. The selected Vulkan adapter remains identified in the log;
the inventory can include other installed adapters. Metadata excludes host paths,
device instance identifiers, raw INI contents, usernames, and computer names.
Path, network-address, and known host identity redaction also applies to freeform
notes. Use a neutral tester nickname and describe gameplay without personal
details; arbitrary personal information in prose cannot be reliably inferred.

Use `-EnableRayTracing` for a separate RT-on launch. Compare the same scene,
build, and settings across launches. GPU zones and CPU phase measurements add
overhead; collect an ordinary run as well before claiming a performance gain.
`-InputTrace` is available for controller problems, but adds frequent logging.

Save received files with distinct names in `repos\kyty logs` so one tester does
not overwrite another. The monitor compares content hashes and crash signatures.
Each case should retain build identity, hardware/driver, game version, active
RT state, launch settings, scene, fatal error and Vulkan result, and shutdown
status. Missing data stays unknown. A normal shutdown is evidence for that run,
not proof of general stability. Group matching failures across machines before
making a hardware-specific workaround; retain a working-machine case to check
for regressions after each fix.
