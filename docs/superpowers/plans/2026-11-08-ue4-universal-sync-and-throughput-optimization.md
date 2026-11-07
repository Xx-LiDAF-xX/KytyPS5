# UE4 Games Universal Sync & Throughput Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Overhaul universal GPU-CPU synchronization stalls, background pipeline compilation scaling, compiler optimization flags, and per-draw descriptor hashing to elevate draw throughput and frame stability across Unreal Engine 4 games (*GYLT*, *Sackboy: A Big Adventure*, *Pumpkin Jack*) and general titles without graphical regressions.

**Architecture:**
1. Enable Frame Pointer Omission (`/Oy`) in Release/RelWithDebInfo builds and silence deprecated C++17 conversion warnings in `src/utils.cmake`.
2. Convert blocking occlusion query harvest in `CommandProcessor::ReleaseMem` to non-blocking (`Publish(false)`), reserving blocking waits (`Publish(true)`) strictly for conditional predication, explicit buffer waits, and frame boundaries.
3. Scale the background pipeline linking worker pool in `PipelineCache` from a single thread to a hardware-concurrency scaled worker pool (2–4 threads, `MaxOutstandingLinks = 16`) to eliminate compile hitches without thread contention.
4. Eliminate redundant descriptor set hash computations in `RenderExecutor::Draw` / `CommitBindings` by forwarding the already computed MDI bindings hash.

**Tech Stack:** C++20, Vulkan 1.3, CMake, Ninja, Clang-CL, LLVM, Windows x64.

---

## Global Constraints
- Strictly local Git commits on `main`; **NEVER execute `git push`**.
- Must be 100% universal: NO title-ID checks, no game-specific hacks.
- Zero tolerance for visual regressions, artifacts, flickering, or checkerboard corruption.
- Parallel Ninja compilation must stay at `-j 4` to prevent Clang-CL memory exhaustion.
- Preserve all unit tests (`MdiBatcherTests`, `OcclusionQueryTests`, `ResourceTrackingTests`, `MemoryTrackerTests`).

---

## Review Focus
1. **Occlusion query synchronization**: Non-blocking `Publish(false)` in `ReleaseMem` must never leave predication buffers un-updated when `SetPredication` executes.
2. **Checkerboard reconstruction**: Queue switching and interrupt signaling order must remain intact on every slice.
3. **Multi-threaded pipeline linking**: Multiple worker threads must safely access `m_link_jobs`, `m_link_results`, and `vkCreateGraphicsPipelines` without data races or driver cache corruption.
4. **Frame pointer omission**: CodeView / PDB symbol generation and exception unwinding must remain intact on Windows x64.
5. **Memory tracker integrity**: Page fault handling and GPU write tracking must not regress.

---

## Tasks

- [ ] **Task 1: Compiler Optimization Flags & Warning Suppression**
  - **Files:** `src/utils.cmake`
  - **Details:**
    - In MSVC / Clang-CL configuration, replace `/Oy-` with `/Oy` for Release and RelWithDebInfo builds to enable Frame Pointer Omission on x64, freeing `rbp` as a 16th general-purpose register.
    - Add compile definitions `_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING` and `_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS` to eliminate spam during compilation.
  - **Verification:** Run `ninja -C _Build/windows -j 4` and verify clean compilation without codecvt warnings.

- [ ] **Task 2: Eliminate Synchronous CPU-GPU Wait Stalls in ReleaseMem**
  - **Files:** `src/graphics/guest_gpu/graphicsRun.cpp`
  - **Details:**
    - In `CommandProcessor::ReleaseMem` (around line 1065), replace `m_renderer.GetOcclusionQueries().Publish(true)` with `m_renderer.GetOcclusionQueries().Publish(false)`.
    - This allows completed queries to be harvested without stalling the command processor thread on the timeline semaphore (`MasterSemaphore::Wait`) thousands of times per second.
    - Confirm that blocking waits (`Publish(true)`) remain active where required: in `CommandProcessor::SetPredication` (where the GPU CPU actually tests predication data), `CommandProcessor::BufferWait()`, and `SubmissionType::SuspendPoint`.
  - **Verification:** Run `.\_Build\windows\occlusion_queries_tests.exe` and `.\_Build\windows\mdi_batcher_tests.exe`.

- [ ] **Task 3: Scale Background Pipeline Linking Worker Pool**
  - **Files:** `src/graphics/host_gpu/renderer/pipeline/pipelineCache.h`, `src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp`
  - **Details:**
    - Increase `MaxOutstandingLinks` from 2 to 16.
    - Replace the single `std::jthread m_link_thread` with a vector of worker threads: `std::vector<std::jthread> m_link_threads`.
    - Initialize workers based on `std::clamp(std::thread::hardware_concurrency() / 4, 2u, 4u)`.
    - In destructor, notify all workers (`m_link_ready.notify_all()`) and join all workers before saving cache.
  - **Verification:** Build and run all test binaries.

- [ ] **Task 4: Eliminate Duplicate MDI Bindings Hash Calculation**
  - **Files:** `src/graphics/host_gpu/renderer/renderDraw.cpp`, `src/graphics/host_gpu/renderer/pipeline/descriptors.cpp`, `src/graphics/host_gpu/renderer/render.h`
  - **Details:**
    - In `RenderExecutor::Draw`, when `CanFastAppendMdiBindings` evaluates the 128-bit hash, pass an optional output pointer `MdiHash128* out_hash`.
    - If the batch cannot be appended and a new batch begins, forward `computed_hash` directly into `gfx_extra.out_mdi_bindings_hash` instead of re-running the full `ComputeMdiBindingsHash` over all descriptor tables a second time.
  - **Verification:** Run `.\_Build\windows\mdi_batcher_tests.exe` and `.\_Build\windows\resource_tracking_tests.exe`.

- [ ] **Task 5: Full Regression Testing & Binary Linking**
  - **Files:** All modified files
  - **Details:**
    - Build full project with `ninja -C _Build/windows -j 4`.
    - Execute all test suites: `mdi_batcher_tests.exe`, `occlusion_queries_tests.exe`, `resource_tracking_tests.exe`, `memory_tracker_tests.exe`.
    - Verify clean Git status and record commit details.
