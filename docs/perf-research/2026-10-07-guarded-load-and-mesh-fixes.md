# KYTYPS5 PERFORMANCE ANALYSIS

October 7, 2026. Base commit: `78e776e979e5a1416d42605a202cb4b126f80496`.
Local changes only; no commit or push. Windows Release / clang-cl, Ryzen 7 5800X,
RTX 4060 Ti. This records two correctness fixes, not a validated FPS improvement.

## 1. CURRENT PERFORMANCE

Astro Bot (PPSA21567) still crashes before representative gameplay. The guarded-load
candidate exited 321 after 17.339 seconds, reading eight bytes at guest address 0x30.
The run used the extracted `C:\ROMS\PPSA21567`, an explicit RT-preserving patch
configuration, Quality, RT enabled, validation enabled, pipeline libraries disabled,
relaxed readback disabled and speculative draws disabled. No gameplay FPS or frame
time result is available. Time to failure is not a speedup metric.

## 2. BOTTLENECK CLASSIFICATION

Primary remaining blocker: CPU resource planning follows a scalar pointer chain
that contributes to a buffer descriptor. Separate confirmed bugs: optional planning
of execution-only scalar pointer loads behind GPU branches, and fast-launch mesh
vertex numbering when the host slices a large dispatch.

## 3. EVIDENCE

The pointer regression now uses varying FragCoord input. It requires no host reads,
three live scalar BDA loads, and valid SPIR-V. It failed before the change and passes
afterward. Removing planning globally broke native shared-exit descriptor selection;
that attempt was discarded.

The mesh fixture had case fields for fast launch and thread count but ignored them.
After repairing the fixture, group=1, slice base=1, lane=0, threads=32 produced vertex
11 instead of 12 with base vertex 11. Adding the slice offset fixes this case and the
fixture's other wave32/wave64, topology, indexing and slice cases.

## 4. ROOT CAUSE

PlanScalarReads collected physical-address dependencies and optional immediate
scalar loads without checking GPU branch ancestry. A syntactically CPU-evaluable
pointer value is not evidence that its load executes. Separately, the fast-launch
mesh entry used dispatch-local WorkgroupId while ordinary mesh input assembly
already incorporated the host's base workgroup offset.

## 5. PROPOSED FIX

Keep execution-only physical-address reads on the GPU below branches that cannot
be fully evaluated by the resource runtime. Preserve explicit descriptor dependency
planning. Conservatively propagate the restriction through joins and backedges.
For fast-launch meshes, use WorkgroupId.x plus draw parameter 6 as the global group.

## 6. FILES/FUNCTIONS TO MODIFY

- ResourceTracking.cpp: GpuControlledBlocks and PlanScalarReads.
- frontend/translate/Translate.cpp: mesh entry group computation.
- shaderCfgTests.cpp: varying guarded-pointer fixture, mesh fixture setup and focused runner.
- ResourceMaterializationTests.cpp: replace stale test invocation with the existing sampler
  variant test and register the existing finite-image refresh test.
- CMakeLists.txt: register shader_mesh_input_assembly.

## 7. PATCH

The source changes are in the working tree. GpuControlledBlocks validates complete
branch expressions and traverses affected successors once. It does not reinterpret
unknown predicates as false, synthesize null reads as zero or disable RT. Explicit
descriptor planning remains unchanged, which is why Astro's remaining chain still fails.

## 8. WHY THIS SHOULD HELP

Execution-only pointer chains retain their original GPU control flow instead of
being speculatively followed during CPU resource refresh. Fast-launch dispatch
slices now preserve the original draw's vertex numbering. Neither result establishes
an FPS benefit or correct ray-traced gameplay in Astro Bot.

## 9. CORRECTNESS RISKS

Conservative branch propagation can retain additional GPU loads beyond a join.
Actual descriptor dependencies still execute on the CPU and require a separate
architectural correction. Test emission validates SPIR-V, not rendered output.
The mesh fix requires long draws in games for visual verification. Full regression
testing is not green, as detailed below.

## 10. VENDOR IMPACT

NVIDIA: local build and Astro launch performed on RTX 4060 Ti. AMD and Intel: not run.
Both fixes are shared compiler logic; there is no vendor-specific workaround.
Linux and macOS have not been built for these changes.

## 11. TEST PROCEDURE

Build kyty_emulator and shader_cfg_tests in _Build/rt. Run:

```powershell
ctest --test-dir _Build/rt -R '^shader_(resource_control|ray_tracing_ir|mesh_input_assembly)$' --output-on-failure
```

Live guarded-load evidence: `_Build/rt/probes/guarded-planning-fix/result.json`, stdout.log
and stderr.log. That executable hash was
`B4A271BCFC2F311D9454800E74EBD3C3AC76662FE3F95FC471A1A1D56B402532`;
the subsequent mesh fix was built and tested separately, not included in that launch.
Captured replay: `_Build/rt/guarded-srt-replay-{stdout,stderr}.log`.
Replay remains incomplete: compiled missing=5 / low-address requests=2; reference
missing=4 / low-address requests=1. Unknown captured memory was not filled with zeros.

Before performance evaluation, obtain stable gameplay and compare identical scenes,
settings and cache states with tracing/validation disabled, repeated cold and warm
runs, CPU/GPU timings and actual frame intervals.

## 12. ACCEPTANCE CRITERIA

The guarded-pointer fixture must perform zero CPU reads and retain all three live
loads. Native shared-exit descriptor selection must stay correct. All mesh input
cases must preserve vertex IDs, instance IDs, wave counts and index addressing for
sliced and unsliced dispatches. Those focused criteria currently pass. Full-game
stability, visual correctness and performance acceptance remain outstanding.

## 13. REGRESSION CHECK

Focused CTest: 3/3 pass (resource control, RT IR, mesh input assembly). Emulator and
shader_cfg_tests build. The full shader_cfg runner now passes its previous mesh
blocker but stops at `wave32 ballot round trip lost its predicate`.
Additional resource targets build after repairing the stale invocation, but fail
strict provenance for GPU-dirty writable descriptors and SampleAdjust sampler
scratch validation. They are unresolved and were not weakened to pass.
The compute GPU test target cannot build because several registered comparison
case functions are missing; see `_Build/rt/guarded-planning-regression-build.log`.
No GPU compute regression pass is claimed. Other games, audio and prolonged runs
remain unverified. Final changes pass git diff --check.

## 14. NEXT BOTTLENECK

Astro's descriptor-dependent scalar chain must retain GPU execution semantics.
Investigate dynamic scalar-buffer descriptor lowering and complete native branch /
memory-visibility evidence; do not suppress the invalid pointer read. Repair the
remaining compiler/resource validation regressions before performance tuning.
