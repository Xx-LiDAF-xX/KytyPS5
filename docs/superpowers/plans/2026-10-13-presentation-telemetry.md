# Presentation Telemetry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist sanitized presented-frame and current-FPS evidence in compatibility diagnostics so a timed-out process can be distinguished from a title actively presenting frames.

**Architecture:** Extend the optional run-report metadata with a latest presentation snapshot and update it from the existing one-second window-title cadence. Each update atomically replaces the diagnostics file, so forced corpus termination retains recent progress without per-frame I/O. The summarizer copies only numeric presentation fields and still leaves compatibility rung assignment manual.

**Tech Stack:** C++20, nlohmann/json, SDL3/Vulkan presentation loop, Python 3 unittest.

**Spec:** `docs/superpowers/specs/2026-10-12-compatibility-program.md` and `docs/compatibility/ps5-comparison-protocol.md`

## Global Constraints

- Windows and Linux must emit the same additive schema fields.
- Diagnostics must remain opt-in and must not change guest-visible behavior when disabled.
- Commercial game frames, paths, executables, and captured proprietary payloads remain outside Git.
- Presented-frame telemetry is evidence of host presentation activity, not proof of rendering correctness or a compatibility rung.
- The update cadence remains the existing one-second title refresh; no per-frame report writes are allowed.

## Review Focus

- Disabled diagnostics: presentation updates must not create a file or enable diagnostics; covered by Task 1.
- Forced termination: every enabled presentation update must persist immediately; covered by Task 1.
- Non-finite or negative FPS input: serialize a finite non-negative value; covered by Task 1.
- Reports without presentation data: preserve the existing JSON shape and summarizer behavior; covered by Tasks 1 and 3.
- Misleading compatibility claims: summary output must expose frame evidence while retaining `compatibility_rung: unknown`; covered by Task 3.

---

### Task 1: Add a persisted presentation snapshot to run diagnostics

**Files:**
- Modify: `src/common/runDiagnostics.h`
- Modify: `src/common/runDiagnostics.cpp`
- Test: `tests/RunDiagnosticsTests.cpp`

**Interfaces:**
- Consumes: the existing `Diagnostics::Initialize`, lifecycle mutex, and atomic report replacement.
- Produces: `Diagnostics::PresentationMetadata`, `RunReport::presentation`, and `Diagnostics::UpdatePresentation(uint64_t presented_frames, double current_fps) noexcept`.

- [x] **Step 1: Write failing serialization and lifecycle tests**

Add tests asserting that an empty report omits `presentation`, an explicit snapshot serializes `presented_frames` and `current_fps`, an enabled `UpdatePresentation(120, 59.5)` immediately reaches disk, disabled updates are no-ops, and negative/non-finite FPS is stored as `0.0`.

- [x] **Step 2: Run the focused test and verify RED**

Run: `cmake --build _Build/compat-foundation --target run_diagnostics_tests --parallel && _Build/compat-foundation/run_diagnostics_tests.exe`

Expected: compilation fails because the presentation types and update API do not exist.

- [x] **Step 3: Implement the diagnostics API**

Add `struct PresentationMetadata { uint64_t presented_frames; double current_fps; };`, an optional field on `RunReport`, optional JSON serialization, and `UpdatePresentation(...)` that sanitizes FPS, updates only an enabled unfinished report, and calls the existing atomic writer while holding the lifecycle mutex.

- [x] **Step 4: Run the focused test and verify GREEN**

Run: `cmake --build _Build/compat-foundation --target run_diagnostics_tests --parallel && _Build/compat-foundation/run_diagnostics_tests.exe`

Expected: `RunDiagnosticsTests: all cases passed`.

- [x] **Step 5: Commit**

Commit: `feat(diagnostics): persist presentation progress`

### Task 2: Feed the snapshot from the presentation loop

**Files:**
- Modify: `src/graphics/presentation/window/window.cpp`
- Test: `tests/RunDiagnosticsTests.cpp` (API behavior remains the test seam)

**Interfaces:**
- Consumes: `Diagnostics::UpdatePresentation(uint64_t, double)` from Task 1 and the existing `WindowContext::UpdateTitle` one-second call site.
- Produces: live reports containing the same frame number and FPS displayed in the window title.

- [x] **Step 1: Add the diagnostics call at the existing one-second update seam**

Include `common/runDiagnostics.h` and call `Diagnostics::UpdatePresentation(frame_num, current_fps)` in `WindowContext::UpdateTitle` before the SDL title update.

- [x] **Step 2: Build and run the core diagnostics test**

Run: `cmake --build _Build/compat-foundation --target kyty_emulator run_diagnostics_tests --parallel && ctest --test-dir _Build/compat-foundation -R '^run_diagnostics$' --output-on-failure`

Expected: build succeeds and the diagnostics test passes.

- [x] **Step 3: Commit**

Commit: `feat(presentation): report frame progress`

### Task 3: Preserve frame evidence in sanitized summaries

**Files:**
- Modify: `tools/compat/summarize_runs.py`
- Test: `tools/compat/tests/test_summarize_runs.py`
- Modify: `docs/compatibility/README.md`

**Interfaces:**
- Consumes: optional diagnostics JSON `presentation.presented_frames` and `presentation.current_fps`.
- Produces: optional sanitized `presentation` data in each summary result plus `Frames` and `FPS` Markdown columns.

- [x] **Step 1: Write failing Python tests**

Add attempts with valid presentation data, malformed presentation data, and no presentation object. Assert valid numeric values survive, malformed values become `None`, missing data preserves old behavior, and `compatibility_rung` remains `unknown`.

- [x] **Step 2: Run the Python test and verify RED**

Run: `python -m unittest tools.compat.tests.test_summarize_runs -v`

Expected: the new presentation assertions fail.

- [x] **Step 3: Implement sanitized summary extraction and rendering**

Add a helper that accepts only non-boolean non-negative integer frame counts and finite non-negative numeric FPS values. Include the optional object in JSON results and render unknown values as `unknown` in Markdown.

- [x] **Step 4: Document the evidence boundary**

State that presentation telemetry proves only successful host presents and cannot award Rung 1 without visual evidence.

- [x] **Step 5: Run focused and full verification**

Run: `python -m unittest discover -s tools/compat/tests -v`, `ctest --test-dir _Build/compat-foundation -L core --output-on-failure`, and `ctest --test-dir _Build/compat-foundation -L vulkan --output-on-failure`.

Expected: all compatibility-tool, core, and Vulkan tests pass.

- [x] **Step 6: Run the clean eight-title corpus and regenerate the baseline**

Run the existing local corpus manifest, confirm each latest attempt contains presentation telemetry where frames were presented, then regenerate `docs/compatibility/baseline.json` and `.md` with the existing summarizer.

- [x] **Step 7: Commit**

Commit: `feat(compat): summarize presentation evidence`

## Self-Review Result

The plan covers opt-in lifecycle behavior, forced-timeout persistence, cross-platform presentation integration, sanitized summary propagation, documentation, and regression verification. It deliberately excludes screenshots and automatic rung assignment because the existing protocol requires private visual comparison evidence.
