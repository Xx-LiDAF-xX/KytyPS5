# Compatibility Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the deterministic diagnostics and local corpus tooling needed to turn KytyPS5 game failures into reproducible, comparable, regression-ready evidence on Windows and Linux.

**Architecture:** Add an opt-in `Diagnostics` module to the existing `common` library and expose a small interface for metadata, lifecycle events, unsupported imports, fatal failures, and final outcomes. Keep orchestration outside the emulator in Python: a schema-validated local corpus manifest launches isolated emulator runs and summarizes sanitized report files without storing proprietary payloads.

**Tech Stack:** C++20, nlohmann JSON, CMake/CTest, Python 3 standard library, GitHub Actions, Vulkan 1.3 metadata already exposed by the graphics context.

**Spec:** `docs/superpowers/specs/2026-10-12-compatibility-program.md`

## Global Constraints

- Primary operating systems are Windows and Linux.
- Reference hardware is Ryzen 7 5800X, RTX 4060 Ti 16 GB, and 32 GB DDR4-3200.
- Diagnostic collection is opt-in and must not alter guest-visible behavior when disabled.
- Game paths, executables, patches, keys, saves, and proprietary captures never enter Git.
- Committed outputs contain no absolute user paths or proprietary payloads.
- Preserve GPL-2.0-only notices and all third-party license obligations.
- Every behavior change follows a failing-test, minimal-fix, passing-test cycle.

## Review Focus

- A fatal error before diagnostics initialization must preserve the normal text failure and must not recurse or deadlock; covered by Task 2.
- Two threads recording events concurrently must produce valid JSON with both event streams; covered by Task 2.
- Paths containing spaces, Unicode, and user-directory prefixes must launch correctly and serialize only sanitized labels; covered by Task 4.
- A hung child process must be terminated after the configured timeout and classified as `timeout`; covered by Task 4.
- A report missing optional GPU metadata or terminated before finalization must still summarize deterministically; covered by Task 5.

---

### Task 1: Define and serialize the run-report model

**Files:**
- Create: `src/common/runDiagnostics.h`
- Create: `src/common/runDiagnostics.cpp`
- Create: `tests/RunDiagnosticsTests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `Diagnostics::RunMetadata`, `Diagnostics::Event`, `Diagnostics::Outcome`, `Diagnostics::RunReport`, `Diagnostics::Serialize(const RunReport&) -> std::string`, and `Diagnostics::WriteReport(const RunReport&, const std::filesystem::path&, std::string*) -> bool`.
- `RunMetadata` contains schema version `1`, build label, platform, CPU, sanitized title metadata, configuration values, and optional GPU/driver fields.
- `Event` contains a monotonic microsecond offset, category, code, message, and numeric thread identifier.
- `Outcome` is one of `Running`, `Completed`, `Fatal`, `HostException`, `Timeout`, or `LaunchError`.

- [x] **Step 1: Write the serialization tests**

  Add tests named `serializes_required_metadata`, `omits_empty_optional_gpu_fields`, `creates_parent_directory`, and `does_not_serialize_source_paths`. Assert schema version `1`, stable field names, atomic replacement of the destination file, and absence of the supplied absolute test path.

- [x] **Step 2: Run the new test target and verify it fails**

  Run: `cmake --build <build-dir> --target run_diagnostics_tests && ctest --test-dir <build-dir> -R '^run_diagnostics$' --output-on-failure`

  Expected: build failure because `common/runDiagnostics.h` does not exist.

- [x] **Step 3: Implement the report data model and JSON serialization**

  Implement the exact interfaces above using `nlohmann::ordered_json`. Write through a sibling temporary file and rename it only after the complete JSON document is flushed.

- [x] **Step 4: Register and run the test**

  Add `run_diagnostics_tests`, register CTest name `run_diagnostics`, label it `core`, and run the command from Step 2.

  Expected: PASS.

- [x] **Step 5: Commit**

  ```bash
  git add CMakeLists.txt src/common/runDiagnostics.h src/common/runDiagnostics.cpp tests/RunDiagnosticsTests.cpp
  git commit -m "test: define structured compatibility reports"
  ```

### Task 2: Add the thread-safe diagnostics lifecycle

**Files:**
- Modify: `src/common/runDiagnostics.h`
- Modify: `src/common/runDiagnostics.cpp`
- Modify: `tests/RunDiagnosticsTests.cpp`

**Interfaces:**
- Consumes: Task 1 report types and serializer.
- Produces: `Diagnostics::Initialize(const std::filesystem::path&, RunMetadata)`, `Diagnostics::Enabled() -> bool`, `Diagnostics::Record(std::string_view, std::string_view, std::string_view)`, `Diagnostics::SetGpu(std::string_view, uint32_t, uint32_t, uint32_t)`, `Diagnostics::Finish(Outcome, std::string_view)`, and `Diagnostics::Shutdown()`.
- Calls before initialization and after shutdown are no-ops. `Finish` is idempotent and best-effort.

- [x] **Step 1: Write lifecycle and concurrency tests**

  Add tests named `disabled_calls_are_noops`, `concurrent_events_are_retained`, `finish_is_idempotent`, `fatal_before_initialize_is_safe`, and `shutdown_flushes_running_report`.

- [x] **Step 2: Run `run_diagnostics` and verify the new tests fail**

  Expected: compilation failures for the missing lifecycle functions.

- [x] **Step 3: Implement the lifecycle behind one private process-wide state**

  Protect mutable state with a mutex, derive event offsets from `std::chrono::steady_clock`, and preserve the first terminal outcome. Do not throw from `Record`, `Finish`, or `Shutdown`.

- [x] **Step 4: Run `run_diagnostics`**

  Expected: PASS, including the concurrent recorder test.

- [x] **Step 5: Commit**

  ```bash
  git add src/common/runDiagnostics.h src/common/runDiagnostics.cpp tests/RunDiagnosticsTests.cpp
  git commit -m "feat: add compatibility diagnostics lifecycle"
  ```

### Task 3: Integrate diagnostics with emulator execution

**Files:**
- Modify: `src/common/emulatorConfig.h`
- Modify: `src/main.cpp`
- Modify: `src/emulator.cpp`
- Modify: `src/common/assert.cpp`
- Modify: `src/loader/runtimeLinker.cpp`
- Modify: `src/graphics/presentation/window/vulkanWindow.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 lifecycle.
- Produces: CLI option `--diagnostics-output <json>`, `ConfigOptions::diagnostics_output_file`, lifecycle events for initialization and guest execution, `unsupported_import` events, fatal outcomes, and selected Vulkan device metadata.

- [x] **Step 1: Add failing CLI tests**

  Register `emulator_diagnostics_cli` for `kyty_emulator --help --diagnostics-output report.json` and `emulator_diagnostics_missing_value` with `WILL_FAIL TRUE`.

- [x] **Step 2: Run the CLI tests and verify failure**

  Run: `ctest --test-dir <build-dir> -R '^emulator_diagnostics_' --output-on-failure`

  Expected: the option is rejected or absent from help.

- [x] **Step 3: Parse the diagnostics option and initialize metadata**

  Initialize diagnostics after config/log setup and `param.json` loading. Populate build, platform, CPU, title ID, localized title, content version, and non-secret emulator configuration. Store only the title ID and filename leaf for path-valued configuration.

- [x] **Step 4: Record emulator milestones and Vulkan device metadata**

  Record subsystem initialization completion, program load, guest execution start, selected Vulkan device name, vendor ID, device ID, and driver version.

- [x] **Step 5: Record unsupported imports and fatal outcomes**

  Record the symbol NID/name and program filename leaf in `UnresolvedImportStub`. Call `Diagnostics::Finish(Outcome::Fatal, ...)` from the fatal-report path before emergency shutdown; preserve the existing text log and exit behavior.

- [x] **Step 6: Run focused and full core tests**

  Run: `ctest --test-dir <build-dir> -R '^(run_diagnostics|emulator_diagnostics_)' --output-on-failure`

  Then run: `ctest --test-dir <build-dir> -L core --output-on-failure`

  Expected: PASS.

- [x] **Step 7: Commit**

  ```bash
  git add CMakeLists.txt src/common/emulatorConfig.h src/main.cpp src/emulator.cpp src/common/assert.cpp src/loader/runtimeLinker.cpp src/graphics/presentation/window/vulkanWindow.cpp
  git commit -m "feat: emit structured emulator diagnostics"
  ```

### Task 4: Implement the local compatibility corpus runner

**Files:**
- Create: `tools/compat/corpus.schema.json`
- Create: `tools/compat/corpus.example.json`
- Create: `tools/compat/run_corpus.py`
- Create: `tools/compat/tests/test_run_corpus.py`
- Modify: `.gitignore`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 3 `--diagnostics-output` option.
- Produces: `run_corpus.py --manifest <json> --emulator <path> --output <dir> [--title-id <id>]`; exit classification `completed`, `fatal`, `host_exception`, `timeout`, or `launch_error`; one isolated directory per title and attempt.
- Manifest fields: schema version `1`, title ID, local game path, optional patch path, timeout seconds, repeat count, and emulator arguments.

- [x] **Step 1: Write runner tests with a temporary fake emulator**

  Cover valid launch, paths with spaces and Unicode, invalid title ID, missing game path, timeout termination, nonzero exit, repeated attempts, and absence of local absolute paths from generated public summaries.

- [x] **Step 2: Run the Python tests and verify failure**

  Run: `python -m unittest discover -s tools/compat/tests -v`

  Expected: import failure because `run_corpus.py` does not exist.

- [x] **Step 3: Implement manifest validation and process execution**

  Use only Python's standard library. Pass emulator arguments as an argument array, never through a shell. Write stdout, stderr, command metadata with paths replaced by stable labels, and diagnostics output into the attempt directory.

- [x] **Step 4: Ignore local corpus data**

  Add `/tools/compat/corpus.local.json` and `/_CompatibilityRuns/` to `.gitignore`. The example manifest contains placeholders only.

- [x] **Step 5: Register and run the Python tests**

  Register CTest `compat_corpus_tool` with label `core`, then run both the Python command and `ctest -R '^compat_corpus_tool$'`.

  Expected: PASS.

- [x] **Step 6: Commit**

  ```bash
  git add .gitignore CMakeLists.txt tools/compat
  git commit -m "feat: add local compatibility corpus runner"
  ```

### Task 5: Generate sanitized compatibility summaries

**Files:**
- Create: `tools/compat/summarize_runs.py`
- Create: `tools/compat/tests/test_summarize_runs.py`
- Create: `docs/compatibility/README.md`
- Create: `docs/compatibility/baseline.json`
- Create: `docs/compatibility/baseline.md`

**Interfaces:**
- Consumes: Task 4 attempt directories and Task 1 report schema.
- Produces: `summarize_runs.py --input <dir> --json <file> --markdown <file>` and one result per title/version/host/attempt.

- [x] **Step 1: Write summary tests**

  Cover missing diagnostics, incomplete running reports, optional GPU fields, mixed attempts, deterministic ordering by title ID, and path sanitization.

- [x] **Step 2: Run the Python tests and verify failure**

  Expected: import failure because `summarize_runs.py` does not exist.

- [x] **Step 3: Implement summary generation**

  Emit counts and latest outcomes without inferring a compatibility rung from a process exit alone. Use `unknown` when evidence is incomplete.

- [x] **Step 4: Add empty committed baseline artifacts**

  Seed both baseline files with schema version `1`, the eight title IDs, and `not_tested` outcomes. Do not add local paths, proprietary metadata, or guessed game versions.

- [x] **Step 5: Run all tool tests**

  Run: `python -m unittest discover -s tools/compat/tests -v`

  Expected: PASS.

- [x] **Step 6: Commit**

  ```bash
  git add tools/compat docs/compatibility
  git commit -m "feat: summarize compatibility evidence"
  ```

### Task 6: Classify and expand continuous testing

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `.github/workflows/build.yml`
- Create: `docs/testing.md`

**Interfaces:**
- Consumes: all registered CTest targets.
- Produces: CTest labels `core`, `vulkan`, and `platform`; Windows/Linux CI commands that build and execute every `core` test; documented local commands for Vulkan tests.

- [x] **Step 1: Inventory every registered CTest test**

  Record each test's label and required host capability in `docs/testing.md`. No test may remain unlabeled.

- [x] **Step 2: Add labels and verify the inventory**

  Run: `ctest --test-dir <build-dir> --show-only=json-v1`

  Expected: every test contains at least one of `core`, `vulkan`, or `platform`.

- [x] **Step 3: Change Windows and Linux CI to build the `kyty_tests` aggregate and run `core`**

  Replace hand-maintained regular-expression subsets with `ctest --test-dir ... -L core --output-on-failure`. Preserve platform-only checks and do not schedule Vulkan tests on workers without a validated Vulkan device.

- [x] **Step 4: Run the same core command locally**

  Expected: all `core` tests pass.

- [x] **Step 5: Commit**

  ```bash
  git add CMakeLists.txt .github/workflows/build.yml docs/testing.md
  git commit -m "ci: run the complete core test suite"
  ```

### Task 7: Define the PS5 comparison and eight-title baseline procedure

**Files:**
- Create: `docs/compatibility/ps5-comparison-protocol.md`
- Create: `docs/compatibility/corpus.md`
- Modify: `docs/compatibility/baseline.json`
- Modify: `docs/compatibility/baseline.md`

**Interfaces:**
- Consumes: Tasks 4 and 5 tooling plus locally owned PS5 and game installations.
- Produces: reproducible route descriptions, exact local title/version/hash records, Windows/Linux baseline results, and PS5 observation records without proprietary payloads.

- [x] **Step 1: Document the comparison protocol**

  Specify console/game/display settings, cold-start state, input sequence, scene checkpoint, observation duration, screenshot/video naming, frame-time sampling, audio sync observation, and allowed variance.

- [x] **Step 2: Create the private local corpus manifest**

  Copy `corpus.example.json` to ignored `corpus.local.json`, enter the eight owned game paths, and record exact metadata read from each `sce_sys/param.json` plus SHA-256 of the executable.

- [x] **Step 3: Perform one bounded Windows baseline run per title**

  Run each title with Vulkan and shader validation enabled where supported. Preserve every attempt directory, including launch errors and timeouts.

- [ ] **Step 4: Repeat the same corpus on Linux**

  Use the same manifest schema and title-specific arguments. Record distribution, kernel, NVIDIA driver, and Vulkan device metadata through the diagnostic report.

- [ ] **Step 5: Record matching PS5 observations**

  Follow the protocol for the earliest repeatable route available in each title. Store media privately; commit only hashes, observation notes, and non-copyrightable measurements.

- [x] **Step 6: Generate and review the sanitized baseline**

  Run `summarize_runs.py`, inspect both output files for absolute paths or protected content, and commit only the sanitized results.

- [x] **Step 7: Commit**

  ```bash
  git add docs/compatibility
  git commit -m "docs: establish the initial compatibility baseline"
  ```

## Self-Review Result

- The plan covers every phase-one requirement in the specification.
- GPU command replay and shader reduction are intentionally deferred to the next plan because their artifact format depends on evidence collected here.
- Individual game fixes are intentionally deferred until the baseline identifies the first reproducible failure clusters.
- Every implementation task has a focused failing test, verification command, and independently reviewable commit.
- The report and runner interfaces use consistent schema version `1`, outcome names, and path-sanitization rules.
