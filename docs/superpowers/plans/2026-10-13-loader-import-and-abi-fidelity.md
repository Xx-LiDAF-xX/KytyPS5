# Loader Import and ABI Fidelity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make import resolution deterministic and non-fatal, prove unresolved function thunks obey the guest ABI, and retain bounded, sanitized unsupported-import evidence in compatibility reports.

**Architecture:** `SymbolDatabase` will distinguish unique NID fallback from ambiguity instead of returning the first registration. `RuntimeLinker` will prefer exact library/module matches, use only unique fallbacks, and route unresolved function calls through the existing generated thunk with focused ABI tests. `RunDiagnostics` will aggregate repeated unsupported-import calls, and the compatibility summarizer will publish counts without exposing local paths or proprietary payloads.

**Tech Stack:** C++20, CMake/CTest, x86-64 System V ABI bridges, nlohmann/json, Python 3 `unittest`.

**Spec:** `docs/superpowers/specs/2026-10-12-compatibility-program.md` (Program Workstream 3: loader, ABI, kernel, and guest-memory fidelity)

## Global Constraints

- Windows and Linux must use the same diagnostic schema and import-resolution behavior.
- Do not add an implementation for an observed NID until its function signature and behavior have lawful, reproducible evidence.
- Game binaries, modules, keys, captured proprietary payloads, save data, and absolute local paths remain outside Git.
- Diagnostics are opt-in, thread-safe, best-effort, and must not change guest-visible behavior when disabled.
- Exact library/module/version matches always take precedence over NID-only compatibility fallback.
- All new CPU-only tests are labeled `core`; no Vulkan device is required for this plan.

## Review Focus

- The same NID registered by two libraries must be reported as ambiguous, never resolved by insertion order; Task 1 adds duplicate-registration tests.
- Missing or malformed library/module identifiers must leave the import unresolved without terminating the emulator; Task 2 adds synthetic `Program` tests.
- Repeated unresolved calls from multiple guest threads must produce one bounded summary with an exact call count; Task 3 adds concurrent diagnostics tests.
- Generated unresolved-function thunks must return deterministic integer and floating zero values without corrupting the stack or nonvolatile registers; Task 4 adds native ABI fixtures.
- Import summaries containing absolute program paths or malformed JSON values must be sanitized or ignored by public reports; Task 5 adds C++ and Python path/malformed-input tests.

---

### Task 1: Add deterministic unique-NID lookup

**Files:**
- Create: `tests/SymbolDatabaseTests.cpp`
- Modify: `src/loader/symbolDatabase.h`
- Modify: `src/loader/symbolDatabase.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: existing `SymbolDatabase::Add`, `Find`, and `FindByNid` registrations.
- Produces: `enum class NidLookupStatus { NotFound, Unique, Ambiguous }`, `struct NidLookupResult { NidLookupStatus status; const SymbolRecord* record; }`, and `SymbolDatabase::FindUniqueByNid(const std::string&, SymbolType, uint64_t = 0) const`.

- [x] **Step 1: Write failing unique-NID lookup tests**

Add cases named `TestUniqueNidReturnsRecord`, `TestDuplicateNidIsAmbiguous`, `TestTypeAndAddressFiltersDisambiguate`, and `TestMissingNidIsNotFound`. Assert that an ambiguous result has `record == nullptr` and that exact `Find(SymbolResolve)` behavior remains unchanged.

- [x] **Step 2: Run the focused target and verify RED**

Run: `cmake --build _Build/compat-foundation --target symbol_database_tests --parallel 8`

Expected: compile failure because `NidLookupStatus`, `NidLookupResult`, and `FindUniqueByNid` do not exist.

- [x] **Step 3: Implement unique lookup and register the core test**

Scan all records matching NID, type, and optional address. Return `Ambiguous` after the second distinct match instead of retaining the first. Keep `FindByNid` for address-specific relocation bookkeeping, but do not use it for compatibility fallback after Task 2.

- [x] **Step 4: Build and verify GREEN**

Run: `cmake --build _Build/compat-foundation --target symbol_database_tests --parallel 8`
Run: `ctest --test-dir _Build/compat-foundation -R '^symbol_database$' --output-on-failure`

Expected: 1/1 focused test passes and the test is labeled `core`.

- [x] **Step 5: Commit**

```bash
git add CMakeLists.txt src/loader/symbolDatabase.h src/loader/symbolDatabase.cpp tests/SymbolDatabaseTests.cpp
git commit -m "feat(loader): detect ambiguous nid fallbacks"
```

### Task 2: Make runtime import resolution exact-first and non-fatal

**Files:**
- Create: `tests/RuntimeLinkerTests.cpp`
- Modify: `src/loader/runtimeLinker.cpp`
- Modify: `src/loader/runtimeLinker.h`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `SymbolDatabase::FindUniqueByNid` from Task 1 and existing `RuntimeLinker::Resolve` inputs.
- Produces: exact-first resolution with unique-NID fallback; unresolved output is always `{vaddr = 0, name = generated-or-original-name, dbg_name = ""}` without `EXIT` for missing metadata or ambiguous fallback.

- [x] **Step 1: Write failing synthetic resolution tests**

Construct `Program` fixtures with owned `DynamicInfo`, import libraries/modules, and synthetic symbol registrations. Add `TestExactMetadataWinsOverFallback`, `TestUniqueFallbackResolvesMissingExactRegistration`, `TestAmbiguousFallbackRemainsUnresolved`, `TestMissingLibraryOrModuleRemainsUnresolved`, and `TestMalformedSymbolNameRemainsUnresolved`.

- [x] **Step 2: Run the focused target and verify RED**

Run: `cmake --build _Build/compat-foundation --target runtime_linker_tests --parallel 8`
Run: `ctest --test-dir _Build/compat-foundation -R '^runtime_linker$' --output-on-failure`

Expected: the ambiguity assertion fails because current fallback returns the first matching NID; the missing-metadata case terminates in the current `EXIT` path.

- [x] **Step 3: Implement exact-first resolution**

Factor the NID-only path into a private `RuntimeLinker::ResolveUniqueNid(const std::string&, SymbolType, SymbolRecord*) const`. It must merge built-in and loaded-program candidates, return a record only when exactly one distinct candidate exists, and record a `loader/symbol_resolution_ambiguous` diagnostic event for ambiguity. Replace the missing library/module `EXIT` with a normal unresolved result.

- [x] **Step 4: Build and verify GREEN**

Run: `cmake --build _Build/compat-foundation --target runtime_linker_tests --parallel 8`
Run: `ctest --test-dir _Build/compat-foundation -R '^runtime_linker$' --output-on-failure`

Expected: 1/1 focused test passes on Windows and remains host-path independent.

- [x] **Step 5: Commit**

```bash
git add CMakeLists.txt src/loader/runtimeLinker.h src/loader/runtimeLinker.cpp tests/RuntimeLinkerTests.cpp
git commit -m "fix(loader): resolve imports without ambiguous nid binding"
```

### Task 3: Aggregate unsupported-import diagnostics

**Files:**
- Modify: `src/common/runDiagnostics.h`
- Modify: `src/common/runDiagnostics.cpp`
- Modify: `tests/RunDiagnosticsTests.cpp`

**Interfaces:**
- Consumes: symbol name, program path, symbol type, and binding text from the runtime linker.
- Produces: `uint64_t Diagnostics::RecordUnsupportedImport(std::string_view symbol, std::string_view program, std::string_view type, std::string_view binding) noexcept`; return value is the updated call count or zero when diagnostics are disabled.

- [x] **Step 1: Write failing serialization and lifecycle tests**

Add tests asserting that two calls for the same `(symbol, program leaf, type, binding)` serialize as one `unsupported_imports.entries` record with `call_count == 2`; concurrent calls retain the exact total; different programs remain separate; more than 256 unique keys increments `dropped_unique`; and an absolute program path serializes only its filename.

- [x] **Step 2: Run the focused target and verify RED**

Run: `cmake --build _Build/compat-foundation --target run_diagnostics_tests --parallel 8`

Expected: compile failure because `RecordUnsupportedImport` and the summary structures do not exist.

- [x] **Step 3: Implement the bounded summary**

Add `UnsupportedImport`, `UnsupportedImportSummary`, and `RunReport::unsupported_imports`. Keep at most 256 unique entries, increment existing counts without limit, track dropped unique keys, sanitize `program` with `filename()`, and serialize the optional root object only when it is non-empty. Schema version remains `1` because the field is additive.

- [x] **Step 4: Run the focused test and verify GREEN**

Run: `cmake --build _Build/compat-foundation --target run_diagnostics_tests --parallel 8`
Run: `ctest --test-dir _Build/compat-foundation -R '^run_diagnostics$' --output-on-failure`

Expected: 1/1 focused test passes, including disabled-mode and concurrency coverage.

- [x] **Step 5: Commit**

```bash
git add src/common/runDiagnostics.h src/common/runDiagnostics.cpp tests/RunDiagnosticsTests.cpp
git commit -m "feat(diagnostics): aggregate unsupported import calls"
```

### Task 4: Prove unresolved function-thunk ABI behavior

**Files:**
- Modify: `src/loader/runtimeLinker.cpp`
- Modify: `src/loader/runtimeLinker.h`
- Modify: `tests/RuntimeLinkerTests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Diagnostics::RecordUnsupportedImport` from Task 3 and existing generated x86-64 unresolved-import thunks.
- Produces: test-only `Loader::TestUnresolvedImportThunkAbi()` under `KYTY_RUNTIME_LINKER_TESTS`; production thunks preserve the System V stack/nonvolatile-register contract and return zero in both integer and scalar floating return registers.

- [x] **Step 1: Write the failing thunk ABI fixture**

Generate one unresolved function thunk, call it through integer-return and `double`-return System V function pointers with six populated arguments, and bracket the call with sentinels in `rbx`, `rbp`, and `r12-r15`. Assert integer zero, positive floating zero, 16-byte call alignment, unchanged sentinels, and an aggregated call count of two.

- [x] **Step 2: Run the focused target and verify RED**

Run: `cmake --build _Build/compat-foundation --target runtime_linker_tests --parallel 8`

Expected: compile failure because the test seam is absent; after adding only the seam, any ABI defect fails a named assertion rather than crashing silently.

- [x] **Step 3: Integrate aggregation and correct the thunk if required**

Replace the repeated generic `Diagnostics::Record` call with `RecordUnsupportedImport`. Print/checkpoint only on the first call and power-of-two counts through 1024 so diagnostics remain bounded. Preserve the existing per-relocation thunk identity and clear all thunk/summary test state during `RuntimeLinker::Clear`.

- [x] **Step 4: Build and verify GREEN**

Run: `cmake --build _Build/compat-foundation --target runtime_linker_tests --parallel 8`
Run: `ctest --test-dir _Build/compat-foundation -R '^runtime_linker$' --output-on-failure`

Expected: 1/1 focused test passes on the host; Linux CI exercises the same System V contract.

- [x] **Step 5: Commit**

```bash
git add CMakeLists.txt src/loader/runtimeLinker.h src/loader/runtimeLinker.cpp tests/RuntimeLinkerTests.cpp
git commit -m "fix(loader): verify unresolved import thunk abi"
```

### Task 5: Publish sanitized import summaries

**Files:**
- Modify: `tools/compat/summarize_runs.py`
- Modify: `tools/compat/tests/test_summarize_runs.py`

**Interfaces:**
- Consumes: optional `unsupported_imports` diagnostic object from Task 3.
- Produces: each summarized result has `unsupported_imports: {unique, calls, dropped_unique}` or `null`; Markdown gains `Unsupported imports` formatted as `N symbols / M calls`.

- [x] **Step 1: Write failing Python summary tests**

Add `test_unsupported_import_summary_is_preserved`, `test_malformed_import_summary_is_ignored`, and `test_import_program_paths_never_reach_public_outputs`. Include negative, Boolean, non-list, and non-string malformed values.

- [x] **Step 2: Run the focused suite and verify RED**

Run: `python -m unittest tools.compat.tests.test_summarize_runs -v`

Expected: failures because results and Markdown do not contain unsupported-import totals.

- [x] **Step 3: Implement sanitized summary extraction**

Validate counts as finite non-negative integers, recompute `unique` and `calls` from valid entries, retain only aggregate totals in public artifacts, and never copy symbol names or program paths into `baseline.json` or Markdown.

- [x] **Step 4: Run the focused suite and verify GREEN**

Run: `python -m unittest tools.compat.tests.test_summarize_runs -v`
Run: `python -m unittest discover -s tools/compat/tests -v`

Expected: all compatibility-tool tests pass and generated output contains no fixture path.

- [x] **Step 5: Commit**

```bash
git add tools/compat/summarize_runs.py tools/compat/tests/test_summarize_runs.py
git commit -m "feat(compat): summarize unsupported import evidence"
```

### Task 6: Validate the loader slice against the corpus

**Files:**
- Modify: `docs/compatibility/baseline.json`
- Modify: `docs/compatibility/baseline.md`
- Modify: `docs/superpowers/plans/2026-10-13-loader-import-and-abi-fidelity.md`

**Interfaces:**
- Consumes: all core tests, the local eight-title corpus, and the sanitized summarizer.
- Produces: a clean-build baseline with bounded unsupported-import totals and no loader-resolution regression.

- [ ] **Step 1: Run the complete core suite**

Run: `ctest --test-dir _Build/compat-foundation -L core --output-on-failure`

Expected: 100% of core tests pass, including `symbol_database`, `runtime_linker`, and `run_diagnostics`.

- [ ] **Step 2: Run the Python tool suites**

Run: `python -m unittest discover -s tools/compat/tests -v`
Run: `python -m unittest discover -s tools/shader/tests -v`
Run: `python -m unittest discover -s tools/gpu/tests -v`

Expected: 100% pass.

- [ ] **Step 3: Rebuild from a clean commit and run all eight titles**

Run: `cmake --build _Build/compat-foundation --target kyty_emulator --parallel 8`
Run: `python tools/compat/run_corpus.py --manifest tools/compat/corpus.local.json --emulator _Build/compat-foundation/kyty_emulator.exe --output _CompatibilityRuns`

Expected: each title reaches at least its previous presentation evidence; unresolved imports produce bounded summaries rather than repeated event growth. Do not interpret recurring `NpCppWebApi` NIDs as blockers without separate behavioral evidence.

- [ ] **Step 4: Regenerate and sanitize the baseline**

Run: `python tools/compat/summarize_runs.py --input _CompatibilityRuns --json docs/compatibility/baseline.json --markdown docs/compatibility/baseline.md --vulkan-validation true --shader-validation true`

Verify valid JSON, no absolute paths, no proprietary payloads, and aggregate import totals for every run that called an unresolved thunk.

- [ ] **Step 5: Commit**

```bash
git add docs/compatibility/baseline.json docs/compatibility/baseline.md docs/superpowers/plans/2026-10-13-loader-import-and-abi-fidelity.md
git commit -m "docs(compat): record loader import fidelity baseline"
```

---

## Self-Review Result

1. **Spec Coverage:** This plan covers the loader/import/ABI slice of Workstream 3. Kernel APIs and guest-memory pointer validation remain separate follow-on plans because they have independent interfaces and failure evidence.
2. **Step Scan:** Every implementation task has a focused failing test, an exact interface, a green verification command, and an independent commit.
3. **Type Consistency:** `FindUniqueByNid` feeds `ResolveUniqueNid`; `RecordUnsupportedImport` feeds the runtime thunk and the additive diagnostic object; the Python summarizer consumes only that object.
4. **Review Focus:** Duplicate NIDs, missing metadata, concurrent call aggregation, ABI return/register behavior, malformed summaries, and path sanitization all have named tests in their owning tasks.
5. **Proportion:** The plan avoids guessed commercial-library stubs and confines the slice to deterministic resolution, ABI safety, and evidence needed to plan actual library implementations.
