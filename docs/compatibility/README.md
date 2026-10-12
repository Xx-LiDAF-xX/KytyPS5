# Compatibility Evidence

This directory contains sanitized, versioned compatibility results. It never contains game files,
firmware, cryptographic keys, save data, captured proprietary payloads, or absolute local paths.

Copy `tools/compat/corpus.example.json` to the ignored `tools/compat/corpus.local.json`, replace the
placeholder with paths to games you lawfully own, and run:

```text
python tools/compat/run_corpus.py --manifest tools/compat/corpus.local.json --emulator _Build/windows/kyty_emulator.exe --output _CompatibilityRuns
python tools/compat/summarize_runs.py --input _CompatibilityRuns --json docs/compatibility/baseline.json --markdown docs/compatibility/baseline.md
```

Review generated files before committing them. A process outcome alone never proves a compatibility
rung; visual, audio, input, save/reload, stability, and frame-pacing evidence must be reviewed using
the [PS5 comparison protocol](ps5-comparison-protocol.md). The private setup and eight-title run
sequence are documented in [corpus.md](corpus.md).
