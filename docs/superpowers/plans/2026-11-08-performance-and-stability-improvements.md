# Emulator Performance and Gameplay Stability Improvements

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate Vulkan queue-submission thrashing (~50 submits/frame down to 1–3/frame), reduce write-tracking SEH page fault churn (36.4k faults/s), relieve PS5 direct memory pressure (11.77 GB / 12.00 GB), and eliminate pipeline compilation hitches to achieve smooth, high-FPS gameplay in titles like *GYLT*.

**Architecture:**
1. **PM4 Submission Coalescing & Slicing:** Process up to 16 consecutive packets per queue slice before rotating queues, and remove indiscriminate queue-switch flushes so command buffers accumulate draws/dispatches across PM4 packets rather than submitting on every micro-switch.
2. **Buffer Write-Fault Invalidation & Clustering Optimization:** Optimize buffer cache dirty tracking and cluster unprotecting so that write faults don't trigger per-page SEH traps repeatedly within the same frame.
3. **Physical Direct Memory Allocation Audit & Header Sanitation:** Check direct memory management in `src/kernel/memory.cpp` to ensure released physical ranges and transient buffers are properly recycled to maintain direct memory headroom.
4. **Asynchronous Pipeline Pre-warming & Cache Persistence:** Prevent synchronous pipeline compile stalls during gameplay.

**Tech Stack:** C++20, Vulkan 1.3, Clang-CL, Windows x64 SEH (`VirtualProtect`).

---

## Global Constraints

- **STRICT:** **NO `git push`** at any time. Local commits only.
- All code must comply with C++20 and compile with Clang-CL in MSVC Developer environment.
- Preserve all existing documentation and docstrings.
- 100% CTest pass rate (79/79) must be preserved at every step.
- Windows Medium integrity level must be applied to newly built binaries via `icacls`.

---

## Review Focus

1. **Queue Slicing Starvation:** Slicing a queue for up to 16 packets must not starve other queues that are unblocked.
2. **Cross-Queue Synchronization:** When a queue issues a synchronization barrier (`RELEASE_MEM`, `WAIT_REG_MEM`, or `MEM_SEMAPHORE`), it must flush to Vulkan if another queue or CPU thread depends on that GPU signal.
3. **Slot Buffer Binding Stability:** In `BufferCache::ObtainBuffer`, keep `can_stream` guarded so sparse descriptor bindings in test #50 (`RenderExecutorStencilBindingDiscovery`) are not misidentified as streaming ring buffers.
4. **SEH Re-entrancy & Thread Safety:** Memory unprotecting must not race with GPU thread dirty tracking or cause double faults.

---

## Tasks

- [x] **Task 1: Implement Queue Slicing & Eliminate False Queue-Switch Flushes**
  - **Files:** `src/graphics/guest_gpu/graphicsRun.cpp`
  - **Details:**
    - Introduce `constexpr uint32_t MaxQueueSlice = 16u;` and a per-thread slice counter in `GuestGpu::ThreadRun`.
    - Do not advance `m_next_queue` immediately if the active queue has pending submissions, up to `MaxQueueSlice`.
    - Remove the indiscriminate cross-queue flush loop (`for (uint32_t offset = 0; offset < QueueCount; offset++) ... if (id != submission.queue_id) needs_flush = true;`).
    - Retain `needs_flush` for true synchronization points: `!complete`, `NeedsFlush()` (e.g. batch draw limits or interrupt release mem), and suspend points.
  - **Verification:**
    - Clean build `kyty_tests`.
    - Run CTest suite: all 79 tests must pass (specifically test #50, #51, #57, #58, #59, #60, #66).
    - Commit locally with `feat(gpu): coalesce queue submissions and eliminate cross-queue submit thrashing`.

- [x] **Task 2: Optimize Buffer Invalidation & Write-Fault Clustering**
  - **Files:** `src/graphics/host_gpu/renderer/cache/bufferCache.cpp`, `src/graphics/host_gpu/memoryTracker.h`
  - **Details:**
    - In `BufferCache::InvalidateMemory`, inspect clustering when `size == 1`.
    - If `has_owner` and the owner buffer is not GPU-modified, unprotect the owner's full range up to `MaxCluster` (512 KB), avoiding repetitive single 4 KB page faults when the CPU sequentially writes to uniform or vertex structures.
    - Verify that `can_stream` in `ObtainBuffer` retains `size <= CACHING_PAGESIZE` to preserve compatibility with descriptor discovery.
  - **Verification:**
    - Run CTest test #75 (`buffer_cache_dirty_gc`), #79 (`buffer_cache_ranges`), and #41 (`shader_recompiler_compute`).
    - Commit locally with `perf(cache): cluster buffer write-tracking invalidations to reduce SEH page fault traps`.

- [x] **Task 3: Direct Memory Allocation Audit & Reclamation Safeguards**
  - **Files:** `src/kernel/memory.cpp`
  - **Details:**
    - Audit `KernelReleaseDirectMemory` and physical memory page tracking to ensure freed physical chunks are actively coalesced.
    - Check flexible vs direct memory accounting to prevent false out-of-memory errors when direct memory approaches 12 GB.
  - **Verification:**
    - Run CTest test #52 (`virtual_memory_allocation`).
    - Commit locally with `fix(kernel): ensure released direct memory chunks are coalesced and reusable`.

- [x] **Task 4: Run Clean Verification and Validate Performance Impact**
  - **Details:**
    - Re-run all 79 CTest tests and launcher tests.
    - Verify 100% test pass rate across the board.
