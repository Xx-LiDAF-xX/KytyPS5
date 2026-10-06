# KYTYPS5 PERFORMANCE ANALYSIS

Follow-up to `2026-10-07-baseline-analysis.md`. Investigation date: October 7, 2026.
This report records builds, diagnostics and a fresh game launch. No optimization is validated.

GAME: Astro Bot; supporting captured shader: Astro's Playroom.

GAME ID: PPSA21567; supporting PPSA01325.

EMULATOR COMMIT: `1d3e48cab6277ea399c0efd75f7eb2ca9e0e8835`, extensive preexisting dirty changes.
Diagnostic executable SHA-256: `3D35C0F582477077FBAD839C854F30BD3D42A10519AF45D8EB488314EC039F7A`.
Build: Release, clang-cl. No commit created.

OS: Windows 11 Pro 10.0.26200. CPU: Ryzen 7 5800X, 8 cores / 16 threads.

GPU: RTX 4060 Ti, PCI vendor 10DE / device 2805. DRIVER: Windows 32.0.16.1714.

RAM: 33,460,184 KiB visible (~31.91 GiB); process usage unmeasured. VRAM: capacity/budget unmeasured.

RESOLUTION: Quality/native preset; exact internal/presentation extents unmeasured.

CACHE STATE: isolated probe directory; driver cache uncontrolled. Translator source changes may
invalidate emulator caches. Do not classify this as a controlled cold/warm comparison.

TARGET FPS: 60 (16.67 ms/frame); 30/120 targets remain unassessed.

## 1. CURRENT PERFORMANCE

The extracted game at `C:\ROMS\PPSA21567` launches. Fresh diagnostic run exits 321 after
17.921 seconds, before a representative gameplay benchmark. This is a correctness blocker.
Average FPS, median, 1%/0.1% lows, P95/P99, exact maximum, standard deviation, CPU/GPU frame time,
CPU/per-thread utilization, GPU busy time/utilization and process RAM/VRAM usage: unmeasured for gameplay.
Synchronous tracing and validation make this run unsuitable for performance comparisons.

## 2. BOTTLENECK CLASSIFICATION

Primary blocker: shader resource planning / CPU guest-memory dereference, HIGH confidence.
Secondary performance candidates remain pipeline creation and synchronization from the earlier report;
they are not quantified by this probe. Confidence in the ultimate semantic cause: MEDIUM.
The fresh failure occurs with ray tracing enabled, strict readback, speculative draws disabled,
timestamp scale 100, and pipeline libraries disabled. Pipeline lookahead alone cannot explain it.

## 3. FRAME-TIME BREAKDOWN

Guest CPU, kernel/HLE, render submission, shader translation, pipeline creation, GPU execution,
synchronization, streaming and other exclusive frame costs: unmeasured in this startup diagnostic.
Do not add nested drain timers or interpret the instrumented 17.921 seconds as a frame-time sample.

## 4. ROOT CAUSE

The invalid producer is now localized. Compute shader `78af8e269b528b5c` executes these resource-plan reads:

| Native PC | Instruction/read | Live diagnostic evidence |
| --- | --- | --- |
| `0x1F5C` | `S_CBRANCH_EXECZ` to `0x2A54` | Pointer chain is behind an execution-mask branch |
| `0x1F60` | `S_LOAD_DWORDX2 s8, s0`, offset 0 | Read at `0x50CC3D300` returns two zero dwords |
| `0x1F90` | `S_LOAD_DWORDX2 s10, s8`, offset 48 | CPU flat-run read uses base 0, address `0x30`, 2 dwords |

`ClampRangeSize` rejects the final 8-byte access at `memory.cpp:984`.
The preserved baseline binary/PDB symbolizes the earlier stack through resource refresh,
compute-program lookup and PM4 processing. The original PDB GUID/age matches its executable.

Offline replay of the new capture attempts `0x30` in both compiled and reference walkers.
Playroom's captured shader `5df28d2058caf728` similarly attempts `0x28` at PC `0xF5C` in both walkers.
Therefore flat-run batching is not the sole source of the invalid read.

Hypothesis: planning elevates GPU-controlled scalar pointer data into CPU descriptor/resource refresh
without proving the native path executes. An opt-in synthetic reproducer demonstrates this eagerness.
This does not prove that the live GPU execution mask is zero, that the guest pointer is valid on its
active path, or that memory visibility is correct. Those alternatives still need investigation.
The captured logs omit some specialization reads; neither replay is a complete memory oracle.

## 5. PROPOSED FIX

Separate scalar execution data from host descriptor dependencies, and only hoist reads when resource
binding and native control-flow semantics permit it. Preserve guarded runtime pointer loads on the GPU
where supported. Dynamic scalar-buffer descriptors require additional lowering work and validation.
Do not insert null-to-zero reads or skip ray-tracing dispatches.
A narrow attempt to remove opportunistic planning was insufficient for the real shader and failed a
resource regression. It was reverted; `ResourceTracking.cpp` has no retained change from this investigation.

## 6. FILES/FUNCTIONS TO MODIFY

Retained diagnostics:

- `src/graphics/shader/recompiler/ir/passes/SrtWalker.cpp`: `TraceResourceRead`, `BuildRawRead`,
  both `EvaluateRawRead` implementations and `ReadFlatRun` retain/report native PC before a read.
- `tests/CapturedSrtReplay.inc`: `ReplayCapturedSrt`, a captured-data-only compute-shader diagnostic.
- `tests/shaderCfgTests.cpp`: replay CLI and `TestGpuGuardedScalarPointerChain` opt-in reproducer.

Future fix investigation: `ResourceTracking.cpp::PlanScalarReads` / `CollectScalarRead`, descriptor
collection, `SrtWalker.cpp::FindActiveSources`, and scalar-address/indirect-buffer SPIR-V lowering.
Keep `src/kernel/memory.cpp::ClampRangeSize` checks intact.

## 7. PATCH

Diagnostics are implemented; no performance or crash fix remains. Set `KYTY_TRACE_SRT_READS=1` before
process launch to log shader hash, stage, native PC, base, final address and word count to stderr.
The setting is cached at first use and disabled by default. Enabled logging flushes each read.

The replay CLI accepts `--replay-srt <shader.bin> <stdout.log>`. It uses the final captured compute input,
separate ordinary/clean read maps, rejects conflicting captured dwords, and fails on missing data.
It never dereferences guest addresses. It uses default compute metadata beyond captured user data/hash;
the caller must supply the matching shader. It is diagnostic, not a complete production replay harness.

`--scalar-pointer-chain-only` runs the new synthetic reproducer. It currently fails the expectation that
a GPU-controlled scalar pointer chain remain outside CPU resource refresh. It is intentionally opt-in,
not registered in the default passing regression suites. Do not report it as a passing crash-fix test.
Preserve preexisting edits when removing these diagnostic additions.

## 8. WHY THIS SHOULD HELP

Native PCs connect invalid host reads to guest shader instructions, replacing ambiguous optimized
stack symbols with direct provenance. Both walkers can be compared without launching the game or
accessing guest memory. This narrows the architectural investigation; FPS benefit remains unknown.

## 9. CORRECTNESS RISKS

Do not confuse unknown GPU predicates with false predicates. Removing host planning indiscriminately
can lose descriptor identity, native shared-exit sources and specialization semantics. A valid correction
must preserve memory ordering and descriptor/control dependencies. Diagnostics change source/cache
identity and tracing perturbs timing; disabled overhead has not been benchmarked.
The live log reports an AMD CPU instruction compatibility patch; exact built-in game patch identity
and extracted-game version must also be captured for a future controlled comparison.

## 10. VENDOR IMPACT

NVIDIA: the fresh failure reproduces on this RTX 4060 Ti host. AMD and Intel GPU paths were not run.
The suspected planning issue is shared compiler architecture; cross-vendor impact remains unverified.
No vendor-specific path or workaround was introduced.

## 11. TEST PROCEDURE

Evidence directory: `_Build/rt/probes/srt-origin-02`, containing `result.json`, launch arguments,
stdout/stderr and captured shaders. `result.json` records all settings, runtime, exit and executable hash.
Baseline executable/PDB preserved in `_Build/rt/crash-baseline-20261007` before rebuilding.

Build diagnostic emulator and `shader_cfg_tests` with the existing `_Build/rt` configuration.
Run `ctest --test-dir _Build/rt -R '^shader_(resource_control|ray_tracing_ir)$' --output-on-failure`.
Run the captured shader with `--replay-srt` and tracing enabled; inspect native PC/address provenance.
The new Astro replay outputs are `_Build/rt/astro-live-srt-replay-{stdout,stderr}.log`.

Before accepting a fix, obtain all compile inputs and specialization reads, establish native branch
reachability and memory visibility, make the synthetic regression pass, and retest real shaders and games.
Then follow the baseline report's matched scene/cold/warm benchmark procedure with tracing disabled.

## 12. ACCEPTANCE CRITERIA

The real pointer chain must behave according to native shader execution without bypassing range checks.
Both walkers must agree under complete captures; descriptor/shared-exit and scalar-buffer tests must pass.
Astro Bot must proceed beyond this failure and show correct ray-traced output under validation.
Performance acceptance requires matched, repeated gameplay frame intervals and resource measurements;
none are available yet. No 30/60/120 FPS claim is justified.

## 13. REGRESSION CHECK

Emulator and diagnostic test target build successfully. Latest focused run: shader_resource_control and
shader_ray_tracing_ir pass, 2/2. Full shader_cfg fails the existing mesh-prolog input-assembly/wave-count/
vertex-ID/instance-ID assertion, also recorded in the earlier full-suite log. The suite is not clean.
The opt-in new scalar-pointer reproducer fails as described above. Captured replay exits 1 because data
is incomplete: Astro compiled missing=5/low-address=2; reference missing=4/low-address=1.
These counts reflect evaluator request patterns, not distinct crashes or equivalent completed runs.

Live Playroom and additional titles, Linux, AMD/Intel GPUs, screenshots, audio and long-duration tests
remain unverified. No correctness or speedup conclusion may be drawn from the focused passing tests alone.

## 14. NEXT BOTTLENECK

Resolve native execution/descriptor planning for this chain first. Once stable gameplay is available,
measure pipeline creation/lookahead exclusive costs and DCC/readback synchronization as prioritized
in the baseline report. Collect exact frame intervals and per-thread/GPU traces before optimizing.
