# KYTYPS5 PERFORMANCE ANALYSIS

Analysis date: October 7, 2026 (Asia/Colombo). This is a source and existing-log analysis;
no game, benchmark, build, or test was executed for this report. No optimization is validated.

GAME: Astro Bot; separate supporting workload: Astro's Playroom.

GAME ID: PPSA21567; supporting workload PPSA01325.

EMULATOR COMMIT: current HEAD `1d3e48cab6277ea399c0efd75f7eb2ca9e0e8835`.
Astro Bot's error identifies `1d3e48c-dirty`. The workspace has extensive modifications.
Neither that label nor HEAD uniquely identifies the measured executable's source or build flags.

OS: current host reports Windows 11 Pro, version 10.0.26200.

CPU: current host reports AMD Ryzen 7 5800X, 8 cores / 16 logical processors.

GPU: current host reports NVIDIA GeForce RTX 4060 Ti, PCI vendor 10DE / device 2805.
An existing GPU test log also identifies this GPU. The game probes do not independently establish
every aspect of their hardware environment.

DRIVER: current Windows driver reports 32.0.16.1714. An existing GPU test reports raw Vulkan
driverVersion 2588114944; preserve this as a raw API value rather than treating it as a decimal
driver release number.

RAM: current OS reports 33,460,184 KiB visible memory, about 31.91 GiB. Process usage unknown.

VRAM: unknown; collect Vulkan memory heaps and budgets. Do not infer capacity from GPU name.

RESOLUTION: Quality/native preset in probe arguments; actual internal and presentation extents unknown.

CACHE STATE: both latest probes log an empty Vulkan pipeline cache at initialization.
Shader journal state and driver-managed disk cache state are unknown; this is not proof of a fully
cold run.

TARGET FPS: 60 for this initial assessment (16.67 ms/frame); also report misses against 30 FPS
(33.33 ms) and 120 FPS (8.33 ms) where relevant.

## 1. CURRENT PERFORMANCE

The primary evidence is under `_Build/rt/probes/20261007-003209/`.

Astro Bot exited with code 321 after 17.57 seconds. Its last complete five-second reporter window
contains 184 fresh frames (36.8/s), p50 4 ms, p95/p99 8 ms, and a reported max of 100 ms.
The scene is unidentified and includes startup; these values are not a gameplay benchmark.
A separate hitch line records 13,147 ms, demonstrating that the histogram max is capped.

Astro's Playroom ran for 61.62 seconds and was externally bounded/stopped, not proven to have
exited normally. Five-second windows range from zero frames through 60.2 fresh frames/s.
One steady window reports 60.0/s, p50 16 ms, p95/p99 17 ms. Later windows show p99 33-40 ms.
Hitch records include 4,791 ms, 2,740 ms, 783 ms and 167 ms. No scene labels or visual oracle
establish gameplay correctness, and zero-frame windows at the end require investigation.

For both workloads, whole-run average FPS, exact median, 1% low, 0.1% low, exact P95/P99,
exact maximum, standard deviation, CPU frame time, total GPU frame time, CPU/per-thread/GPU
utilization, RAM usage and VRAM usage are unavailable. Interval histograms do not recover them.
Do not turn P99 into a claimed 1% low: the chosen low-FPS statistic must be defined independently.

## 2. BOTTLENECK CLASSIFICATION

- **CONFIRMED / high confidence:** Astro Bot has a P0 invalid-range termination. stderr reports
  an eight-byte access at 0x30 in `memory.cpp:984`. This is the checked failure site, not the
  established originating bug. No sustained gameplay bottleneck is established for this run.
- **CONFIRMED costs / medium causal confidence:** Playroom has severe pipeline/lookahead stalls.
  The 4,791 ms hitch reports pipeline-create 1,618.9 ms and lookahead 2,540.1 ms; the 2,740 ms
  hitch reports pipeline-create 1,914.1 ms and lookahead 896.6 ms. Timers can overlap and cannot
  be summed as exclusive frame slices. Validation was enabled; production cost is unmeasured.
- **CONFIRMED costs / medium critical-path confidence:** Playroom repeatedly synchronizes for
  DCC metadata. In the final active interval, 159 DISPATCH_INDIRECT DCC full drains total
  563.54 ms (about 3.52 ms per 160 fresh frames). DrawThread guest-read-fault tick waits total
  565.25 ms (about 3.53 ms/frame). Their dependencies and overlap need a timeline capture.
- **UNKNOWN:** dominant steady-state guest execution, resource refresh, streaming, driver and
  host GPU limits. Sparse timestamps do not measure every use of the host GPU.

The September AMD roadmap concerns a Ryzen 7800X3D / RX 9070 XT and older source states.
Its 38% drain and 12% materialization findings are historical leads, not measurements of this host.

## 3. FRAME-TIME BREAKDOWN

For Playroom's 300-frame / five-second window at stdout line 173:

| Measurement | Reported time per fresh frame | Interpretation |
| --- | ---: | --- |
| Full drains | 3.71 ms | Aggregate CPU blocking; includes DCC work |
| Tick waits | 2.93 ms | Multiple threads/reasons, not exclusive |
| Priority publication waits | 0.06 ms | Aggregate callback completion wait |
| Queue submission | 0.75 ms | Includes lock wait by definition |
| Instrumented GPU busy | 5.20 ms | Command-buffer interval union, not a complete GPU profiler |
| Instrumented GPU gap | 11.46 ms | Gap between measured command-buffer intervals |

Guest CPU, kernel/HLE, render submission excluding nested calls, shader decode/translation/emission,
actual driver compilation, streaming and other exclusive costs are unknown. The table must not
be summed: host waits overlap device execution and different threads.

## 4. ROOT CAUSE

Astro Bot: `LibKernel::Memory::ClampRangeSize` terminates when the virtual-range table rejects
a requested range. A null-based address is a hypothesis; the source of 0x30 is unknown until
the stack is symbolized against the matching executable/PDB and resource provenance is captured.
Do not bypass the range check or synthesize zero data.

Playroom DCC: `TextureCache::MaterializeColorClearNow` first attempts
`MaterializeDccClearOnGpu`. If it cannot legally handle that view, GPU-written metadata falls back
to `BufferCache::ReadMemory` before CPU interpretation. Binding type, volume status, slice limits,
format capabilities and backing-image properties can reject the GPU path. Which predicate rejects
these DISPATCH_INDIRECT accesses is unknown.

Pipeline stalls: cold application pipeline creation and lookahead are measured, but nested lookup
timers include work beyond the driver call. `SlowLookupTimer` records lookups lasting at least
1 ms; its shader-compile label does not prove a shader was compiled, and it misses shorter events.

## 5. PROPOSED FIX

First establish the invalid-resource producer and repair it with a focused correctness test.
Before performance changes, collect precise frame intervals and nested compiler/driver phases.
Instrument DCC fallback reasons so any extension of the GPU path is based on a specific legal case.
Do not remove synchronization or the conservative fallback merely because it is expensive.

## 6. FILES/FUNCTIONS TO MODIFY

These are proposed measurement locations, not implemented modifications:

| Source location | Measurement needed |
| --- | --- |
| `src/kernel/memory.cpp:977`, `ClampRangeSize` | On failure, capture thread, stack, address/size and caller resource provenance |
| `src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp:159` and `:170` | Record failed read context: shader/stage, resource-plan node, base pointer and offset; these are candidate callers, not a confirmed stack |
| `src/graphics/host_gpu/renderer/drainStats.cpp:629`, `CountFrame` | Export monotonic nanosecond fresh-frame intervals, frame ID and scene boundaries in an opt-in capture |
| `src/graphics/host_gpu/renderer/drainStats.cpp:232`, `FrameTimeSummary` | Avoid presenting overflow bucket 100 as an exact maximum; calculate requested statistics from precise intervals |
| `src/graphics/host_gpu/renderer/drainStats.h`, `SlowLookupTimer` | Separate cache hit/miss and actual compile/create counters from slow lookup duration |
| `src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp:436`, `CompileModule` | Separate emission, SPIR-V validation and shader-module creation zones |
| `pipelineCache.cpp`, `TranslateProgram` calls and `CreatePipeline` paths | Translation zones, immutable key, hit/miss, worker/thread and first-use status |
| `src/graphics/host_gpu/renderer/pipeline/shaders.cpp:637`, `CreateMonolithicPipeline`; `:689`, `CreateLibraryPart`; `:848`, `CreateLibraryPipeline` | Time actual Vulkan creation and link calls, distinct from parent lookup |
| `src/graphics/host_gpu/renderer/cache/textureCache.cpp:1541`, `MaterializeDccClearOnGpu` | Count each fallback condition with binding type, format, slices and resource identity |
| `src/graphics/host_gpu/renderer/cache/bufferCache.cpp:353`, `ReadMemory` | Connect producer tick, download range, submit, wait and publication to requester |
| `src/graphics/presentation/window/window.cpp:935` | Correlate fresh frame versus reused present; host present completion is not display scanout timing |

Keep measurement disabled by default; use bounded buffers and count dropped events rather than
blocking measured threads on file output. Capture the collection overhead with instrumentation
disabled/enabled before relying on results.

## 7. PATCH

No optimization code written. Evidence does not yet identify a safe minimal optimization for
Astro Bot. Existing dirty changes have been left intact and no commit was created.
This report is the only new artifact. Revert it by removing this newly added report file.

## 8. WHY THIS SHOULD HELP

Symbolization identifies the actual P0 producer. Exclusive phase zones distinguish translation,
validation, pipeline construction and driver compilation. DCC reason counters identify whether
a portable GPU implementation can replace a measured fallback while preserving metadata semantics.
Precise intervals allow objective smoothness comparisons. Expected FPS improvement is unknown.

## 9. CORRECTNESS RISKS

Do not mask invalid pointers, skip BVH dispatches, serve stale readbacks, skip writing draws,
alter guest timestamps or disable barriers to achieve the target. Preserve ordering, page dirty
state, range ownership and completion publication. Existing automatic game patches must be included
in the baseline identity: the Astro Bot log applies both an archive patch and a built-in Asobi patch.
The Playroom baseline also needs its exact patch set recorded.

## 10. VENDOR IMPACT

AMD: historical findings require repetition on current AMD CPU/GPU configurations.

NVIDIA: current host/test identifies RTX 4060 Ti; game-time capabilities and memory budgets still
need capture. Existing tests reporting conditional rendering do not establish why a particular
game view fails the GPU DCC path.

Intel: neither Arc nor integrated GPU nor Intel CPU evidence was supplied/found in the reviewed logs.

Keep a common renderer and select paths from queried capabilities. No vendor-specific optimization
is justified by this analysis.

## 11. TEST PROCEDURE

1. Record executable SHA-256, PDB identity, HEAD, complete dirty diff/untracked source snapshot,
   compiler/build type, launch arguments, game version, patches, scene/save/route, driver and Vulkan
   capabilities. The existing build directory is configured Release/clang-cl; that alone does not
   establish how an older measured binary was compiled.
2. Symbolize Astro Bot's stderr stack against its matching binary/PDB. Reproduce with the logged
   strict settings and a breakpoint at `ClampRangeSize`'s invalid-range branch. Preserve all checks.
   Capture caller arguments and the resource node that formed the address.
3. For performance, keep ray tracing enabled, relaxed readback and speculative draws disabled, and
   GPU timestamp scale at 100 as in the probes. Keep guest patches identical. Capture validation
   separately from the release performance pass and record both configurations.
4. Measure identical menu, quiet, dense, particle, geometry, traversal, cutscene and loading scenes.
   Capture 120 seconds per scene, three repetitions per build; alternate before/after order after a
   separate warmup. Keep loading and warmup as separately reported samples rather than discarding
   unexplained spikes. For 0.1% lows seek at least 10,000 intervals and disclose tail sample count.
5. Separate first execution, shader journal populated, pipeline cache populated and fully warm runs.
   Copy/preserve caches for the experiment; record driver disk cache control separately. An empty
   Vulkan pipeline cache alone is insufficient to label the run fully cold.
6. Capture Tracy (`--profile`) and drain stats (`--drain-stats 5`) plus ETW/WPA scheduling/CPU samples
   on Windows; use perf/scheduling traces on Linux. Record per-thread CPU, waits, lock contention,
   wakeups and migrations. Use GPU timestamps/capture tools for device workload and dependencies.
   Sample process memory and Vulkan heap usage/budget without querying every draw.
7. Export exact frame intervals. Define average FPS as frame intervals / elapsed seconds; nearest-rank
   percentiles in milliseconds; 1%/0.1% low as 1000 divided by the mean of the slowest ceil(1%/0.1%)
   intervals; population standard deviation; exact max; and target-budget miss counts. Keep presentation
   rate separate. Compare matched screenshots and captures, audio underruns and frame order.

## 12. ACCEPTANCE CRITERIA

The crash fix must eliminate the identified invalid producer without suppressing checks and pass
a targeted regression. An optimization stays only after repeated matched runs reduce its measured
cost and improve the relevant latency distribution beyond baseline run-to-run variation, with
no graphics, audio, timing, lifetime or validation regression. Report average, median, both lows,
P95/P99/max/deviation and memory changes even when they worsen. No observed evidence here proves
30/60/120 FPS across representative scenes. Keep each change separately reversible.

## 13. REGRESSION CHECK

Existing `LastTest.log` contains passing RT/settings/resource-control tests, while
`LastTestsFailed.log` still lists shader_resource_control. Treat that failure list as potentially
stale; neither file establishes a clean full-suite run for this source state. Tests were not rerun.

After a source fix, rebuild and run the targeted test plus resource materialization/tracking,
shader CFG/RT, memory, scheduler and frame-statistics tests as appropriate. Then run Astro Bot,
Astro's Playroom, another demanding title and a lightweight title with fixed scenes in cold/warm
states. Include AMD/NVIDIA/Intel GPUs and AMD/Intel CPUs where available; explicitly mark unavailable
cells. Check texture/depth/lighting/shadow/transparency/particle/UI/video/color correctness,
loading, saves, audio and long-duration stability. Synthetic tests do not replace gameplay checks.

## 14. NEXT BOTTLENECK

After the P0 producer is repaired, measure pipeline/lookahead exclusive time and DCC fallback
reasons first. Then sample `Thread_Gpu` per-draw materialization, PM4 parsing, submission and guest
readback dependencies in actual scenes. Follow current measurements rather than transferring the
old AMD bottleneck percentages to this NVIDIA host.
