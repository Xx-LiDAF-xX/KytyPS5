# Host performance and memory budgeting

KytyPS5 should use the host's available resources without changing the guest game's
rendering or simulation semantics. Hardware detection alone cannot guarantee native
PC performance, remove every shader compilation stall, or repair unsupported guest
behavior. Frame-cap patches need validation for each game.

## Changes

- Initialize registered live switches again after startup presets and engine profiles
  populate the environment, before renderer initialization. Previously, static
  switches read the environment before `main`; standalone launches silently ignored
  settings such as `KYTY_VRAM_GC_BUDGET=1` in the bundled preset. Startup initialization
  does not call runtime change hooks, and already-applied live overrides retain priority.
- Compute a total device-local memory budget on both discrete and integrated GPUs.
  Non-local host heaps are not counted as additional device-local capacity. In
  particular, an integrated GPU's physical heap size no longer overrides a smaller
  driver budget or `KYTY_VRAM_BUDGET_MB` cap.
- Treat the driver's budget as total process capacity, including existing allocations.
  Subtracting current process usage again would make the collector's target shrink
  as the cache fills and encourage unnecessary eviction/recreation.
- Leave one eighth of the reported budget as headroom, up to 1 GiB, preserving the
  existing discrete-GPU policy. Without driver budget reporting, shared-memory GPUs
  reserve one quarter of the device-local heap, up to 8 GiB, so smaller PCs retain
  usable capacity. This fallback is a heuristic, not a measurement of free system RAM.
- Recompute the reusable image pool's limit from the current budget: at most one
  eighth of that budget and never above `KYTY_NATIVE_IMAGE_POOL_MB`. Zero budget keeps
  no pooled images. Capacity checks handle a budget shrinking below retained storage
  without unsigned underflow.
- Trim completed, retired native images against budget/count limits at most every
  100 ms, even when idle expiry is disabled and no new image enters the pool. Pressure
  releases pooled storage; ordinary reuse still avoids native allocation churn.

The meaning of `heapBudget` and `heapUsage` follows
[VK_EXT_memory_budget](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/appendices/VK_EXT_memory_budget.adoc).
Budget and usage are estimates and can change as other applications use the GPU.

## Existing hardware behavior

Automatic GPU selection considers compatibility, native drivers, device type, and
device-local capacity. CPU placement already recognizes the available CPU topology,
keeps reservations within allowed processors, and handles supported dual-CCD X3D
layouts. Shader and pipeline caches avoid rebuilding previously encountered work.

`--present-mode Immediate` requests presentation without waiting for display vblank
when the driver supports it; it can tear. It does not unlock a guest game's own
30/60 FPS logic. Changing `--vblank-frequency` is not a universal safe FPS unlock.
These settings are unchanged by the memory fixes.

## Validation scope

Regression tests cover startup settings loaded after static registration, duplicate
switch registrations, explicit disables, defaults, live-override precedence, and
runtime hooks. Memory tests cover small and large budgets, integrated/discrete
policies, caps, missing driver budget reporting, zero capacity, and pool capacity
shrinking below retained allocations.

Game measurements and their limitations are recorded separately in
`_Build/diagnostics/host-memory-budget/results.md`. Launch traces measure completed
guest-flip intervals, not GPU execution time or a controlled gameplay route. They
cannot establish that every scene runs smoothly or that a title is crash-free.
