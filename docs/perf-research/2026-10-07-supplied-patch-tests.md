# KYTYPS5 PERFORMANCE ANALYSIS

October 7, 2026. User-supplied patch startup tests; no renderer or patch edits.

## 1. CURRENT PERFORMANCE

| Game / emulator RT option | Result | Duration |
| --- | --- | --- |
| Astro's Playroom / on | No fatal crash before deliberate time-limit stop | 61.436 s |
| Astro Bot / on | Fatal Vulkan shader-interface validation error | 38.368 s |
| Astro Bot / off | Same fatal Vulkan shader-interface validation error | 37.379 s |

These were unattended startup runs, not representative interactive gameplay benchmarks.
Playroom's logged frame rates varied substantially; no minimum 60 FPS claim.

## 2. BOTTLENECK CLASSIFICATION

With the supplied Astro Bot patch, startup reaches a graphics pipeline interface
failure rather than the earlier RT-preserving invalid host read at 0x30.

## 3. EVIDENCE

Logs confirm both Playroom entries and all three Astro Bot entries applied.
Both Astro Bot runs report fragment input Location 1 Component 0 (float32 vec4)
without a corresponding vertex-stage output. The validation callback terminates
the emulator at vulkanWindow.cpp:854. Both exit -1073740791. Playroom has no
fatal or validation-error log entries; its -1 exit is the deliberate test stop.

## 4. ROOT CAUSE

The immediate failure is an incompatible vertex/fragment SPIR-V interface.
The generating shader, pipeline key and compiler decision remain to be captured;
the underlying translator/cache defect is not established by these startup tests.

## 5. PROPOSED FIX

Capture the failing pipeline's paired shaders and stage metadata, reproduce their
interface mismatch, then correct interface generation/linkage with a focused
regression. Do not suppress validation to disguise the mismatch.

## 6. FILES/FUNCTIONS TO MODIFY

None modified in this test. Investigate shader stage interface emission and
graphics pipeline construction after capturing the failing shaders.

## 7. PATCH

Used original Desktop/_Patches/PPSA01325.json and PPSA21567.json unchanged.
Playroom selects non-tiled deferred lighting and GI-unavailable state. Astro Bot
selects non-tiled deferred lighting, bypasses GI probe computation and disables
GI probes/lighting shaders. They do not enable ray tracing. They bypass GI and
RT-related rendering work, rather than turning the emulator's RT setting on.
This does not establish that every possible RT feature in the game is disabled.
External patch writes are applied even when emulator RT is enabled.

## 8. WHY THIS SHOULD HELP

The patches avoid previously failing lighting work and expose a later failure
in Astro Bot. This identifies a separate correctness blocker; it is not evidence
that full RT or gameplay stability has been fixed.

## 9. CORRECTNESS RISKS

GI/lighting removal changes visual fidelity. Short startup tests cannot establish
long-term stability or correct rendering. No controller gameplay or image checks.

## 10. VENDOR IMPACT

NVIDIA: tested on RTX 4060 Ti with Ryzen 7 5800X. AMD and Intel: not tested.
Windows only. No vendor-specific changes.

## 11. TEST PROCEDURE

Fresh per-run directories, current bin/kyty_emulator.exe, original matching patch,
Quality preset, auto optimization off, shader/Vulkan validation on, relaxed
readback and speculative draws off, pipeline libraries off, timestamp scale 100,
drain stats every five seconds. Stop still-running processes at approximately
60 seconds. Astro Bot was repeated with only the RT boolean changed.

Exact arguments, hashes, elapsed times and exit codes are recorded in result.json
under _Build/rt/probes/supplied-patches-20261007-104418/{PPSA01325,PPSA21567,PPSA21567-rt-off}.
Each directory contains stdout.log and stderr.log. Executable SHA256:
42DE1D62877C76FFD9F2B48C219F58FBB1BE52E4DF7042A3446BE9DEED62A72D.

## 12. ACCEPTANCE CRITERIA

Original patch application confirmed; fatal versus deliberate stop distinguished;
RT-on and RT-off failure compared. All test-procedure criteria satisfied.
Astro Bot startup stability criterion failed. Full RT was not under test.

## 13. REGRESSION CHECK

Playroom remained alive through the bounded run. Astro Bot reproduced the same
interface error with both RT settings. No code changes, dependency installs or
patch changes were needed. No commits or pushes.

## 14. NEXT BOTTLENECK

The missing Location 1 vertex output is the next blocker on the supplied-patch
path. The RT-preserving descriptor chain remains a separate unresolved blocker.
