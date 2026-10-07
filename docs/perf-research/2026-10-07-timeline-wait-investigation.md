# KYTYPS5 PERFORMANCE ANALYSIS

## 1. CURRENT PERFORMANCE

The newly supplied Astro Bot log fails in `MasterSemaphore::Wait` at the call to
`vkWaitSemaphores`. No frame-time samples or crash location were supplied. RT is
disabled in this run: the log explicitly skips the guest BVH dispatch. This is
a different failure from the image-descriptor translation crash corrected earlier.

## 2. BOTTLENECK CLASSIFICATION

GPU completion failure with insufficient diagnostics. The current evidence does
not establish a shader fault, device loss, memory exhaustion, or synchronization
defect as the root cause.

## 3. EVIDENCE

The attachment `3187d71a-72dc-4fae-bdda-95ebe6d90bbc/Pasted text.txt` ends with
two concurrent failures at `masterSemaphore.cpp:53`. That line checked only
`result != vk::Result::eSuccess`; it discarded the actual Vulkan return code.
The wait uses `UINT64_MAX`, not a short application timeout. Shader counts reached
VS 270, PS 196, CS 102, and GS 8. Counts do not measure FPS or identify the last
completed GPU command.

## 4. ROOT CAUSE

Unknown from the supplied output. The semaphore is the point that observes the
failure, not sufficient evidence that the semaphore itself is defective. The
RT-off built-in patch's old name said "Auto-Optimizer" even when automatic tuning
was disabled; its selection depends on the RT setting.

## 5. PROPOSED FIX

Log the Vulkan result name, numeric result, requested completion tick, last known
GPU tick, and next allocated tick before terminating. Correct the RT-off patch
label to describe its actual behavior. Preserve all waits and fail on GPU errors.
The launcher was independently found rewriting timestamp scale to 125 on launch;
both settings writers and its launch arguments now use 100 for honest GPU timing.

## 6. FILES/FUNCTIONS TO MODIFY

`src/graphics/host_gpu/renderer/masterSemaphore.cpp`: constructor, `Refresh`, `Wait`.
`src/loader/gamePatch.cpp`: built-in Asobi patch display name.
Launcher `mainDialog.cpp` and `configurationListWidget.cpp`: timestamp settings
writers and launch arguments; packaged `bin/kyty_settings.ini`: timestamp scale.

## 7. PATCH

Replace generic assertions at the three Vulkan calls with explicit fatal messages.
The RT-off patch now reports "Asobi stability fixes (guest RT/GI bypass enabled)".
No guest patch bytes, semaphore values, timeouts, or completion semantics change.
The timing correction is not established as the cause of this GPU completion failure.

## 8. WHY THIS SHOULD HELP

The next reproduction can distinguish Vulkan device loss from other API failures
and expose the completion backlog. The label separates the RT-off compatibility
patch from the independent automatic optimization setting.

## 9. CORRECTNESS RISKS

This is diagnostic work, not a crash repair. Failed GPU work must never be marked
complete or reused. The exit path remains fatal. A bounded startup test cannot
prove later interactive gameplay is correct.

## 10. VENDOR IMPACT

Platform-independent Vulkan diagnostics. Local execution uses an RTX 4060 Ti;
AMD/Intel behavior is untested.

## 11. TEST PROCEDURE

Build the emulator, run controller and RT settings regressions, and execute a
bounded Astro Bot startup probe with RT off, automatic optimization off, timestamp
scale 100, and asynchronous options matching the installed configuration. Vulkan
validation remains off for this reproduction; its separate shader-interface
failure is already documented and has not been resolved.

## 12. ACCEPTANCE CRITERIA

Build and selected regressions pass. Report whether the bounded run reproduces
the supplied failure, with its actual result and executable hash. Claim a crash
repair only after identifying and correcting its underlying cause.

## 13. REGRESSION CHECK

Emulator build passed; controller and RT settings tests passed (2/2). The current
change preserves the previously tested mask and GPU descriptor repairs.

The RT-off runtime probe lasted 92.1 seconds with no fatal/error log entry, then
was deliberately stopped. It reached frame 2838; interactive gameplay was not
performed. Evidence: `_Build/rt/probes/semaphore-rt-off-20261007-140836/result.json`
and `stdout.log`. Tested emulator SHA256:
`58D8B2C61CA43876EB3DE9201398DF4E1C21929EE3BA08788D5994C4F1CD5DFA`.

The launcher build also passed. Diagnostic binaries were installed into repository
`bin` and Desktop `Kytytest`, with prior files backed up. Hashes and backup location
are recorded in `_Build/rt/semaphore-diagnostic-deployment.json`. Desktop settings
retain RT enabled and automatic optimization off, with timestamp scale restored
to 100. The already published prerelease asset has not been replaced by this
diagnostic build.

## 14. NEXT BOTTLENECK

Obtain the failing run's actual Vulkan result and game location. Strict Vulkan
shader-interface validation and pipeline compilation stalls remain unresolved.
