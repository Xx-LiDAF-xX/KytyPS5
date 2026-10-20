# Engine-Aware Optimization Subsystem Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement an automatic engine detection and optimization subsystem in KytyPS5 C++ core with tailored runtime presets and flags for 17 PlayStation Studios and third-party game engine families (including Rockstar Advanced Game Engine - RAGE).

**Architecture:** A centralized `Common::EngineProfile` module inspects the `/app0` filesystem structure, package manifests, and title IDs to fingerprint the running game engine family. It maps the detected engine to tailored performance flags (`KYTY_*` settings, Vulkan queue submission modes, BDA indexing, and BVH/RT heuristics) and safely injects them into the runtime environment prior to renderer initialization while respecting explicit user overrides.

**Tech Stack:** C++20, STL (`<filesystem>`, `<string_view>`, `<vector>`), fmt, nlohmann_json, CMake.

**Spec:** [`docs/superpowers/specs/2026-10-20-engine-aware-optimization-subsystem.md`](file:///c:/Users/TAPIOCA/Desktop/repos/KytyPS5/docs/superpowers/specs/2026-10-20-engine-aware-optimization-subsystem.md)

## Global Constraints

- Must compile under MSVC/Clang-CL and GCC/Clang on Windows and Linux without exceptions in core modules.
- Header files must follow the existing style and namespace conventions (`Common::EngineProfile`).
- Detection must be fast (bounded directory scan, non-recursive beyond 2 levels) and never crash on missing/unreadable files.
- Explicit user settings (command line or pre-existing environment variables) must strictly override automatic engine presets.

## Review Focus

1. **Missing / Malformed /app0 directory**: Must cleanly fall back to `EngineFamily::Unknown` without throwing or aborting.
2. **Ambiguous Engine Signatures**: Titles with generic layouts must reliably match Title ID tables or safely fall back to `PlayStationStudiosGeneric` without misidentifying engines.
3. **User Overrides Precedence**: If a user sets `KYTY_RT_SOFTWARE=0`, an engine profile specifying `KYTY_RT_SOFTWARE=1` must not clobber the user's setting.
4. **Case-Insensitive File Matching**: Windows paths and PS5 casing variations (`uecommandline.txt` vs `UECommandLine.txt`, `engine/` vs `Engine/`) must resolve correctly.
5. **JSON Preset Integrity**: All 17 standalone preset files in `tools/engine-presets/` must conform to `u59-preset.json` schema (`KYTY_` / `TRACY_` keys only, string values only).

---

### Task 1: Core Engine Profile Module (`src/common/engineProfile.h`, `src/common/engineProfile.cpp`)

**Files:**
- Create: `src/common/engineProfile.h`
- Create: `src/common/engineProfile.cpp`

**Interfaces:**
- Produces:
  - `enum class EngineFamily : uint8_t` (covering 17 engines + Unknown)
  - `struct EngineDetectionResult { EngineFamily family; std::string engine_name; std::string detected_reason; std::vector<Common::U59Preset::Entry> tuning_overrides; }`
  - `std::string_view EngineFamilyToString(EngineFamily family)`
  - `EngineFamily StringToEngineFamily(std::string_view name)`
  - `EngineDetectionResult DetectAndProfile(const std::filesystem::path& app0_dir, const std::string& title_id, const std::string& app_name)`
  - `std::vector<Common::U59Preset::Entry> GetEnginePresetOverrides(EngineFamily family)`
  - `void ApplyEngineProfile(const EngineDetectionResult& result, bool log_changes = true)`

- [x] **Step 1: Declare `EngineFamily`, data structures, and signatures in `src/common/engineProfile.h`**
- [x] **Step 2: Implement engine fingerprinting heuristics in `src/common/engineProfile.cpp`**:
  - File checks: `uecommandline.txt`, `/engine`, `Engine/Binaries` -> `UnrealEngine5`
  - Decima markers: `ds2.bin`, `core` -> `Decima`
  - Insomniac markers: `toc` + `*.cache` -> `Insomniac`
  - Rockstar markers: `.rpf` archives -> `Rage`
  - Title ID tables: `PPSA21567` -> `TeamAsobi`, `PPSA01325` -> `Bluepoint`, `PPSA03420` -> `Rage`, etc.
  - Sucker Punch, Naughty Dog, Polyphony, Santa Monica, San Diego, BubbleBath, Destiny/Tiger, Valkyrie, PhyreEngine, Japan Studio, PlayStation Studios, RAGE.
- [x] **Step 3: Implement tuning overrides generator `GetEnginePresetOverrides(EngineFamily)`** with the exact settings specified in the matrix.
- [x] **Step 4: Implement `ApplyEngineProfile`** which iterates over entries and calls `_putenv_s` / `setenv` only when the variable is not already explicitly set.

---

### Task 2: Standalone Engine Presets Catalog (`tools/engine-presets/*.json`)

**Files:**
- Create: `tools/engine-presets/ue5.json`
- Create: `tools/engine-presets/decima.json`
- Create: `tools/engine-presets/insomniac.json`
- Create: `tools/engine-presets/naughty-dog.json`
- Create: `tools/engine-presets/sucker-punch.json`
- Create: `tools/engine-presets/santa-monica.json`
- Create: `tools/engine-presets/polyphony.json`
- Create: `tools/engine-presets/team-asobi.json`
- Create: `tools/engine-presets/bluepoint.json`
- Create: `tools/engine-presets/san-diego.json`
- Create: `tools/engine-presets/bubblebath.json`
- Create: `tools/engine-presets/tiger.json`
- Create: `tools/engine-presets/valkyrie.json`
- Create: `tools/engine-presets/phyreengine.json`
- Create: `tools/engine-presets/japan-studio.json`
- Create: `tools/engine-presets/playstation-studios.json`
- Create: `tools/engine-presets/rage.json`

**Interfaces:**
- Conforms to `Common::U59Preset::Parse` validator schema.

- [x] **Step 1: Write all 17 engine JSON preset files in `tools/engine-presets/`** with verified `KYTY_*` settings.
- [x] **Step 2: Verify each JSON file contains valid syntax and keys**.

---

### Task 3: Comprehensive Unit Tests & Build Integration (`tests/EngineProfileTests.cpp`, `CMakeLists.txt`)

**Files:**
- Create: `tests/EngineProfileTests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Common::EngineProfile` functions.
- Produces: Executable `engine_profile_tests`.

- [x] **Step 1: Write `tests/EngineProfileTests.cpp`** exercising:
  - Mock directory detection for all 16 engine families.
  - Title ID detection priority.
  - String conversion consistency.
  - Environment variable application without overwriting existing variables.
  - Validation of all JSON files in `tools/engine-presets/` via `Common::U59Preset::Parse`.
- [x] **Step 2: Register `engine_profile_tests` in root `CMakeLists.txt`** linking against `common`.

---

### Task 4: Emulator Startup Hook Integration (`src/emulator.cpp`)

**Files:**
- Modify: `src/emulator.cpp`

**Interfaces:**
- Consumes: `Common::EngineProfile::DetectAndProfile`, `Common::EngineProfile::ApplyEngineProfile`.

- [x] **Step 1: Include `common/engineProfile.h` in `src/emulator.cpp`**.
- [x] **Step 2: Add engine detection and profile application early in `Emulator::Run()`** right after `title_id` is extracted and before graphics/runtime linker initialization.
- [x] **Step 3: Log the detected engine name, reason, and applied optimization count** to console and log file.

---

### Task 5: Verification & Review

- [x] **Step 1: Verify all modified and new files with clang-format / style check**.
- [x] **Step 2: Review git status, diff, and ensure complete workspace hygiene**.
