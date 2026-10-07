# KYTYPS5 PERFORMANCE ANALYSIS

## 1. CURRENT PERFORMANCE

The initial RT-off bounded startup run produced later five-second intervals near
37 frames/s and 25.7 ms of aggregate GPU work per frame. Input-trace suppression
alone has not established a repeatable FPS gain. Per-zone GPU profiling identifies
draw work as the main cost in this captured startup workload.

## 2. BOTTLENECK CLASSIFICATION

GPU draw cost is the measured dominant category. Repetitive input logging is
avoidable CPU/file overhead, but is not established as the FPS limiter. A separate
controller speaker fallback gain defect was exposed by the haptics regression.

## 3. EVIDENCE

Evidence directory: `_Build/rt/probes/input-log-ab-20261007-151819/`.
Baseline and candidate use RT off, timestamp scale 100, the same initial pipeline
cache files, and matching asynchronous options. Both are stopped deliberately
after approximately 61 seconds. Their final workloads differ: baseline intervals
reach 37.4 frames/s with 25.68 ms GPU work/frame, while the candidate reaches
52.4 frames/s with 17.60 ms GPU work/frame. This is not a controlled same-scene
speedup: the draw/dispatch mix also changes.

The separate GPU-zone run reports 26.56 ms/frame total: game draws 21.18 ms/frame
and game dispatches 3.40 ms/frame. Shader keys `7904c41b5a13e609`,
`ef31694ed8d87754`, and `9d7e6bca13da68ed` account for 4.244, 3.602, and
2.953 ms/frame respectively. Several draw render areas are approximately eight
million pixels (width times height times layers). A small host window does not
prove that the guest renders at that window's resolution.

The original log's repeated raw handle lines were not all established as
controller calls. Other polling APIs retain their own logging. Coarse PM4 timing
attributed much GPU time to dispatch submissions, whereas per-zone timestamps
place most measured cost in draws; use the finer attribution for optimization.

## 4. ROOT CAUSE

Hot input polling and movement paths formatted and synchronously submitted trace
messages even when detailed tracing was unnecessary. The measured GPU draw cost
needs shader-level analysis before a correctness-preserving optimization.

`Audio::PrepareOutputBuffer` accepted a controller gain argument but ignored it
when deciding to bypass conversion and when scaling samples. A muted controller
speaker could consequently play audible fallback audio after the controller was
unplugged. Existing production-path tests reproduced this defect.

## 5. PROPOSED FIX

Make high-frequency input traces opt-in through `KYTY_TRACE_INPUT=1`. Preserve
input processing and errors. Apply controller gain to the existing output scaling
path. Add a repeatable performance launcher writing dated logs into the user's
designated folder, with optional GPU zones and shader capture.

## 6. FILES/FUNCTIONS TO MODIFY

`emulatorConfig`: cached `InputTraceEnabled` flag.
`controller.cpp`, `libPad.cpp`, and `window.cpp`: selected polling/movement traces.
`audio.cpp`: `Audio::PrepareOutputBuffer` gain multiplication.
`tools/run-performance.ps1`: profiling launch and build metadata.

## 7. PATCH

Skip trace formatting and logger acquisition on the selected input hot paths by
default; no event dispatch or controller return values change. Read the environment
flag once per process. Include controller gain in the existing master scaling value
for float and integer PCM and channel mixing. The existing fast path remains when
the combined gain is one. Fatal and device lifecycle logs remain available.

## 8. WHY THIS SHOULD HELP

Avoid unnecessary work on frequent polls without changing gameplay. Preserve
speaker mute/volume when device routing falls back. Per-zone profiling supplies
specific shader keys for the next renderer optimization rather than inferring
RT state or bottlenecks from FPS alone.

## 9. CORRECTNESS RISKS

Input tracing is opt-in and must be enabled before process startup. Profiling and
shader capture add overhead; captured timings must not be presented as an
uninstrumented benchmark. Controller speaker/vibration settings must affect only
their selected output types; ordinary game audio uses a gain of one.
RT/GI bypass behavior is unchanged and the measured probes use RT off.

## 10. VENDOR IMPACT

Measured on RTX 4060 Ti and Ryzen 7 5800X. Logging and PCM scaling are portable;
other GPU vendors and operating systems are not tested in this change.

## 11. TEST PROCEDURE

Build the emulator and selected tests. Build the previously missing
`pad_haptics_tests` target, reproduce its muted fallback failure, apply the gain
correction, and run haptics, controller settings, RT settings, and shader RT IR
tests. Use bounded baseline/candidate/GPU-zone runs with separate save directories.
Parse-check the performance PowerShell script.

For interactive measurement, run `tools/run-performance.ps1 -DisableRayTracing
-GpuZones`, then play the same scene before comparing separate launches. The
script writes build hash and launch arguments beside its dated log, keeps existing
saves in the emulator directory, and does not change the game dump.

## 12. ACCEPTANCE CRITERIA

Selected regressions pass; fallback mute and gain are preserved. No crash is
recorded in the bounded runs. Publish no claim of a logging-related FPS improvement
or sustained 60 FPS without a controlled same-scene comparison.

## 13. REGRESSION CHECK

All four selected tests pass after the audio fix. The complete haptics test's
existing USB, Bluetooth, integer/float, mute, gain, routing failure, and unplug
cases run through the production audio implementation. Build and script parsing
passed. Prior RT settings and resource corrections are retained.

## 14. NEXT BOTTLENECK

Capture and analyze the expensive draw shaders identified above, then evaluate
their generated SPIR-V and image/sampling operations. The separate semaphore
crash and strict Vulkan shader-interface mismatch remain unresolved. Preserve
sync, sampling, visibility, and pixel output while optimizing.

Native code and generated SPIR-V for all three target hashes were captured under
`draw-shader-capture/_Shaders/` in the evidence directory. Five SPIR-V variants
passed `spirv-val --target-env vulkan1.3`. The `9d7e6bca13da68ed` variants contain
30 static image sampling/gather instructions and four loops; `7904c41b5a13e609`
contains eight such instructions and no loops, while `ef31694ed8d87754` contains
nine and no loops. Static instruction counts do not establish dynamic cost or
justify dropping samples. Keep these artifacts for shader optimization and image
equivalence checks. The first capture attempt lacked `--graphics-debug-dump` and
produced no shader binaries; the completed cold capture enables that flag and is
not used for FPS comparison.

Final build deployment hashes and backups are recorded in
`_Build/rt/input-trace-deployment.json`. Local repository `bin` and Desktop
`Kytytest` receive the updated binaries; no release upload or push is performed.
