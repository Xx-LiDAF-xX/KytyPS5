# KYTYPS5 PERFORMANCE ANALYSIS

October 7, 2026. User reports a USB DualSense cannot perform L2/R2 frog-glove
punches at the gold door in Astro Bot's Aztec Trail. Native-RT profile and
controller gameplay changes are local; no commits or pushes.

## 1. CURRENT PERFORMANCE

The newly built Astro Bot native-RT profile still exits 321 after 17.247 seconds
on an invalid eight-byte read at 0x30. No gameplay FPS or RT speedup claim.
The controller implementation builds and its focused regression suite passes.
The exact frog-glove interaction has not been tested interactively.

## 2. BOTTLENECK CLASSIFICATION

RT: scalar pointer/resource descriptor planning remains the startup blocker.
Controller: a missing guest trigger-state implementation is a likely gameplay
blocker distinct from physical rumble and trigger-resistance output.

## 3. EVIDENCE

The installed emulator and previous supplied-patch test results shared SHA256
42DE1D62877C76FFD9F2B48C219F58FBB1BE52E4DF7042A3446BE9DEED62A72D,
confirming those tests included the earlier changes and touchpad removal.
scePadGetTriggerEffectState nevertheless returned zero for both triggers.
The [Jetsku fork's adaptive-trigger fix](https://github.com/Jetsku/KytyPS5/commit/a6d70665df26c1ea1d0e2256b37e474b9a53b66e)
addresses the same class of Astro Bot L2/R2 gameplay issue. Commands' layout and
mode ranges were checked against the existing code and the trigger header in
[Valve's Proton source](https://github.com/ValveSoftware/Proton/blob/proton_11.0/lsteamclient/steamworks_sdk_155/isteamdualsense.h).

The C:\ROMS executables match the source hashes in the supplied patches:
Playroom 8572CC581114DD71C75058554F5C26932D37E194F0B99D460EC661FFE15F0A41;
Astro Bot 3F6873C9AA0262B3E5513201810A4C40E0DD5EE910567A3A06FCC412275A1E16.

## 4. ROOT CAUSE

Returning an invariant zero gives gameplay no feedback, weapon or vibration
state transition to act on. Trigger effects were also resent when unchanged;
delivery failures were ignored for caching. The RT descriptor fault is separate.

## 5. PROPOSED FIX

Derive guest trigger state from each accepted effect and trigger travel. Preserve
invalid-command rejection. Make digital trigger presses supply full analog travel
without promoting partial analog presses. Cache only successfully delivered host
outputs and retry failed sides independently. Keep RT/GI bypasses disabled in a
separate experimental profile rather than claiming an unsupported game-code fix.

## 6. FILES/FUNCTIONS TO MODIFY

controller.cpp/.h: TriggerTravel, TriggerEffectState, PadGetTriggerEffectState,
GameController::GetTriggerEffectState, SetTriggerEffect and SendTriggerEffect.
libPad.cpp delegates its registered API to the controller implementation.
ControllerSettingsTests.cpp exercises production code with deterministic host I/O.
Profiles: patches/rt-native/PPSA01325.json and PPSA21567.json.
Launcher script: tools/run-native-rt.ps1.

## 7. PATCH

State logic adapts a6d70665, bbe1f24b and 6a8a89ee from the fork, without its
accepted-state update for rejected commands. Masked requests preserve the other
trigger. Commands and input determine guest state independently of host intensity.
Position-zero feedback/vibration requires an actual press, avoiding spontaneous
actions at rest. Pad switching clears state and delivery caches. SDL joystick
locking protects lookup and submission during hotplug.

All supplied renderer/GI bypass mods are disabled in the new profiles. They are
copied to Desktop\_Patches\RT-native-trigger-fix. The launcher script verifies the
game hash, explicitly enables RT and disables auto optimization, speculative
draws and relaxed readback. It uses the installed emulator's save directory by
default and supports a separate RunDirectory. Game files remain unmodified.

## 8. WHY THIS SHOULD HELP

Games now receive trigger-state transitions rather than permanent zero. Duplicate
successful output commands avoid redundant host reports. Failed delivery does not
make a later request look successfully cached. Unit tests establish these behaviors;
they do not establish that Aztec Trail is playable or that native RT is fixed.

## 9. CORRECTNESS RISKS

SDL does not expose the actual trigger motor-state report. The state calculation
is an approximation based on accepted commands and input travel; the eight-unit
press threshold follows the fork's rest-noise correction. USB hardware feel and
frog-glove gameplay remain unverified. JSON alone cannot repair emulator APIs
or shader resource planning. Do not label these profiles crash-free or optimized RT.

## 10. VENDOR IMPACT

NVIDIA RTX 4060 Ti: native-RT startup failure reproduced. AMD and Intel: not run.
Controller unit tests cover mocked output, not real USB/Bluetooth transmission.
Windows Release / clang-cl verified; other operating systems not built.

## 11. TEST PROCEDURE

```powershell
cmake --build _Build/rt --target kyty_emulator controller_settings_tests -j8
ctest --test-dir _Build/rt -R '^controller_settings$|^ray_tracing_settings$|^resource_(tracking|materialization)$|^shader_(cfg|resource_control|ray_tracing_ir|mesh_input_assembly)$' --output-on-failure
```

8/8 selected tests pass. New checks cover all supported modes, weapon thresholds,
feedback/vibration at rest, masked updates, invalid-command state preservation,
mute independence, digital and partial analog travel, disconnect, duplicate
delivery suppression and independent failed-send retry. Script parsing succeeds.

Latest native-RT run: the path in _Build/rt/latest-native-rt-trigger-probe.txt,
with exact arguments, stdout/stderr and result.json. A separate working directory
was used and a 45-second bound set; the emulator failed naturally before it.
Build/test logs: rt-trigger-state-build.log and rt-trigger-regression.log in _Build/rt.

## 12. ACCEPTANCE CRITERIA

Controller build and automated semantics criteria pass. Real USB frog-glove actions,
stable full-RT startup and measured rendering optimization criteria remain outstanding.

## 13. REGRESSION CHECK

All eight selected suites pass; git diff --check passes. Updated bin executable
matches the newly built/tested binary: SHA256
AEA35199A6FE50D42040D2CA30E44E3BAFEB87E9FB9A3408E58BF3B56FADCA61.
Deployment record: _Build/rt/trigger-gameplay-deployment.json. Previous executable
backed up to _Build/rt/backups/trigger-gameplay-20261007-110027.

## 14. NEXT BOTTLENECK

Verify frog-glove gameplay on the user's USB DualSense when the stage is reachable.
Resolve native RT scalar descriptor planning without null-to-zero or shader skipping.
The original GI-bypass patch path also has the separately reproduced vertex/pixel
interface mismatch; fixing trigger input does not resolve either rendering blocker.
