# KYTYPS5 PERFORMANCE ANALYSIS — gameplay log review

## 1. CURRENT PERFORMANCE

`performance-20261007-153457-556.txt` records an approximately twelve-minute
Astro Bot session with RT off, GPU zones enabled, and normal window closure.
Late intervals fall to 14–16 FPS. One five-second interval produces 70 game
frames and 300 presentations; repeated presentation is not 60 FPS gameplay.
Its frame times are p50 66 ms, p95 83 ms, and maximum 84 ms.

## 2. BOTTLENECK CLASSIFICATION

Mixed GPU work and gaps between GPU command buffers. Host command preparation
and guest memory fault handling warrant investigation. This differs from the
earlier startup probe, where expensive draw shaders dominated measured GPU work.

## 3. EVIDENCE

The interval beginning at line 206237 reports 25.57 ms GPU busy and 45.99 ms
GPU gap per game frame. Zones attribute 14.54 ms to draws, 5.45 ms to compute,
2.36 ms to buffer copies, and 1.81 ms to image copies. Shader compilation costs
only 1.09 ms across the whole interval; timeline waits total 65.12 ms.

The top write-fault site, guest PC `0x920003e55` on `tbb_thead`, records
92,217 faults and 1,209.56 ms aggregate handler time. The eight displayed
write-fault sites total 2,009.60 ms. These are aggregate times across guest
threads, not a serial two-second frame stall. GPU thread idle is only 0.61 ms.

Metadata identifies emulator SHA256
`08B70B6F2EF4F300C7E494E942B9827807BF2C9EC25F44997ADB44C5AA6B6766`,
game `C:\ROMS\PPSA21567`, timestamp scale 100, and auto optimization off.
The startup message and applied RT/GI bypass independently confirm RT off.
No fatal error or Vulkan error result is recorded in this session.

A separate new `_kyty1.txt` records RT enabled, the guest RT renderer preserved,
RTX 5080, build banner `ef6a730-dirty`, and game location
`C:/Ps5 Emulator/Roms/PPSA21567`. It also records normal window closure, with
no fatal crash signature. No executable hash, drain statistics, or draw phase
timings are available for that run. Do not treat it as a same-build or
same-hardware comparison. The older `_kyty.txt` semaphore crash is unchanged.

## 4. ROOT CAUSE

The exact cause of the gaps is unproven. The logs support investigating CPU
render preparation and frequent guest writes/cache invalidation. They do not
prove RT causes these drops, or that removing synchronization would help.

## 5. PROPOSED FIX

Measure render CPU phases in the same slow scene using the existing
`KYTY_DEBUG_DRAW_PHASES=all` diagnostic. Correlate expensive phases with memory
fault and cache activity before choosing an implementation change.
`tools/run-performance.ps1 -CpuPhases` now enables that diagnostic. Its metadata
also captures tester/scene labels, CPU, display adapters and drivers, RAM, OS,
prelaunch settings, build hash, and completion status for comparisons across PCs.

## 6. FILES/FUNCTIONS TO MODIFY

No renderer modification is justified by this review alone. Inspect
`RenderContext::HandleFault`, buffer and texture cache invalidation, and
`DrawPhaseTimer::End`. `CommandScheduler::ReadTimestamps` measures the union
of instrumented command-buffer intervals and gaps between them; it is not a
system-wide GPU utilization counter.

## 7. PATCH

This review adds evidence documentation and updates the monitor's processed
file hashes. The capture script and `tools/TESTER-LOGS.md` support collecting
comparable cases. It makes no rendering or synchronization change.

## 8. WHY THIS SHOULD HELP

CPU phase measurements distinguish command preparation overhead from guest
work, memory coherence, and actual GPU shader cost. FPS alone cannot do that.

## 9. CORRECTNESS RISKS

Fault-based invalidation and readbacks enforce guest/host coherence. Preserve
their semantics. Diagnostic logging and GPU zones add overhead, so compare
equally instrumented runs and verify final performance without instrumentation.

## 10. VENDOR IMPACT

The measured performance run belongs to the local RTX 4060 Ti setup. The
RTX 5080 log provides separate RT configuration evidence, not comparable FPS.

## 11. TEST PROCEDURE

Repeat the same gameplay scene with CPU draw phases and GPU zones enabled.
Keep the build, game, RT state, saves, and launch options consistent. Capture
scene location and timings before and after any subsequent minimal fix.

## 12. ACCEPTANCE CRITERIA

Identify the dominant CPU phase or coherence cost before modifying it. Require
same-scene frame-time improvement and correct output/input after a fix. This
review establishes neither sustained 60 FPS nor resolution of the old crash.

## 13. REGRESSION CHECK

Both newly reviewed logs have normal shutdown markers and no recorded fatal
error. Capture script parsing and hardware inventory succeeded locally. An
end-to-end script check using a fake emulator verified metadata, the prelaunch
settings snapshot, completion status, and restoration of the CPU phase diagnostic
environment. No game was launched for that check; no engine rebuild is needed.

## 14. NEXT BOTTLENECK

Resolve the source of command-buffer gaps and heavy guest write-fault activity.
Then revisit the remaining GPU draw, compute, and copy costs: even the measured
25.57 ms GPU busy time exceeds the 16.67 ms budget for 60 FPS.
