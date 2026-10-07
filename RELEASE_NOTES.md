# KytyPS5 Windows prerelease — 2026-10-07

This build preserves game touchpad input, persists RT settings across launcher
sessions, and reports the active RT state separately from the next-launch state.
It implements asynchronous APR address-wait ordering, applies fallback audio gain
to the controller speaker, gates input tracing behind an explicit option, and
records Vulkan semaphore error codes and timeline ticks.

## Install and settings

Extract the entire ZIP into a fresh writable folder and run `launcher.exe`.
The archive includes a fresh portable launcher profile and emulator settings.
Select your own game directory. Preserve your existing saves before migrating.

RT and automatic optimization default to off. Enable RT in Graphics settings or
F2, then fully restart the game. RT uses guest BVH intersections through Vulkan
compute; the setting does not establish native hardware RT use or visual parity.
The touchpad no longer opens emulator quality settings. File logging defaults to
unique UTC-dated `_kyty` files. CLI console/silent options remain available.

## Validation

The emulator, launcher, and all configured regression executables built with the
Windows ClangCL Release configuration. All 73 CTest entries were run sequentially
with a 90-second timeout: **64 passed; 9 failed or timed out**.

Privacy logging, RT settings, APR ordering, controller settings, pad haptics, and
focused shader RT tests passed. Earlier targeted APR ordering stress checks also
passed. A short isolated Astro Bot startup with RT off completed without a fatal
error; this is startup coverage, not sustained gameplay validation.

Unresolved full-suite results:

| Tests | Observed failure |
| --- | --- |
| `kernel_file_system` | Windows per-call nonblocking socket expectation |
| `audio_out_timing` | Priming deadline assertion |
| `shader_recompiler_compute`, `shader_recompiler_compute_hw_bounds`, `pm4_context_state` | Native target geometry register classification |
| `gpu_command_lane` | 90-second timeout |
| `gpu_tiler` | Scratch workspace reuse assertion |
| `compute_meta_clear_classification`, `texture_cache_image_overlap` | GPU color metadata clear value |

These failures remain open. Publication does not establish a stable build, a fix
for every crash, complete RT correctness, or a guarantee of 60 FPS.

## Privacy and archive contents

Runtime diagnostics filter host paths, known host identities, network addresses,
and sensitive fields. Guest diagnostic fragments are buffered before filtering.
Freeform tester descriptions should still be reviewed before sharing.

The runtime ZIP uses an explicit file allowlist. It excludes games, patches,
saves, logs, caches, captures, build directories, research reports, and PDBs.
It includes only executables, required DLL/plugin dependencies, fresh settings,
documentation, component licenses, and a SHA-256 build manifest.

Executable source paths are mapped to relative paths and PDB references use
basenames. Signed Qt DLLs retain standard vendor build/debug paths and parser
strings; their signatures are preserved. These are vendor data rather than
developer or tester installation directories. No embedded personal host identity
or credential was found in the audited current runtime files.

Obsolete tracked release ZIPs were removed from the current repository tree,
while local copies were preserved. Historical Git commits and previous releases
were not rewritten or deleted and may retain earlier artifacts.

`BUILD-MANIFEST.json` records source and package revisions and exact file hashes.
The companion `.zip.sha256` verifies the archive.
