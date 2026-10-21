# Runtime Refactor Implementation Plan

**Goal:** Reduce duplicated runtime configuration and resource lookup work across the repository, preserving existing behavior.

**Architecture:** Share the identical environment parsing operations while keeping each caller's existing caching and defaults. Route resource-slot access through a single checked lookup that selects its storage representation once.

**Tech stack:** C++20, clang-cl, CMake, CTest.

**Scope:** The user's repository-wide refactor request, prioritized toward runtime code and performance. Existing uncommitted changes are excluded.

## Constraints

- Preserve exact flag semantics, including unset, empty, `0`, `1`, and other strings.
- Preserve startup caching and live-switch behavior.
- Preserve slot generations, stable addresses, const access, and both lookup modes.
- Do not modify third-party code or existing user edits.
- Report benchmark results separately from game performance; no FPS claim without game measurements.

## Tasks

1. [x] Add standalone slot regression tests and an optional lookup benchmark. Establish a baseline with the original container.
2. [x] Add a small shared environment parser, table-driven tests, and migrate only equivalent flag expressions across runtime subsystems.
3. [x] Consolidate slot lookup validation and compare both modes against the baseline. Retain the change only with correct results and acceptable measured performance.
4. [x] Build affected runtime code and focused tests, review the complete diff, verify user edits remain intact, and prepare only this work for commit.

## Review focus

- Empty environment values are not interchangeable with absent values for every existing flag.
- A stale handle must stay invalid after its slot is reused.
- Slots crossing dense storage chunk boundaries must retain stable addresses.
- Const access must not expose mutable resources.
- Benchmark loops must perform observable lookups and report repeatable samples.

## Validation

Run `environment_tests`, `slot_vector_tests`, existing shared-container tests, and relevant available runtime tests through CTest. Build `kyty_emulator` to compile the migrated callers. Run the same optimized slot benchmark before and after. Run `git diff --check`, inspect staged changes, and compare hashes of the pre-existing modified files before committing.

## Results (2026-10-22)

- Migrated 96 readers in 47 runtime files. `ReadEnvFlag` treats only exact `0` as disabled, with an explicit default for missing values. `ReadNonEmptyEnvFlag` also rejects empty values. `ReadEnvEquals` requires an exact match. Existing function-local static and `Common::Once` caching remain at their original call sites; custom parsing rules remain unchanged.
- `SlotVector::try_get` now selects its storage mode once and checks one slot record. Const and mutable access, indexed access, and allocation queries share the checked lookup. Storage, generations, resource lifetimes, and synchronization contracts are unchanged.
- Windows Release builds passed for the emulator and 22 test executables, including the compute/renderer harness. All 39 selected CTest cases passed, including CPU utilities, memory/resource tracking, GPU image-cache modes, draw sequencing, command recording, uploads, rasterization, and program-cache checks. The engine-profile test also passed directly.
- Environment tests cover unset/empty values, exact matches, changed environment values, and 65,280 one/two-byte strings. Slot tests cover stable addresses across 4,097 entries, stale/reused/invalid handles in both modes, const access, iteration, and destruction.
- Both CI workflows now build and run the two new tests. Linux CI was updated but not executed locally.
- The migration was mechanically reversed and compared with the original source to confirm that only equivalent reader expressions and includes changed. Pre-existing modified files were hash-checked and excluded. Whitespace checks passed with `cr-at-eol` to accommodate the repository's existing CRLF sources.

### Lookup measurements

AMD Ryzen 7 5800X, Windows x64, clang-cl 23.1.2, Release `/O2`, no PGO. Each case performs four million lookups per sample, seven samples per process, with three alternating before/after process runs. Values below are the median of each process's median, in nanoseconds per `try_get` lookup. Checksums matched in all cases. The resources are 264 bytes and the mixed workload alternates valid and stale handles.

| Slots | Mode | Handles | Before ns | After ns | Change |
| --- | --- | --- | ---: | ---: | ---: |
| 4,096 | Standard | Valid | 7.405 | 5.126 | 30.8% lower |
| 4,096 | Standard | Mixed | 6.728 | 4.535 | 32.6% lower |
| 16,384 | Standard | Valid | 9.322 | 6.873 | 26.3% lower |
| 16,384 | Standard | Mixed | 8.851 | 6.324 | 28.6% lower |
| 4,096 | Dense | Valid | 5.462 | 4.917 | 10.0% lower |
| 4,096 | Dense | Mixed | 3.719 | 3.272 | 12.0% lower |
| 16,384 | Dense | Valid | 8.142 | 8.112 | Within 1% |
| 16,384 | Dense | Mixed | 7.155 | 7.207 | Within 1% |

Reproduce the current measurements with `slot_vector_tests --benchmark` from a Release build. Timing is informational, not a test threshold. This is a container microbenchmark; no game FPS improvement is claimed. The complete CTest matrix, Linux/macOS builds, and game-level profiling were not run.
