# KYTYPS5 PERFORMANCE ANALYSIS

October 7, 2026. Windows Release / clang-cl, base ef6a730a. Ryzen 7 5800X,
RTX 4060 Ti. Local changes; no commit or push. This report follows the resource
validation restoration report from the same date.

Touchpad input follow-up: the in-game Quality/Performance selector was removed.
Settings now open only with F2; touchpad, Guide and stick buttons no longer open
the emulator settings panel. Touchpad button and touch-coordinate forwarding in
window.cpp is retained. The emulator rebuilt successfully; build log:
_Build/rt/touchpad-settings-build.log. Interactive controller testing remains pending.

## 1. CURRENT PERFORMANCE

Astro Bot with RT enabled still exits 321 on an invalid eight-byte read at 0x30.
The new probe reached this failure in 18.063 seconds. This is startup duration,
not a frame-rate measurement. No stable gameplay or 60 FPS claim.

## 2. BOTTLENECK CLASSIFICATION

The remaining RT blocker is scalar descriptor/pointer materialization. Separate
confirmed defects affected launcher defaults, visibility of RT settings and shader
cache identity.

## 3. EVIDENCE

The launcher defaulted auto_optimize to true and loaded old saved true values.
It passed --auto-optimize without a value. RT was stored and passed to the emulator
but had no checkbox. ShaderMapUserData computed a full code hash, discarded it,
then allowed metadata hashes to identify different executable code.

## 4. ROOT CAUSE

Implicit hardware profiles changed settings without a visible control. Declared
shader hashes are not sufficient source identity after code changes. Hot settings
reload also rewrote guest RT bypass instructions while the game was running.

## 5. PROPOSED FIX

Disable automatic optimization in launcher defaults, old-profile loading and
launch arguments. Expose RT settings with restart semantics. Store the full code
hash computed during registration. Keep Quality mode's timestamp scale at 100%.

## 6. FILES/FUNCTIONS TO MODIFY

launcher/configuration.h; configurationEditDialog.cpp and its .ui; mainDialog.cpp;
configurationListWidget.cpp/.h; common/emulatorConfig.cpp/.h;
presentation/systemOverlay.cpp; shader/shader.cpp; bin/kyty_settings.ini;
tests/RayTracingSettingsTests.cpp. Implementation is in the working tree.

## 7. PATCH

Launcher always supplies --auto-optimize false; configuration files explicitly
save false. Manual CLI opt-in remains available. Launcher Graphics settings expose
an experimental RT checkbox. F2 settings retain a separate pending RT choice,
save it on close and leave the active rendering path intact. After the launched
process ends, the launcher retains that choice for the game's next launch.
External RT settings reload is also deferred; it no longer calls the guest-code
patch toggler. Shader registration stores XXH3 of the complete registered code.

## 8. WHY THIS SHOULD HELP

Older launcher profiles no longer silently enable automatic tuning. RT changes
do not race executing guest code. Executable changes with identical metadata no
longer reuse a shader source identity. The previously failing full shader_cfg
suite now passes. These changes do not fix the descriptor chain or prove a speedup.

## 9. CORRECTNESS RISKS

RT changes require restarting the game, and external patches that disable RT
must be disabled separately. Shader identifiers change where metadata hashes
previously replaced source hashes, so earlier shader cache entries may be unused.
UI controls compile successfully; interactive GUI operation and long gameplay
were not verified. Linux and macOS builds remain unverified.

## 10. VENDOR IMPACT

NVIDIA: the local Astro RT probe reproduced the existing failure. AMD and Intel:
not run. No vendor-specific renderer changes in this patch.

## 11. TEST PROCEDURE

```powershell
cmake -S . -B _Build/rt -DKYTY_BUILD_LAUNCHER=ON -DQt6_DIR=C:/Qt6/6.10.3/msvc2022_64/lib/cmake/Qt6
cmake --build _Build/rt --target launcher shader_cfg_tests ray_tracing_settings_tests resource_tracking_tests resource_materialization_tests -j8
ctest --test-dir _Build/rt -R '^ray_tracing_settings$|^resource_(tracking|materialization)$|^shader_(cfg|resource_control|ray_tracing_ir|mesh_input_assembly)$' --output-on-failure
```

Before: full shader_cfg failed the pixel source identity check. After: 7/7 selected
tests pass, including full shader_cfg. RT settings tests verify deferred enable and
disable, persistence in an isolated temporary directory and external file reload.
Build logs: _Build/rt/settings-build.log, settings-launcher-rebuild.log,
settings-final-build.log. Test log: _Build/rt/settings-regression.log.

For comparable RT startup checks use the exact arguments recorded in
_Build/rt/probes/settings-cache-rt/result.json, with the RT-preserving patch,
fresh working directory and 45-second bound. The probe used emulator SHA256
7BA0CA3B83026258F7942B69AAC08374F136E916CBE82224D57D4284C62511C9;
subsequent changes affect the settings presentation and Quality selection.
Benchmark frame times only after a representative game scene is reachable.

## 12. ACCEPTANCE CRITERIA

Launcher and emulator build; all seven selected tests pass; auto optimization
is explicitly false on launcher launches; pending RT does not alter active RT;
changed executable code cannot retain its earlier shader identity. These pass.
Playable RT and performance acceptance criteria remain outstanding.

## 13. REGRESSION CHECK

git diff --check passes. Deployed emulator --help exits successfully. Updated
executables and Qt runtime are in bin; replaced files are backed up under
_Build/rt/backups/bin-settings-20261007-094846. No save data was overwritten.
Deployment hashes are recorded in _Build/rt/settings-deployment.json.
Other games, UI interactions, RT visuals and extended gameplay still require checks.

## 14. NEXT BOTTLENECK

Resolve the GPU-dependent scalar descriptor chain without speculative host reads,
null-to-zero substitution or skipped shaders. The comparison with jetsku/main
(58a7f743) identified a scalar indirect-buffer GPU lowering path worth testing;
its low-address read-as-zero rule and unresolved-shader skipping were not ported.
