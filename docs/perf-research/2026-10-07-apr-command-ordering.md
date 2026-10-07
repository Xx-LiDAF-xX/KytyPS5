# KYTYPS5 PERFORMANCE ANALYSIS — APR dependency ordering

## 1. CURRENT PERFORMANCE

No gameplay FPS improvement is established. The prior filesystem regression
failed before dependent APR file reads because an unsatisfied address wait did
not retain its submission. This change addresses command correctness.

## 2. BOTTLENECK CLASSIFICATION

Guest asynchronous memory/storage command execution and dependency ordering.

## 3. EVIDENCE

`CommandBufferWaitOnAddress` appended a no-op. APR/AMM submit wrappers executed
commands synchronously, ignored priority, and published successful completion
immediately. The existing regression submitted a blocked APR read followed by
an AMM fence producer; later commands ran despite the unsatisfied dependency.

## 4. ROOT CAUSE

The executor stored read, write, mapping and event commands but no address waits.
Submission bookkeeping had no pending/completed state. Wait APIs removed entries
without waiting for execution. A synchronous wait alone would deadlock callers
that need to submit the command which releases it.

## 5. PROPOSED FIX

Capture command metadata at submit and execute it on a background worker. Keep
FIFO ordering within each priority, retain blocked heads, and let other priorities
progress. Resume after the last completed command. Publish result buffers before
notifying completion waiters, and stop the worker before graphics/memory teardown.

## 6. FILES/FUNCTIONS TO MODIFY

`src/libs/libAmpr.cpp`: address wait recording, resumable executor, submission
queue, completion bookkeeping, APR/AMM wrappers and shutdown.
`src/libs/libs.h` and `src/emulator.cpp`: worker lifecycle registration.
`tests/KernelFileSystemTests.cpp` and `CMakeLists.txt`: APR regression selection
and additional resume/snapshot/shutdown checks.

## 7. PATCH

Added immutable per-submission metadata snapshots, an execution cursor, a shared
priority queue and `std::jthread` worker. Address comparisons cover the existing
SDK regression cases: equality, unsigned greater/less, inequality, wrapped
greater-or-equal, and signed greater/less. Aligned fence reads/writes use acquire/
release atomics. Blocked heads are revisited after producer completion or a 1 ms
timed wait for external CPU stores; an empty worker waits without periodic polling.
Shutdown cancels queued dependencies, publishes cancellation results, and joins
the worker. Submitted operations already executing finish before normal teardown.

## 8. WHY THIS SHOULD HELP

Dependent file reads and memory commands cannot overtake their recorded address
wait. Submit calls return so later producers can be submitted. Resumption does
not replay preceding writes, events, file reads, or mappings.

## 9. CORRECTNESS RISKS

Guest command destinations and fence/result memory must remain valid until
completion. Pending metadata is captured, but guest destination memory remains
live. Counter waits, additional wait flags and unrecognized raw serialized command
records are not implemented by this change. The existing AMM byte-count argument
handling is unchanged. Blocking I/O already in progress is not forcibly cancelled.
No renderer synchronization or shader operations were removed.

## 10. VENDOR IMPACT

Queue and fence handling do not depend on GPU vendor. Built and tested on Windows
with the local RTX 4060 Ti setup; Linux/macOS builds and other tester GPUs remain
unverified.

## 11. TEST PROCEDURE

Reproduce the original failure, rebuild emulator and filesystem tests, and run
the focused `apr_command_ordering` CTest ten times. Run sixteen selected APR,
privacy, CLI, haptics, controller, RT and resource regression checks. Run a bounded
startup probe with separate saves and copied caches, RT off, timestamp scale 100,
auto optimization off, file logging, and the rebuilt executable.

## 12. ACCEPTANCE CRITERIA

Blocked waits retain later commands at their priority. Another priority can
release the fence. All existing comparison cases pass. A CPU fence store releases
work without another submission. Resetting the guest header does not change
captured work, preceding commands do not repeat, and shutdown publishes cancellation.
Require a tester replay of the affected gameplay scene before claiming a game fix.

## 13. REGRESSION CHECK

Focused APR checks passed ten consecutive runs; all sixteen selected regressions
passed. The full filesystem test progresses past APR and then fails an independent
Windows per-call nonblocking socket expectation. Production explicitly returns
unsupported for that case, while another existing test expects that policy; do
not report the full suite as passing or change socket semantics in this patch.

The roughly 54-second startup probe recorded no fatal error or async execution
error. It generated a dated `_kyty` log with no detected Windows/UNC/home path
patterns. It was stopped deliberately; no normal shutdown or interactive gameplay
claim follows from that probe. Its early intervals overlap regression checks and
are not used for performance comparison. Evidence is local under `_Build/rt`.

## 14. NEXT BOTTLENECK

Replay the tester's slow/failing scene on this build. Continue the CPU preparation
and memory coherence investigation, and separately reconcile Windows socket tests
with the supported per-call nonblocking contract.
