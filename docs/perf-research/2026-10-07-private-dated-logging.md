# KYTYPS5 PERFORMANCE ANALYSIS — private, automatic tester logs

## 1. CURRENT PERFORMANCE

Tester logs cover multiple builds, settings, and GPUs. This change improves
automatic diagnostics; it establishes no FPS gain.

## 2. BOTTLENECK CLASSIFICATION

Diagnostic coverage and privacy defects: silent defaults, reused log filenames,
renderer diagnostics bypassing file output, and host paths in collected metadata.

## 3. EVIDENCE

Core and launcher defaults were Silent. `_kyty.txt` was opened with truncation.
Numerous renderer diagnostics used host printf directly. The collector serialized
absolute paths, raw settings contents, and display-device instance identifiers.
Guest descriptor writes used host stdout/stderr directly, bypassing configured
file output. Fatal reports already shortened source paths to source basenames.

## 4. ROOT CAUSE

Output paths and message formatting were handled inconsistently across modules.
Guest output can split sensitive text over several writes. Hardware inventory
mixed useful capabilities with unnecessary host identifiers.

## 5. PROPOSED FIX

Make File the default, migrate launcher profiles, generate a unique UTC-dated
filename for the default `_kyty.txt`, and centralize filtering. Keep useful Vulkan
results, shader hashes, source basenames, and frame timings. Collect only necessary
hardware and settings fields. Retain explicit custom log filenames.

## 6. FILES/FUNCTIONS TO MODIFY

Config defaults, launcher profile loading, common logging, host printf diagnostic
call sites, guest printf/stdout routing, thread sampler exports, and tester capture.
The privacy filter handles paths, known host identities, network addresses and
URLs, and sensitive field values. Guest output is buffered per thread to complete
lines before filtering. Oversized lines are omitted with an explicit marker.

## 7. PATCH

Implemented automatic dated file output and the privacy filter. Internal host
printf diagnostics now use `Log::Printf`; guest printf and descriptor output use
`Log::WriteGuest`. File initialization/write errors use filtered console output.
Metadata excludes paths, device instance identifiers, and raw settings files,
while retaining executable/game hashes and numeric/boolean diagnostic settings.

## 8. WHY THIS SHOULD HELP

Each launch retains its diagnostic history and testers no longer need to select
File manually. Comparable hardware/settings evidence is retained without storing
personal directory layouts in newly collected logs and metadata.

## 9. CORRECTNESS RISKS

Filtering may hide extra context when a path contains spaces. Guest output without
a newline remains buffered and incomplete trailing lines are not emitted. Diagnostic
collection has CPU/I/O overhead; it is not a rendering optimization. Arbitrary
personal information in freeform prose cannot be reliably recognized, so tester
notes must describe gameplay without personal details. Existing received files
are retained; they are not automatically rewritten. Signal-sensitive page-manager
fail-fast diagnostics retain their minimal numeric/static output.

## 10. VENDOR IMPACT

Logging is common to all GPU vendors. Windows build/runtime tests passed on the
local setup. POSIX UTC conversion and process IDs have separate compile branches;
Linux/macOS builds were not run in this Windows environment.

## 11. TEST PROCEDURE

Rebuild the full emulator and launcher plus targeted tests. Exercise Windows,
UNC, POSIX, and relative path filtering, private fields, network values, fatal
output, split guest writes, preserved Vulkan result/source/driver information,
and two successive dated file launches. Verify collector metadata using a fake
emulator without launching a game. Run controller, haptics, RT, resource, CLI,
and filesystem regressions.

## 12. ACCEPTANCE CRITERIA

Default file output and unique filenames work; tested private values do not
appear in generated output; useful crash signatures remain intact. Preserve
renderer synchronization and shader behavior. Do not claim all bugs fixed.

## 13. REGRESSION CHECK

Full emulator and launcher builds passed. Fifteen selected CTests passed,
including the new privacy tests. Collector parsing and sanitized metadata checks
passed. The broader filesystem test reproducibly fails its APR dependency-ordering
assertion. The existing synchronous APR executor does not retain an unsatisfied
wait or queue submissions by priority. This is an unresolved engine issue,
not a passing result or evidence that logging changes fixed it.

## 14. NEXT BOTTLENECK

Investigate APR/AMM asynchronous command execution and dependency ordering as a
separate correctness change, with the existing failure as a regression case.
Continue same-scene CPU/render-coherence measurements across tester hardware.

Follow-up: `2026-10-07-apr-command-ordering.md` records the subsequent address-wait
and asynchronous submission fix. Its focused tests pass; the broader filesystem
suite now reaches a separate Windows socket contract mismatch.
