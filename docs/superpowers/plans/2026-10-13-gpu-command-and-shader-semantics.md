# GPU Command, Shader, and Memory Semantics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build offline GPU command replay and shader test-reduction tooling, diagnose and resolve shader recompiler expansion and presentation stall bottlenecks (targeting ASTRO BOT 2 FPS and Astro's Playroom 0 FPS stall), and establish deterministic regression tests in CTest.

**Architecture:** Add dedicated CLI verbs to `kyty_emulator` for headless shader recompilation (`--recompile-shader`) and PM4 stream replay (`--replay-pm4`), backed by Python analysis and reduction tooling in `tools/shader/` and `tools/gpu/`. Use these tools to profile and shrink ASTRO BOT's bloated compute shaders (1.1 MB SPIR-V) in `ShaderRecompiler`, and resolve presentation swapchain/semaphore stalls in Astro's Playroom.

**Tech Stack:** C++20, Vulkan 1.3, SPIR-V Tools (`spirv-tools-opt`, `spirv-val`), Python 3 standard library, CTest.

**Spec:** `docs/superpowers/specs/2026-10-12-compatibility-program.md` (Workstream 2) and `docs/compatibility/ps5-comparison-protocol.md`.

## Global Constraints

- Primary operating systems are Windows and Linux.
- Reference GPU is NVIDIA GeForce RTX 4060 Ti with Vulkan 1.3.
- Commercial game frames, executables, shaders, dumps, and proprietary payloads remain strictly outside Git.
- Generated offline regression test cases must be sanitized, synthetic, and self-contained in `tests/shaderCfgTests.cpp` or `tests/ShaderRecompilerComputeTests.cpp`.
- Every functional change must follow a failing-test, minimal-implementation, passing-test TDD cycle.
- Maintain existing 58 CTest tests (40 core, 18 vulkan) at a 100% pass rate.

## Review Focus

- Invalid or truncated shader bytecode: standalone recompiler must return a descriptive non-zero exit code and never segfault or leak host memory; covered by Task 1.
- Sanitization: generated C++ test cases must contain zero guest memory paths, raw host pointers, or proprietary metadata; covered by Task 3.
- SPIR-V validity: all recompiler passes and optimizations must produce valid SPIR-V verified by `spirv-val`; covered by Task 1 and Task 4.
- Determinism: PM4 stream replay must produce identical register and dispatch state regardless of host scheduling; covered by Task 2.
- Presentation stall recovery: resolving the Astro's Playroom frame drop must not introduce frame tearing or race conditions in swapchain acquisition; covered by Task 5.

---

### Task 1: Add standalone shader recompiler CLI and Python driver

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/common/emulatorConfig.h`
- Create: `tools/shader/recompile_shader.py`
- Create: `tools/shader/tests/test_recompile_shader.py`
- Test: `tests/ShaderVertexMetadataTests.cpp` or CTest CLI test target

**Interfaces:**
- Consumes: `ShaderRecompiler::TranslateProgram`, `ShaderRecompiler::CompileProgram`, `ShaderCompiler::Disassemble`.
- Produces: CLI option `kyty_emulator --recompile-shader <stage> <input.bin> [--out <output.spv>] [--disasm] [--validate]`.
- Output: writes SPIR-V binary and prints JSON summary (instruction count, SPIR-V size in dwords, compilation status).
- Python: `recompile_shader.py --emulator <path> --stage <cs|vs|ps> --input <file.bin> --output <file.spv>`.

- [x] **Step 1: Write failing Python tests for shader recompiler CLI runner**

Add tests covering valid compute shader compilation from synthetic binary bytes, invalid stage name rejection, nonexistent input file rejection, and output validation verification.

- [x] **Step 2: Run the Python test and verify RED**

Run: `python -m unittest tools/shader/tests/test_recompile_shader.py -v`
Expected: FAIL because `tools/shader/recompile_shader.py` does not exist.

- [x] **Step 3: Implement `--recompile-shader` CLI handler in `src/main.cpp` and `tools/shader/recompile_shader.py`**

In `src/main.cpp`:
- Parse `--recompile-shader <stage> <input.bin>`.
- Load binary bytecode using `Common::File`.
- Invoke `ShaderRecompiler::TranslateProgram` and `CompileProgram` with basic compile options.
- If `--validate` is passed, run `spvValidate` from `spirv-tools`.
- If `--out` is specified, write the compiled SPIR-V bytes.
- Print structured JSON stats to stdout (`{"status": "ok", "size_bytes": ..., "stage": ...}`).
- Exit with 0 on success, 1 on failure.

Implement `tools/shader/recompile_shader.py` with standard argument parsing wrapping the CLI.

- [x] **Step 4: Build and run tests to verify GREEN**

Run: `cmake --build _Build/compat-foundation --target kyty_emulator --parallel`
Run: `python -m unittest tools/shader/tests/test_recompile_shader.py -v`
Expected: PASS.

- [x] **Step 5: Commit**

Commit: `feat(shader): add standalone shader recompilation CLI and driver`

---

### Task 2: Implement offline PM4 command buffer replay harness

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/graphics/guest_gpu/pm4.h`
- Modify: `src/graphics/guest_gpu/pm4.cpp`
- Create: `tools/gpu/replay_pm4.py`
- Create: `tools/gpu/tests/test_replay_pm4.py`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Pm4::DumpPm4PacketStream`, `Pm4::ExecutePm4PacketStream`, `HardwareContext`.
- Produces: CLI option `kyty_emulator --replay-pm4 <stream.bin|stream.log> [--headless]`.
- Binary stream format: schema version header, packet array, register snapshot.
- Driver: `replay_pm4.py --emulator <path> --input <stream>` validating execution without host crashes.

- [x] **Step 1: Write failing replay test cases**

Create Python unit tests in `tools/gpu/tests/test_replay_pm4.py` testing packet stream parsing, execution of synthetic type 3 packets (SET_SH_REG, DRAW_INDEX_AUTO), rejection of corrupted packets, and exit status reporting.

- [x] **Step 2: Run the replay test and verify RED**

Run: `python -m unittest tools/gpu/tests/test_replay_pm4.py -v`
Expected: FAIL because `tools/gpu/replay_pm4.py` does not exist.

- [x] **Step 3: Implement PM4 replay mechanism in emulator and python runner**

In `src/graphics/guest_gpu/pm4.cpp`:
- Implement `bool ReplayPm4PacketStream(const uint32_t* cmd_buffer, uint32_t num_dw, HardwareContext* ctx)`.
- Support reading raw dword packets from a binary file dump.

In `src/main.cpp`:
- Add `--replay-pm4 <file>` option to dispatch PM4 packets directly through the GPU context in headless or windowed mode.

In `tools/gpu/replay_pm4.py`:
- Parse and invoke replay, sanitize stdout/stderr.

- [x] **Step 4: Build and verify GREEN**

Run: `cmake --build _Build/compat-foundation --target kyty_emulator --parallel`
Run: `python -m unittest tools/gpu/tests/test_replay_pm4.py -v`
Expected: PASS.

- [x] **Step 5: Commit**

Commit: `feat(gpu): add offline pm4 stream replay harness`

---

### Task 3: Build automated shader test-reduction and test synthesis tool

**Files:**
- Create: `tools/shader/reduce_shader.py`
- Create: `tools/shader/tests/test_reduce_shader.py`
- Modify: `docs/superpowers/plans/2026-10-13-gpu-command-and-shader-semantics.md`

**Interfaces:**
- Consumes: Raw guest shader bytecode (`.bin`), SPIR-V dumps (`.spv`), compilation error or validation diagnostics.
- Produces: Sanitized, standalone C++ test case snippet formatted for direct inclusion into `tests/shaderCfgTests.cpp`.
- Strips: file paths, guest user data values, hashes, identifying game strings.
- Emits: `const uint32_t code[] = { ... };` with `RecompileForTest(...)` assertion checking SPIR-V validity and expected opcode behavior.

- [x] **Step 1: Write failing tests for test-reduction synthesis**

Add Python unit tests in `tools/shader/tests/test_reduce_shader.py`:
- Test that input binary bytecode is converted into a formatted C++ array literal.
- Test that proprietary paths and game names are completely excluded from output.
- Test delta-debugging / line minimization logic that strips trailing NOPs and unused instruction blocks while preserving compile errors.

- [x] **Step 2: Run test and verify RED**

Run: `python -m unittest tools/shader/tests/test_reduce_shader.py -v`
Expected: FAIL because `tools/shader/reduce_shader.py` does not exist.

- [x] **Step 3: Implement `tools/shader/reduce_shader.py`**

Implement bytecode minimization:
- Chunk binary into 32-bit dwords / GCN instruction boundaries.
- Generate valid C++ snippet using `RecompileForTest` template from `tests/shaderCfgTests.cpp`.
- Add CLI options: `--input <file.bin> --stage <cs|vs|ps> --output <test.cpp> --minimize`.

- [x] **Step 4: Run test and verify GREEN**

Run: `python -m unittest tools/shader/tests/test_reduce_shader.py -v`
Expected: PASS.

- [x] **Step 5: Commit**

Commit: `feat(shader): add shader reduction and test case synthesis tool`

---

### Task 4: Diagnose and optimize ASTRO BOT compute shader bloat (2 FPS bottleneck)

**Files:**
- Modify: `src/graphics/shader/recompiler/ir/passes/DeadCodeElimination.cpp`
- Modify: `src/graphics/shader/recompiler/backend/spirv/SpirvEmitter.cpp`
- Modify: `src/graphics/shader/recompiler/frontend/translate/Translate.cpp`
- Test: `tests/shaderCfgTests.cpp`

**Interfaces:**
- Consumes: ASTRO BOT compute shader bytecode `0008_new_shader_cs_01d6f21611218e74.bin` (25 KB -> 1.1 MB SPIR-V).
- Produces: Optimized IR and SPIR-V code generation with reduced instruction count, eliminating redundant phi nodes, unrolled loop bloat, and duplicated SRT scalar loads.

- [x] **Step 1: Create a minimized reproduction test in `tests/shaderCfgTests.cpp`**

Using Task 3 synthesis, extract the recursive/duplicated instruction block from `0008_new_shader_cs_*.bin` into a new test `check_massive_compute_shader_compilation_size()` asserting that generated SPIR-V size is below a strict threshold (e.g. < 400 KB instead of 1.1 MB).

- [x] **Step 2: Run the test and verify RED**

Run: `cmake --build _Build/compat-foundation --target shader_cfg_tests --parallel && _Build/compat-foundation/shader_cfg_tests.exe`
Expected: FAIL on the size / instruction count assertion.

- [x] **Step 3: Implement configurable SPIR-V optimization in `SpirvEmitter.cpp`**

- Run the SPIRV-Tools size or performance pass pipeline after typed SPIR-V emission while preserving the shader interface.
- Thread the existing `None`, `Size`, and `Performance` configuration through `CompileOptions` and the pipeline cache.
- Use performance optimization for standalone shader analysis so pathological captures can be measured without changing local launcher settings.

- [x] **Step 4: Run test and verify GREEN**

Run: `cmake --build _Build/compat-foundation --target shader_cfg_tests --parallel && _Build/compat-foundation/shader_cfg_tests.exe`
Expected: PASS with significantly reduced SPIR-V size and a smaller driver compilation input.

Measured: the 25,072-byte local compute capture now produces 277,976 bytes of valid SPIR-V, down from 507,120 bytes and below the 400 KB regression ceiling. Optimization increases this capture's one-time standalone compilation from about 369 ms to about 1,000 ms; normal emulation pays that cost only when `Size` or `Performance` is configured.

- [x] **Step 5: Commit**

Commit: `perf(shader): eliminate redundant instructions and shrink compute spirv emission`

---

### Task 5: Diagnose and resolve Astro's Playroom presentation stall (0 FPS issue)

**Files:**
- Modify: `src/graphics/presentation/window/vulkanWindow.cpp`
- Modify: `src/graphics/presentation/window/window.cpp`
- Test: `tests/RunDiagnosticsTests.cpp` and `tests/ShaderRecompilerComputeTests.cpp`

**Interfaces:**
- Consumes: `VulkanWindow::Present`, `AcquireNextImageKHR`, timeline semaphore fences.
- Produces: non-blocking or recovered swapchain presentation when GPU pipeline waits occur near timeout boundaries.

- [ ] **Step 1: Write a synchronization test asserting timely swapchain acquisition**

In `tests/ShaderRecompilerComputeTests.cpp`, add a test exercising swapchain image acquisition when presentation queues have pending command buffers and timeout reaches near boundary, ensuring no deadlocks or permanent 0 FPS stalls occur.

- [ ] **Step 2: Run test and verify failure or diagnose stall**

Run: `cmake --build _Build/compat-foundation --target shader_recompiler_compute_tests --parallel && _Build/compat-foundation/shader_recompiler_compute_tests.exe --scheduler-only`

- [ ] **Step 3: Implement presentation timeout handling and synchronization recovery**

In `src/graphics/presentation/window/vulkanWindow.cpp`:
- Handle `VK_TIMEOUT` and `VK_NOT_READY` from `vkAcquireNextImageKHR` gracefully with retry budgets instead of indefinite waits.
- Ensure frame timeline ticks and semaphore signaled states are properly advanced even during transient queue starvation.

- [ ] **Step 4: Build and verify GREEN**

Run: `ctest --test-dir _Build/compat-foundation -L vulkan --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

Commit: `fix(presentation): prevent swapchain acquisition deadlocks and presentation stalls`

---

### Task 6: Full regression testing and corpus performance validation

**Files:**
- Modify: `docs/compatibility/baseline.md`
- Modify: `docs/compatibility/baseline.json`

**Interfaces:**
- Consumes: local 8-title corpus and all CTest targets.
- Produces: updated baseline reports showing improved FPS for `PPSA21567` and uninterrupted presentation for `PPSA01325`.

- [ ] **Step 1: Run complete core test suite**

Run: `ctest --test-dir _Build/compat-foundation -L core --output-on-failure`
Expected: 100% tests passed (all 40+ tests).

- [ ] **Step 2: Run complete Vulkan test suite**

Run: `ctest --test-dir _Build/compat-foundation -L vulkan --output-on-failure`
Expected: 100% tests passed (all 18+ tests).

- [ ] **Step 3: Run Python tool test suites**

Run: `python -m unittest discover -s tools/compat/tests -v`
Run: `python -m unittest discover -s tools/shader/tests -v`
Run: `python -m unittest discover -s tools/gpu/tests -v`
Expected: 100% tests passed.

- [ ] **Step 4: Run local corpus benchmark on PPSA21567 and PPSA01325**

Run: `python tools/compat/run_corpus.py --manifest tools/compat/corpus.local.json --emulator _Build/compat-foundation/kyty_emulator.exe --output _CompatibilityRuns --title-id PPSA21567`
Run: `python tools/compat/run_corpus.py --manifest tools/compat/corpus.local.json --emulator _Build/compat-foundation/kyty_emulator.exe --output _CompatibilityRuns --title-id PPSA01325`
Verify that `PPSA21567` FPS improves from 2.0 FPS and `PPSA01325` sustains non-zero FPS through the timeout boundary.

- [ ] **Step 5: Commit documentation and baseline updates**

Commit: `docs(compat): update baseline with optimized shader performance`

---

## Self-Review Result

1. **Spec Coverage:** The plan directly addresses Workstream 2 of `docs/superpowers/specs/2026-10-12-compatibility-program.md`: offline GPU command replay, shader test reduction tooling, compute shader bloat optimization, presentation stall fixing, and CTest regression coverage.
2. **Step Scan:** Every step specifies an unambiguous command, file path, interface, and expected output.
3. **Type Consistency:** Method signatures and CLI arguments (`--recompile-shader`, `--replay-pm4`, `--reduce`) are uniformly named across tools and emulator components.
4. **Review Focus:** Handles truncated/invalid shader bytecode, sanitized test creation, SPIR-V validation via `spirv-val`, replay determinism, and swapchain synchronization.
5. **Proportion:** Clear, actionable, and structured into 6 focused tasks.
