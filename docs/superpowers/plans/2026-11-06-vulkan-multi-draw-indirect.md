# Vulkan Multi-Draw Indirect (MDI) Batching Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a Vulkan 1.3/1.4 hardware-accelerated Multi-Draw Indirect (MDI) batching engine in KytyPS5 to coalesce compatible draw calls into contiguous indirect draw buffers, collapsing thousands of host API calls into single GPU hardware dispatches and eliminating CPU-waiting-on-GPU bottlenecks.

**Architecture:** Introduce `MdiBatcher` inside `RenderExecutor` (`src/graphics/host_gpu/renderer/mdiBatcher.h`). When consecutive PM4 draw calls share compatible graphics pipeline layouts, render targets, dynamic state, and descriptor sets, `MdiBatcher` accumulates their draw parameters into a contiguous staging array. When a batch limit is hit or a non-coalescible pipeline barrier/state change occurs, the accumulated commands are copied to the stream ring buffer and executed via `vkCmdDrawIndexedIndirect` (or `vkCmdDrawIndexedIndirectCount`).

**Tech Stack:** C++20, Vulkan 1.3/1.4 (`vk::CommandBuffer::drawIndexedIndirect`, `drawIndexedIndirectCount`), CMake, Ninja.

**Spec:** Pure Vulkan 1.3/1.4 Multi-Draw Indirect acceleration for KytyPS5 draw submission pipeline.

## Global Constraints

- Must maintain 100% visual correctness: occlusion culling, depth/stencil tests, dynamic lighting, and tessellation must produce identical output.
- Dynamic hardware scalability: batch limits and stream allocations must scale dynamically with host CPU core count and GPU VRAM.
- Codebase synchronization: keep branch clean and synced with `origin/main` without breaking existing unit tests (`memory_tracker_tests`, `page_manager_tests`, `occlusion_queries_tests`).
- Standard Vulkan 1.3 baseline: do not require external CUDA or ROCm SDK installations.

## Review Focus

- Primitive restart and index size compatibility between batched draws.
- Render pass boundaries: dynamic rendering must remain valid across MDI execution without premature `EndRendering`.
- Flush triggers: push constants, descriptor re-bindings, and shader buffer write barriers must cleanly flush any pending MDI batch before state changes take effect.
- Zero-draw and single-draw handling: a batch of 1 draw must route cleanly without unnecessary indirect dispatch overhead.

---

### Task 1: MdiBatcher Core Data Structures & Unit Test

**Files:**
- Create: `src/graphics/host_gpu/renderer/mdiBatcher.h`
- Create: `src/graphics/host_gpu/renderer/mdiBatcher.cpp`
- Create: `tests/MdiBatcherTests.cpp`
- Modify: `CMakeLists.txt:563`

- [x] **Step 1: Write the failing unit test for MdiBatcher**
  Implement test cases in `tests/MdiBatcherTests.cpp` verifying draw aggregation, state compatibility checking (matching pipeline, index buffer address, render target key), batch capacity limits, and emission format.
- [x] **Step 2: Add test executable to CMakeLists.txt**
  Register `mdi_batcher_tests` in `CMakeLists.txt` and link with `common`, `fmt::fmt`, and `Vulkan::Headers`.
- [x] **Step 3: Run test build to confirm failure**
  Run `ninja -C _Build/windows mdi_batcher_tests` to confirm failure prior to implementation.
- [x] **Step 4: Implement MdiBatcher class**
  Implement `MdiBatcher` in `mdiBatcher.h` and `mdiBatcher.cpp` managing the accumulation of `vk::DrawIndexedIndirectCommand` and `vk::DrawIndirectCommand`, compatibility state keys, and buffer emission.
- [x] **Step 5: Verify tests pass**
  Run `.\_Build\windows\mdi_batcher_tests.exe` and confirm all tests pass.
- [x] **Step 6: Commit**
  `git commit -m "feat(graphics): implement core MdiBatcher and unit test suite"`

---

### Task 2: Integrate MdiBatcher into RenderExecutor (`renderDraw.cpp`)

**Files:**
- Modify: `src/graphics/host_gpu/renderer/render.h`
- Modify: `src/graphics/host_gpu/renderer/renderDraw.cpp`

- [x] **Step 1: Add MdiBatcher member to RenderExecutor**
  Instantiate `MdiBatcher m_mdi_batcher` in `RenderExecutor` and expose `FlushMdi(CommandBuffer& buffer)`.
- [x] **Step 2: Intercept consecutive indexed draws in ExecutePreparedDraw**
  When `m_mdi_batcher.CanBatch(...)` is true, append draw parameters (`index_count`, `instance_count`, `first_index`, `vertex_offset`, `first_instance`) into `m_mdi_batcher` instead of immediately issuing `vk_buffer.drawIndexed`.
- [x] **Step 3: Flush MdiBatcher on incompatible state boundaries**
  Trigger `FlushMdi()` before pipeline changes, render target changes, push constant modifications, and `ShaderWriteHazardBarrier`.
- [x] **Step 4: Emit Multi-Draw Indirect command on flush**
  When flushing, if batch count > 1, copy commands to `StreamBuffer` and issue `vk_buffer.drawIndexedIndirect(...)`. If batch count == 1, fall back to direct `vk_buffer.drawIndexed(...)`.
- [x] **Step 5: Build and verify regression suite**
  Run `ninja -C _Build/windows kyty_emulator memory_tracker_tests page_manager_tests occlusion_queries_tests mdi_batcher_tests`.
- [x] **Step 6: Commit**
  `git commit -m "feat(graphics): integrate MdiBatcher into RenderExecutor draw pipeline"`

---

### Task 3: CommandProcessor BufferFlush & End-of-Batch Integration

**Files:**
- Modify: `src/graphics/guest_gpu/graphicsRun.cpp`
- Modify: `src/graphics/guest_gpu/command_processor/commandProcessor.h`

- [x] **Step 1: Flush pending MDI commands on BufferFlush**
  Ensure `CommandProcessor::BufferFlush()` calls `m_renderer.GetRenderExecutor().FlushMdi(CurrentBuffer())` before submitting to Vulkan.
- [x] **Step 2: Flush pending MDI commands before Compute dispatches and Flip**
  Ensure compute dispatches and flip presentations flush any open MDI graphics batch.
- [x] **Step 3: Build kyty_emulator and test on target game PPSA13526**
  Run full build and verify emulator launch and rendering.
- [x] **Step 4: Commit and push**
  `git commit -m "feat(graphics): complete Vulkan MDI batching integration in CommandProcessor"`
