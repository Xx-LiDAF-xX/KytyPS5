# Initial Compatibility Corpus

The first corpus contains eight owned installations. Expected names are aids
only; `sce_sys/param.json` is authoritative, and every unverified field remains
`pending` until inspected locally.

| Title ID | Expected title | Local metadata | Windows | Linux | PS5 route |
| --- | --- | --- | --- | --- | --- |
| PPSA01325 | Astro's Playroom | recorded | timeout | not tested | pending |
| PPSA01859 | Resident Evil Village: Maiden | recorded | timeout | not tested | pending |
| PPSA02100 | Stray | recorded | timeout | not tested | pending |
| PPSA21567 | Astro Bot | recorded | timeout | not tested | pending |
| PPSA25991 | Senua's Saga: Hellblade II | recorded | timeout | not tested | pending |
| PPSA26223 | The Mound: Omen of Cthulhu | recorded | timeout | not tested | pending |
| PPSA27624 | Bendy and the Dark Revival | recorded | timeout | not tested | pending |
| PPSA31162 | Super Meat Boy 3D | recorded | timeout | not tested | pending |

## Private setup

Copy `tools/compat/corpus.example.json` to the ignored
`tools/compat/corpus.local.json`. Replace each placeholder with the root of the
matching owned installation. A directory installation must contain
`eboot.bin` and `sce_sys/param.json`. Optional patches must be local files and
must match the exact title and content version.

For every entry, privately record these values before running it:

- `titleId`, `contentId`, `contentVersion` or `appVersion`, default language,
  localized title, region when present, and SDK metadata when present;
- SHA-256 of `eboot.bin` (or the selected executable inside a lawful local
  extraction);
- source type (`directory` or supported archive) and whether a patch is active;
- acquisition provenance sufficient for the owner to reproduce the setup,
  without copying protected material into the repository.

On Windows, the executable hash can be obtained with:

```powershell
Get-FileHash -Algorithm SHA256 "D:\owned-game\eboot.bin"
```

On Linux, use:

```bash
sha256sum /path/to/owned-game/eboot.bin
```

Do not commit the local manifest, raw `param.json`, executable, hashes tied to
an undisclosed path record, save data, screenshots, video, or `_CompatibilityRuns`.

## Bounded baseline sequence

Build Release first, then validate one title before launching the whole corpus:

```text
python tools/compat/run_corpus.py --manifest tools/compat/corpus.local.json --emulator _Build/compat-foundation/kyty_emulator.exe --output _CompatibilityRuns --title-id PPSA01325
```

Inspect its `command.json`, `run.json`, `diagnostics.json`, `stdout.txt`, and
`stderr.txt`. Confirm that no absolute path appears in `command.json`. Then run
the remaining titles one at a time so a host or driver failure cannot obscure
which installation caused it. The initial timeout is 120 seconds; increase it
only when the retained evidence shows normal progress near the boundary.

Repeat the same title IDs on Linux with Linux-local paths and the same
title-specific arguments. Record distribution, kernel, NVIDIA driver, Vulkan
device, display settings, and KytyPS5 commit. Never compare results from
different executable hashes as though they were the same build.

## PS5 observations and publication

Define one earliest repeatable route per title using
[ps5-comparison-protocol.md](ps5-comparison-protocol.md). Keep media private and
record only hashes, measurements, and defect notes in sanitized evidence.

Generate candidate public summaries with:

```text
python tools/compat/summarize_runs.py --input _CompatibilityRuns --json docs/compatibility/baseline.json --markdown docs/compatibility/baseline.md
```

Before committing, search both generated files for drive letters, home
directories, game paths, executable contents, and copied log payloads. Leave a
compatibility rung as `unknown` until the visual, audio, input, save/reload,
stability, and frame-time evidence supports it.
