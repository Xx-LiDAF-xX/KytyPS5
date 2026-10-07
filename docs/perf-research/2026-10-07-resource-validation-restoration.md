# KYTYPS5 PERFORMANCE ANALYSIS

October 7, 2026. Follow-up to the guarded-load and mesh report. Base: ef6a730a.
Windows Release / clang-cl; Ryzen 7 5800X; RTX 4060 Ti. No dependencies needed
installation: missing implementation and test code was recoverable from local Git history.
Changes are local; no commit or push performed by this task.

Subsequent update: [settings and shader identity](2026-10-07-settings-and-shader-identity.md)
fixes the pixel cache identity failure described below; the full shader_cfg suite now passes.

## 1. CURRENT PERFORMANCE

Astro Bot's RT-enabled startup still exits 321 after 17.959 seconds at an invalid
eight-byte guest read at 0x30. Stable gameplay and frame-time measurements remain unavailable.

## 2. BOTTLENECK CLASSIFICATION

The remaining game blocker is descriptor-dependent scalar-pointer planning. This
iteration also repaired concrete resource validation and specialization defects.

## 3. EVIDENCE

Previously failing resource_materialization and resource_tracking suites now pass.
Wave32 ballot projection was missing although its regressions and wave-size interface
remained. Four comparison case definitions were missing although their runner entries
remained. Local commits contained both implementations. The restored comparisons
execute successfully on the NVIDIA GPU, including wave32 and wave64.

## 4. ROOT CAUSE

GetHandle accepted unsupported GPU-varying image/sampler words after validation failed.
Finite candidate image lists unnecessarily required a separate table descriptor.
Specialization read capture collected ranges in scratch storage but never copied them
into the snapshot used by the renderer's write-overlap proof. A capture wrapper could
call a missing strict-memory reader. Separately, verification doubled callback counts,
and dead fixture consumers allowed DCE/select simplification to invalidate test assumptions.

## 5. PROPOSED FIX

Require image/sampler sources to satisfy their supported planning path. Treat finite
candidate lists independently of guest table enumeration. Publish exact captured ranges
for all indirect image specializations. Preserve missing strict-reader behavior while
wrapping ordinary mapped reads for capture. Restore wave32 projection with current
FoldInstruction/PossibleValues interfaces; preserve wave64 behavior and raw mask consumers.

## 6. FILES/FUNCTIONS TO MODIFY

ResourceTracking.cpp / GetHandle; ResourceMaterialization.cpp / ExtractResourcePlan,
ObservedRuntime, MaterializeIndirectImage, Materialize; ConstantPropagation.cpp /
LaneMaskProjection and ConstantPropagationPass. Tests: ResourceTrackingTests.cpp,
ResourceMaterializationTests.cpp, shaderCfgTests.cpp, ShaderRecompilerComputeTests.cpp,
and RestoredVectorCompareCases.inc.

## 7. PATCH

Implementation is in the working tree. LaneMaskProjection was recovered from 35759e48;
comparison cases from 581dad7d and 5b826c6e. Strict-reader count checks run each walker
independently. Finite candidate refresh checks compare compiled and reference snapshots.
Guarded-read tests keep descriptor consumers live and distinguish failed batched-read
retries from new reads after disabling a feature. No RT bypass or null-to-zero fix was added.

## 8. WHY THIS SHOULD HELP

Unsupported descriptors fail explicitly. Finite image refresh can run without an unused
table descriptor. The renderer receives specialization ranges required to reject overlapping
GPU writes. Wave32 ballot bit extraction recovers the lane predicate and exposes feature
guards. These results do not establish a gameplay speedup or fix Astro's remaining pointer chain.

## 9. CORRECTNESS RISKS

Specialization range capture has added cost and disables block reads when exact ranges
are required. Restored projection applies only to wave32 and a restricted mask graph.
Cross-platform builds, long gameplay and image comparisons remain unverified. The full
shader_cfg suite is still not green; it reaches the pixel program source/static identity check.

## 10. VENDOR IMPACT

NVIDIA: targeted compute checks passed on the local RTX 4060 Ti. AMD and Intel: not run.
No vendor-specific workaround. Windows builds verified; Linux and macOS not built.

## 11. TEST PROCEDURE

Build kyty_emulator, shader_cfg_tests, resource_tracking_tests,
resource_materialization_tests and shader_recompiler_compute_tests in _Build/rt.

```powershell
ctest --test-dir _Build/rt -R '^resource_(tracking|materialization)$|^shader_(resource_control|ray_tracing_ir|mesh_input_assembly)$' --output-on-failure
_Build/rt/shader_recompiler_compute_tests.exe --restored-compare-only
_Build/rt/shader_recompiler_compute_tests.exe --cmp-eq-f64-only
_Build/rt/shader_recompiler_compute_tests.exe --fp32-compare-only
_Build/rt/shader_recompiler_compute_tests.exe --indirect-image-only
```

Logs: _Build/rt/restored-comparisons-gpu.log, restored-fp64-gpu.log,
restored-fp32-gpu.log, restored-indirect-image-gpu.log and restored-projection-tests.log.
The indirect-image invocation completed its host checks and cube-gradient GPU case;
it is not proof of every dynamic-image shader path.

Live probe: _Build/rt/probes/restored-guards-rt/result.json, stdout.log and stderr.log.
Executable SHA256: E95D3B193AF971A813B3F2CAA180DDC606553EF4A1DBF0A579B04E875E349CC2.
Explicit RT-preserving game patch, Quality, validation on, RT on, relaxed readback and
speculative draws off, pipeline libraries off. Later edits affect focused test runners
only; that live executable includes the engine changes in this report.

## 12. ACCEPTANCE CRITERIA

Both resource suites, wave32 mask projection and existing RT/mesh checks pass. Recovered
comparison GPU cases pass in both wave sizes. Finite refresh remains allocation-stable
after warmup and agrees with the reference walker. These criteria currently pass.
Game stability, visually correct RT and performance criteria remain outstanding.

## 13. REGRESSION CHECK

Focused CTest: 5/5 pass. All four GPU invocations above exit 0. All five requested build
targets compile. git diff --check passes. The full shader_cfg runner stops at
"pixel program matching did not preserve an identical source/static identity"; no complete
suite pass is claimed. No game passed a representative gameplay benchmark in this iteration.

## 14. NEXT BOTTLENECK

Resolve Astro's dynamic buffer descriptor/scalar pointer execution without CPU speculation.
Current GPU-selected raw-buffer support is limited to vector DWORD x2/x3/x4 reads and
does not cover the full scalar-buffer chain. Investigate native reachability and dynamic
scalar-buffer lowering with GPU regression coverage. Separately investigate the remaining
pixel cache identity failure. Measure frame pacing only after gameplay becomes stable.
