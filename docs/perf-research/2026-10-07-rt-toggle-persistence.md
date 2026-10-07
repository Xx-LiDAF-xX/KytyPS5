# KYTYPS5 PERFORMANCE ANALYSIS

## 1. CURRENT PERFORMANCE

The user reports similar FPS with RT toggled through the launcher or F2. No
same-scene timing measurements were supplied. F2 intentionally changes the next
launch setting, leaving the current renderer unchanged until the game restarts.

## 2. BOTTLENECK CLASSIFICATION

Settings persistence and unclear active/pending state. Similar FPS alone does not
identify a rendering or CPU bottleneck and does not prove that RT stays enabled.

## 3. EVIDENCE

F2 previously saved its pending choice only when closing the overlay. Launcher
`WriteSettings` unconditionally rewrote `kyty_settings.ini` with global RT defaults
on saves and window resizes, even while a per-game session was running. The
emulator reloads that file periodically. The launcher imports the game's RT
choice after its tracked process exits, so an overwrite could lose that choice.

## 4. ROOT CAUSE

Two components owned the active settings file. Global launcher defaults could
overwrite the running game's next-launch RT preference. Separately, the checkbox
showed pending RT without displaying the active session's RT state.

## 5. PROPOSED FIX

Save the F2 RT choice immediately, report failures, show active and pending states,
and prevent launcher settings writes from replacing a running game's file. Keep
restart semantics because RT also controls guest code patches at startup.

## 6. FILES/FUNCTIONS TO MODIFY

`emulatorConfig`: add `SaveRayTracingEnabledOnRestart`.
`systemOverlay.cpp`: F2 save and status display.
Launcher `configurationListWidget.cpp`: running-game guard in `WriteSettings`.
`emulator.cpp`: explicit RT-off startup log.
`RayTracingSettingsTests.cpp`: immediate persistence and failed-save coverage.

## 7. PATCH

The new save API updates the pending state only after a successful settings write.
F2 displays "Ray tracing in this session" and "Restart required" when appropriate.
Launcher configuration still persists, but its emulator INI synchronization is
skipped while any game row is running. Per-game launch arguments continue to
select the active mode on the next launch.

## 8. WHY THIS SHOULD HELP

The user can distinguish an active RT change from a saved preference. Resizing or
saving the launcher cannot erase a running game's F2 choice. Startup logs clearly
report RT on or off even if the game has not yet submitted a BVH shader.

## 9. CORRECTNESS RISKS

Live RT switching remains unsupported. The active renderer is preserved on save
failure and when a restart is pending. External game patches can still bypass RT.
No claim is made that every visual RT effect is correct.

## 10. VENDOR IMPACT

Settings behavior is platform-independent. Runtime probes use the local RTX 4060
Ti. Linux/macOS and other vendors are not tested in this change.

## 11. TEST PROCEDURE

Build emulator, launcher, and settings tests. Run RT settings and shader RT IR
regressions. Launch Astro Bot from separate RT-off/on settings files without CLI
RT overrides; inspect the startup mode and built-in patch selection. Stop both
probes at a bounded limit. These probes do not compare interactive FPS.

## 12. ACCEPTANCE CRITERIA

Immediate RT persistence succeeds; a failed file write does not change pending or
active state. Runtime settings select the requested mode. Manual F2 interaction,
launcher resize, game exit, and relaunch still need UI confirmation.

## 13. REGRESSION CHECK

Emulator and launcher builds passed. RT settings and shader RT IR tests passed
(2/2). The settings test covers an unwritable path using a directory in an isolated
temporary folder. RT-off shader IR retains its tested skip-dispatch behavior.

Separate startup probes read `ray-tracing = false` and `true` from their settings
files without a CLI RT override. OFF logged guest BVH disabled and selected the
RT/GI bypass patch; ON logged compute BVH enabled and preserved the guest RT
renderer. Evidence is under `_Build/rt/probes/rt-toggle-20261007-142242/`.
Tested emulator SHA256:
`09ECB9566B3F26C9C7E4711205B7EB6E9CF83D39888CDEFD54A76E913A52AFD5`.
Updated emulator and launcher are installed into repository `bin` and Desktop
`Kytytest`; backup paths and hashes are in `_Build/rt/rt-toggle-deployment.json`.
The published prerelease has not been replaced.

## 14. NEXT BOTTLENECK

Compare the same gameplay scene after separate RT-off/on launches. Measure CPU,
GPU, pipeline compilation, and readback time before attributing similar FPS to RT.
The separate semaphore failure and strict Vulkan shader-interface issue remain
unresolved.
