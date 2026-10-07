# KYTYPS5 PERFORMANCE ANALYSIS

## 1. CURRENT PERFORMANCE

This is a correctness repair for RT-enabled startup, not a 60 FPS claim. The Desktop
`Kytytest/_kyty.txt` confirms the RT setting was enabled and reproduces the same
invalid eight-byte read at `0x30` as the isolated test installation.

## 2. BOTTLENECK CLASSIFICATION

Shader resource planning speculates a guarded pointer chain on the CPU. After this
repair, a longer Astro Bot test reaches a separate vertex/fragment interface error.

## 3. EVIDENCE

Final executable SHA256: `9A348D8C7D9DE4F31FC4AC5EF47166938C2C23556E05DD234640BA24DC077C53`.
The final repeat used `probes/gpu-pointer-final-20261007-114526`: Astro Bot reached
the later interface error after 39.632 seconds; Playroom ran for 61.581 seconds and
was stopped deliberately, with no fatal error recorded. These are unattended
startup tests, not confirmed interactive gameplay or visual RT correctness.

Captured Astro Bot CS `78af8e269b528b5c`: native PC `1f60` loads a null pointer;
PC `1f90` reads the descriptor at that pointer plus `0x30`. The chain follows a
GPU-controlled EXEC branch. Both the compiled and reference CPU walkers previously
followed this chain. Offline replay after the repair reports zero low-address reads;
it remains incomplete because four ordinary/clean reads were not present in the
original capture. Missing data is never synthesized.

Tests and build logs are in `_Build/rt/gpu-pointer-*.log`. Probe directories and
exact executable hashes are recorded by `latest-gpu-pointer-final-probes.txt` and
the per-title `result.json` files.

## 4. ROOT CAUSE

Excluding optional scalar reads was insufficient: buffer descriptor collection
recursively pulled the same execution-only reads back into CPU planning. Dynamic
scalar buffer descriptors also lacked GPU lowering. Structured merge IDs must be
looked up by CFG ID, rather than indexing the block array, because translation
prepends a prologue.

## 5. PROPOSED FIX

Keep guarded scalar pointer dependencies on the GPU. Execute scalar buffer loads
through their runtime descriptor and the existing BDA page table. Preserve CPU
planning for direct descriptor tables and uniform predicates.

## 6. FILES/FUNCTIONS TO MODIFY

`ResourceTracking.cpp`: `GpuControlledBlocks`, `PlanScalarReads`, `CollectScalarRead`,
and buffer collection. `SrtWalker.cpp`: raw-read eligibility. `ShaderIR.h` and
`Program.cpp`: memory metadata and validation. `spirvEmitterMemory.cpp`:
`EmitReadConstBuffer`. Tests cover guarded chains and GPU-selected scalar descriptors.

## 7. PATCH

`gpu_execution_only` prevents CPU evaluation of guarded pointer loads. This repair
targets one/two-word pointer producers feeding address handles; it is not a general
GPU image-descriptor implementation. Unsupported accesses still fail explicitly.
Scalar indirect loads preserve separate alignment of the immediate and SOFFSET,
use 64-bit descriptor sizes and offsets, and return the defined buffer out-of-bounds
value. Invalid guest memory still uses the existing GPU fault machinery.

## 8. WHY THIS SHOULD HELP

The guest GPU predicate can skip a dormant null-pointer chain before it executes.
The emulator no longer executes that chain eagerly during CPU resource refresh.

## 9. CORRECTNESS RISKS

Dynamic scalar buffer bounds must match the existing scalar path. Tests exercise
zero stride, structured descriptors, empty buffers, partial final words, dynamic
selection, and unaligned offsets. Visual RT fidelity and later gameplay are not
established by startup tests.

## 10. VENDOR IMPACT

GPU testing uses the local RTX 4060 Ti. The implementation uses Vulkan compute and
BDA, not a new NVIDIA hardware ray tracing pipeline. AMD/Intel were not tested.

## 11. TEST PROCEDURE

Build the emulator and shader/resource test targets. Run the eight selected CTest
regressions, then `shader_recompiler_compute_tests.exe --gpu-scalar-buffer-only`
with no game process running: running both concurrently can exhaust Windows commit
capacity. Run both extracted games with RT on, the native RT profile, automatic
optimization off, timestamps at 100%, and shader/Vulkan validation enabled.

## 12. ACCEPTANCE CRITERIA

All selected regressions pass; GPU-selected scalar buffer results match expectations;
startup no longer fails at the original `0x30`/`0x28` dereference. A later failure is
reported separately, never treated as successful full-game validation.

## 13. REGRESSION CHECK

All eight selected CTest regressions pass. The GPU-selected scalar buffer test
passes on the RTX 4060 Ti, with nine descriptor/offset cases and 36 checked words.
An attempted concurrent GPU test exhausted Windows commit capacity; rerunning it
alone passed without changing memory settings.

Installed this executable in repository `bin` and Desktop `Kytytest`, with backups
under `_Build/rt/backups/rt-pointer-20261007-114644`. Exact paths and hashes are in
`_Build/rt/rt-pointer-deployment.json`. Desktop RT remains enabled, auto-optimization
remains disabled, and its legacy 125% timestamp scale was restored to 100%, with an
INI backup alongside the executable backup. The CMake-cached build banner still
says `1d3e48c-dirty`; the executable hashes identify the tested build accurately.

Uniform guarded descriptors remain CPU-planned. GPU-guarded chains produce no CPU
table reads and retain their live address loads. Existing controller and RT settings
checks are included. Built-in RT bypasses, shader skipping, memory guards, and Vulkan
validation are not altered to conceal a failure.

## 14. NEXT BOTTLENECK

Astro Bot's later pipeline validation error reports fragment input Location 1,
Component 0, `vec4 float32`, with no corresponding vertex output. Capture and link
the exact stage pair before changing interpolation/default-input semantics. Startup
drain telemetry also shows substantial guest readback waits; do not remove required
synchronization to improve the displayed frame rate.
