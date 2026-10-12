# PS5 Comparison Protocol

This protocol turns a PS5 comparison into repeatable evidence. It is intended for
games and hardware the operator lawfully owns. Console media, game data, keys,
firmware, executables, saves, and proprietary captures remain private.

## 1. Identify the comparison

Create one record per title version, route, host, and attempt. Record:

- title ID, title name, content version, executable SHA-256, and region;
- KytyPS5 commit and whether the working tree is clean;
- Windows or Linux version, kernel where applicable, CPU, RAM, GPU, driver, and
  Vulkan device/driver versions;
- PS5 system software version and console model, without dumping firmware;
- display resolution, refresh rate, HDR, VRR, 120 Hz mode, color range, and
  output device;
- game graphics mode, language, accessibility options, controller connection,
  audio output mode, and every non-default emulator option.

Change one variable at a time. If a setting changes, start a new comparison
record rather than editing the old result.

## 2. Define a deterministic route

Prefer the earliest route that reaches the subsystem under test. Describe it as
numbered controller actions from a named starting state, for example: cold boot,
wait for the first legal screen, press Cross, select the first save slot, and
stop when a specified landmark is fully visible.

For each route, record:

1. starting save or no-save state and whether network access is disabled;
2. cold shader/pipeline cache or warmed cache;
3. every input, hold duration, menu selection, and wait condition;
4. the checkpoint that proves the route succeeded;
5. observation duration after the checkpoint;
6. expected transitions, audio cues, and user-visible state.

Do not use online matchmaking, leaderboards, anti-cheat systems, or production
services as compatibility fixtures. Prefer offline and local-only routes.

## 3. Lock the environment

Before both console and PC runs:

- use the same game version, language, graphics mode, display resolution, HDR,
  and audio channel layout where the platforms expose equivalent settings;
- disable overlays, notifications, downloads, recording features not used for
  evidence, and unrelated background workloads;
- use the same display input and display processing mode;
- reboot after driver, OS, game, or emulator updates;
- preserve the emulator pipeline cache state and label it `cold` or `warm`;
- synchronize the camera or capture clock used for side-by-side timing.

Run the PS5 route first when practical, then Windows and Linux. Do not tune the
route after seeing a PC-only failure; version the route instead.

## 4. Capture each attempt

Start capture before launch and retain at least 10 seconds after the route
checkpoint or until the failure is stable. A normal attempt records:

- launch-to-first-frame, launch-to-menu, and route-completion time;
- frame-time samples for at least 60 seconds of comparable gameplay, or the
  whole scene when shorter;
- screenshots at the same checkpoint and camera position;
- video with audible game output and visible input timing when latency or sync
  is under review;
- Vulkan validation output, KytyPS5 diagnostics, stdout, and stderr on PC;
- crashes, stalls, missing imports, rendering defects, audio defects, input
  defects, save/reload behavior, and recovery behavior.

Repeat a claimed success three times: one cold-cache attempt and two warm-cache
attempts. A failure needs one deterministic reproduction plus one confirming
attempt unless it risks data loss.

## 5. Evaluate allowed variance

Console-equivalent does not require identical video encoding or pixel hashes.
It requires equivalent game state and output after accounting for documented
platform differences. Unless a title-specific record defines a stricter rule:

- route results, geometry, UI, animation state, effects, and save state must be
  functionally identical; persistent missing or corrupt output is not allowed;
- the sustained frame-rate target must match, with no recurring emulator hitch
  above twice the target frame time during the measured steady-state route;
- median frame time may differ by at most 3%, and the 1% low by at most 5%, when
  both systems target the same frame rate and are not GPU-limited differently;
- visible input response may trail the PS5 by no more than one displayed frame
  after capture latency is calibrated;
- audio may not crackle, drop, repeat, or drift visibly from video; measured
  audio/video offset should stay within 20 ms over a 10-minute observation;
- launch and route timing may vary by 5% or one second, whichever is larger,
  provided no guest-visible transition is missing;
- zero Vulkan validation errors, emulator fatal errors, or host exceptions are
  allowed for a console-equivalent claim.

HDR tone mapping, display scaling, dynamic resolution, randomized particles,
network content, and nondeterministic NPC behavior must be listed as variance
sources. If they prevent a fair comparison, choose a different checkpoint.

## 6. Name and retain evidence

Use this private filename pattern:

```text
<title-id>_<content-version>_<ps5|windows|linux>_<route>_<attempt>_<utc-timestamp>.<ext>
```

Keep original media and full logs outside Git. A committed observation may
contain the file SHA-256, duration, numeric measurements, non-copyrightable
notes, and a short defect description. It must not contain absolute local paths
or reproduced game content.

## 7. Award a compatibility rung

Assign the highest rung whose entire contract has direct evidence. A completed
process alone never proves rendering, interaction, gameplay, or correctness.
`console-equivalent` requires a complete playthrough and save/reload coverage on
the declared game version plus repeatable Windows and Linux results. Record
`unknown` whenever evidence is incomplete.
