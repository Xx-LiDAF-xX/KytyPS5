# KYTYPS5 PERFORMANCE ANALYSIS — Suspend points and validation follow-up

## 1. CURRENT PERFORMANCE

No gameplay FPS improvement is established. The command-lane regression now
completes in approximately two seconds instead of reaching its 90-second timeout.
That is a regression-test measurement, not a gameplay benchmark.

## 2. BOTTLENECK CLASSIFICATION

Guest GPU frame-boundary synchronization, shader wave emulation, cached image
clear handling and stale regression assumptions. An intermediate validation
attempt encountered insufficient Windows commit headroom; subsequent runs
completed after sufficient memory became available.

## 3. EVIDENCE

SuspendPoint ignored the configured frame lead and always called WaitForIdle.
Its native completion callback released an initially signaled binary semaphore
that no caller acquired. The one-frame-ahead test held the guest command lane
behind a gate, expecting the first boundary to return; the unconditional wait
prevented the caller from releasing that gate.

## 4. ROOT CAUSE

CPU queue draining replaced the bounded boundary protocol, while the old native
completion semaphore remained. Releasing a binary semaphore beyond its permitted
count violates its contract. Distinct boundary completions need distinct credits.

## 5. SOURCE CHANGE

Each boundary now owns a shared promise/future completion ticket. Enqueue order
and the existing outstanding-submission set identify the required older work.
The host retains at most the configured number of recent boundary tickets, then
waits for older command processing and native completion without holding the
queue mutex. Native callbacks retain their ticket independently of GuestGpu.

## 6. SYNCHRONIZATION

Zero lead waits for the caller's own boundary, including concurrent callers.
One lead allows the first outstanding boundary and waits for the preceding one
when another arrives. Vulkan barriers, timeline completion, priority callback
ordering and borrowed command memory remain part of the protocol. GPU-thread
callers do not block their own command lane. Higher frame-lead values retain the
same bound but do not yet have dedicated stress coverage.

## 7. AUDIO REGRESSION

Production output buffering uses a 60 ms target, but the test retained the old
40 ms block counts. Twelve 256-frame blocks at 48 kHz prime the current queue;
the twelfth anchors the clock and the thirteenth waits. The updated test preserves
its priming, drift, queue-failure, underrun, recovery, batching and speaker checks.
Production audio scheduling was not changed in this follow-up.

## 8. TILER REGRESSION

The scratch pool returns buffers only after their scheduler tick completes.
The old test required an in-flight buffer to be reused. The test now requires
distinct live workspaces with exact requested spans and retains all GPU-content
and CPU/GPU tiling parity checks. Production tiling was not changed.

## 9. LOG EVIDENCE

Two additional older logs were reviewed and their hashes recorded. One mixes
older build labels and records an image creation fatal at image.cpp:737 for a
2048x2048x1 image, format 146, one layer and twelve levels. It omits VkResult.
The other records RT on with an older build and no fatal marker. Neither proves
anything about the current executable without its hash and unambiguous identity.

## 10. ALLOCATION DIAGNOSTIC

CreateImage now logs the numeric Vulkan allocation result, extent, format,
layers, mip levels, usage, creation flags and sample flags before the existing
heap budget report. Allocation failure still propagates through the existing
fatal path. This improves diagnosis and does not fix or hide the reported crash.

## 11. VALIDATION

The emulator and affected test executables built. Ten focused checks passed after
the initial ticket implementation: APR ordering, audio timing, haptics, RT settings,
privacy logging, RT shader/IR coverage, command lane, suspend point and GPU tiling.
Suspend-point coverage includes native completion, independent compute/host lane
progress, graphics FIFO/reset and shutdown callback lifetime.

## 12. FOLLOW-UP VALIDATION

The concurrent zero-lead assertion subsequently passed with sufficient memory.
Additional fixes preserve whole-wave ballot and first-active-lane results across
emulated wave halves, and correctly reuse the emitted scalar branch result.
The previous const instruction pointer could select the Boolean IR constructor
instead of an instruction reference. SPIR-V validation and GPU wave64 tests pass.

A one-mip descriptor can now clear its selected view when backed by a cached
multi-mip image. CPU and GPU metadata-clear tests verify the selected mip contents
and preservation of higher mips. A stale PM4 test classified the live pixel shader
user-data address registers as removed holes; it now validates both direct and
indirect 64-bit address updates while retaining the genuine hole checks.

The full sequential run passed 72 of 75 tests in 68.20 seconds. The remaining
failures concern a conflicting Windows socket expectation and host LDS limits
in two shader-suite configurations. Rebuilt RT shader/IR, wave64 and frame-boundary
checks passed together (five tests). An isolated Astro Bot startup with RT enabled
remained running for 40 seconds with no captured crash marker. The test process
was then stopped. This is startup coverage, not gameplay stability or FPS evidence.

## 13. PRIVACY

The current tracked/new file scan found no private host identities or credentials.
The local monitor state now retains log basenames and hashes instead of absolute
host paths. Raw logs and local build/test output remain excluded from Git and
release packaging.

## 14. REMAINING WORK

Investigate host LDS limits without silently truncating guest workgroup memory,
resolve the socket contract, stress higher frame-lead values and benchmark actual
gameplay. Reproduce image allocation with a diagnostic log before changing
allocation policy. The published prerelease predates these local follow-ups;
the tested executable is the local build identified as cb02347-dirty.
