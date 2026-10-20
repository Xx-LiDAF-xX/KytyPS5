# Engine-Aware Optimization Subsystem Specification

**Date:** 2026-10-20  
**Target:** KytyPS5 (Jetsku int19.4+ baseline)  
**Status:** Approved for Implementation  

## 1. Executive Summary

KytyPS5 runs diverse PlayStation 5 titles that utilize dramatically different engine paradigms—ranging from Epic Games' compute-heavy Nanite/Lumen in Unreal Engine 5 to Guerrilla's custom DCC/depth aliasing in Decima, Insomniac's BVH ray traversal in Marvel's Spider-Man 2, Team ASOBI's custom tiled lighting with `IMAGE_BVH_INTERSECT_RAY` in Astro Bot, and Bluepoint's runtime texture-LOD feedback loops in Demon's Souls.

Rather than relying on a single one-size-fits-all preset or requiring players to manually guess environment flags (`KYTY_*`), this subsystem provides **automatic engine detection and dynamic per-engine optimization profiles** directly inside KytyPS5's C++ core.

## 2. Engine Matrix & Technical Profiles

The system explicitly supports and optimizes for 17 target engine families:

| # | Engine Family | Representative Titles | Primary Bottlenecks & Characteristics | Key Optimization Flags |
|---|---|---|---|---|
| 1 | **Unreal Engine 5** | *Fortnite, Silent Hill 2, Lords of the Fallen, PPSA29340* | Nanite compute rasterization, Lumen software/hardware BVH traversal, massive BDA buffer writes, virtual shadow maps | `KYTY_BDA_WRITES=1`, `KYTY_BDA_INCREMENTAL_SYNC=1`, `KYTY_RT_SOFTWARE=1`, `KYTY_RT_NODE_BUDGET=16384`, `KYTY_PIPELINE_PREFETCH=1`, `KYTY_DRAW_RUN=1` |
| 2 | **Decima Engine** | *Horizon Forbidden West, Death Stranding 1 & 2* | Async compute culling, tile lighting, dense texture streaming, DCC depth/color target aliasing | `KYTY_DCC_GPU=1`, `KYTY_DCC_GPU_REFRESH=1`, `KYTY_TEXTURE_SPARSE_RESIDENCY=1`, `KYTY_DIRTY_IMAGE_QUERY=direct`, `KYTY_FAST_BACKING_TRANSFERS=1` |
| 3 | **Insomniac Engine** | *Marvel's Spider-Man 2, Ratchet & Clank: Rift Apart* | BVH ray traversal, mesh shaders, high-bandwidth asset decompression, dynamic resolution tiers | `KYTY_RT_SOFTWARE=auto`, `KYTY_RT_NODE_BUDGET=8192`, `KYTY_MESH_RESTART=1`, `KYTY_PIPELINE_FAST_FIRST=1`, `KYTY_DEFER_PROTECTS=all` |
| 4 | **Naughty Dog Engine** | *The Last of Us Part I & II Remastered* | Async compute particle/volumetric physics, skeletal deformation compute, page table tracking | `KYTY_CP_COMMIT=all`, `KYTY_CP_SEQ=1`, `KYTY_CP_SEQ_PREFETCH=1`, `KYTY_SUBMISSION_COALESCE=1`, `KYTY_EOP_TIMESTAMPS=gpu` |
| 5 | **Sucker Punch Engine** | *Ghost of Tsushima Director's Cut* | Dense foliage instancing, dynamic wind compute shaders, depth-of-field compute passes | `KYTY_PIPELINE_PREWARM_COMPUTE=1`, `KYTY_SHADER_PRECOMPILE=1`, `KYTY_DRAW_PREP_BINDINGS=1`, `KYTY_RECORDER_DESCRIPTOR_SETS=1` |
| 6 | **Santa Monica Studio Engine** | *God of War Ragnarök* | Clustered lighting compute, dual depth/stencil synchronization, hair/cloth simulation | `KYTY_META_CLEAR_MEMO=1`, `KYTY_PENDING_REFRESH_US=150`, `KYTY_BINDING_MEMO_TOUCH=1`, `KYTY_PIPELINE_PREFETCH=1` |
| 7 | **Polyphony Digital Engine** | *Gran Turismo 7* | High-precision FP64/FP32 vertex processing, RT audio/reflections, HDR presentation pacing | `KYTY_RT_SOFTWARE=auto`, `KYTY_CB_SHADER_MASK_EXPORTS=1`, `KYTY_TEXTURE_CLEAN_PROOFS=1`, `KYTY_EOP_TIMESTAMPS=gpu` |
| 8 | **Team ASOBI Engine** | *Astro's Playroom, Astro Bot (PPSA21567)* | Tiled compute lighting with `IMAGE_BVH_INTERSECT_RAY`, D32 depth memory reused as RG16F DCC target, adaptive triggers | `KYTY_RT_SOFTWARE=1`, `KYTY_RT_NODE_BUDGET=8192`, `KYTY_DCC_GPU=1`, `KYTY_DCC_GPU_REFRESH=1`, `KYTY_NATIVE_IMAGE_POOL=1` |
| 9 | **Bluepoint Engine** | *Demon's Souls Remake* | Runtime texture-LOD feedback loops (`OpPhi` control flow), clustered forward lighting, tessellation | `KYTY_LOD_STATS_MODE=gpu`, `KYTY_PIPELINE_FAST_FIRST=1`, `KYTY_BINDING_LEAN_MISS=1`, `KYTY_DIRTY_IMAGE_QUERY=direct` |
| 10 | **San Diego Studio Engine** | *MLB The Show* | Crowd instancing, high-frequency physics tick, continuous presentation pacing | `KYTY_DRAW_RUN=1`, `KYTY_CP_OWNED_COUNTERS=1`, `KYTY_DRAW_PREP_CLEAN_SINCE=1` |
| 11 | **BubbleBath Engine** | *Dreams (Media Molecule)* | Signed Distance Field (SDF) point cloud splatting, compute volume allocation, procedural synthesis | `KYTY_BDA_PAGETABLE_SPARSE=1`, `KYTY_PIPELINE_PREWARM_COMPUTE=1`, `KYTY_TILER_SCRATCH_POOL_IDLE_MS=2000` |
| 12 | **Tiger Engine / Destiny** | *Destiny 2, Marathon* | Multi-threaded command stream replay, compute culling passes, network telemetry | `KYTY_SUBMISSION_COALESCE=1`, `KYTY_CP_RECORDER=1`, `KYTY_CP_SEQ=1`, `KYTY_RENDERER_BATCH=1` |
| 13 | **Valkyrie Engine** | *Action / RPG Engines* | Dynamic action particle bursts, skeletal deformation bursts, rapid draw submission bursts | `KYTY_DRAW_PREP_BINDINGS=1`, `KYTY_PUSH_SHADOW_FRESH_SKIP=1`, `KYTY_OCCLUSION_BATCH=1` |
| 14 | **PhyreEngine** | *Sony Internal Multiplatform Engine* | Classic GNM/GNMX standard pipeline translation, dynamic vertex buffers, static shader permutations | `KYTY_PROGRAM_CACHE=1`, `KYTY_IMAGE_LOOKUP=fast`, `KYTY_FAST_BACKING_TRANSFERS=1` |
| 15 | **Japan Studio Engine** | *Gravity Rush 2, The Last Guardian* | Physics ragdoll compute, cel-shading custom post-process passes, depth-stencil precision | `KYTY_CB_SHADER_MASK_EXPORTS=1`, `KYTY_PIPELINE_JOURNAL=1`, `KYTY_RAM_SMALL_UPLOAD_RING=1` |
| 16 | **PlayStation Studios Generic** | *Other 1st Party Titles* | PS5 hardware baseline, DualSense haptics, conservative safe RDNA2 pipeline settings | `KYTY_SUBMISSION_COALESCE=1`, `KYTY_PROGRAM_CACHE=1`, `KYTY_PIPELINE_PREFETCH=1` |
| 17 | **Rockstar Advanced Game Engine (RAGE)** | *Grand Theft Auto V, Red Dead Redemption* | Heavy world draw stream coalescing, ray-traced shadows & reflections, RPF archive streaming, dynamic virtual memory page residency | `KYTY_RT_SOFTWARE=auto`, `KYTY_RT_NODE_BUDGET=12288`, `KYTY_DRAW_RUN=1`, `KYTY_DRAW_RUN_ACQUIRE=1`, `KYTY_DRAW_RUN_PUSH=1`, `KYTY_DRAW_RUN_WRITES=1`, `KYTY_PIPELINE_PREFETCH=1`, `KYTY_PIPELINE_PREFETCH_PROGRAMS=1`, `KYTY_SUBMISSION_COALESCE=1`, `KYTY_CP_SEQ=1`, `KYTY_CP_SEQ_PREFETCH=1`, `KYTY_TEXTURE_SPARSE_RESIDENCY=1`, `KYTY_FAST_BACKING_TRANSFERS=1`, `KYTY_DCC_GPU=1`, `KYTY_DCC_GPU_REFRESH=1`, `KYTY_VRAM_GC_BUDGET=1` |

## 3. Detection Architecture

Detection occurs in `Common::EngineProfile::DetectAndProfile` during early boot in `Emulator::Run()`:
1. **File System Fingerprinting**:
   - `uecommandline.txt` or `/app0/engine` or `Engine/Binaries` -> `UnrealEngine5`
   - `core` or `ds2.bin` or Decima `.bin` manifests -> `Decima`
   - `toc` + `*.cache` -> `Insomniac`
   - `pak23.psarc` or Naughty Dog archives -> `NaughtyDog`
   - PhyreEngine signatures (`.phyre`, `.pfx`) -> `PhyreEngine`
   - Destiny packages (`packages/*.pkg`) -> `TigerEngine`
   - Rockstar package archives (`.rpf`) -> `Rage`
2. **Title ID & Metadata Matching**:
   - `PPSA21567` (Astro Bot) -> `TeamAsobi`
   - `PPSA01325` (Demon's Souls) -> `Bluepoint`
   - `PPSA01859` (Spider-Man) -> `Insomniac`
   - `PPSA01317` (Gran Turismo 7) -> `PolyphonyDigital`
   - `PPSA08329` (God of War Ragnarok) -> `SantaMonica`
   - `PPSA03209` (Ghost of Tsushima) -> `SuckerPunch`
   - `PPSA03420`, `PPSA03421`, `PPSA01757` or "Grand Theft Auto", "GTA", "Red Dead", "Rockstar" -> `Rage`
   - Known title ID prefix ranges and `sce_sys/param.json` fields (`TITLE`, `CONTENT_ID`).
3. **Environment & Override Policy**:
   - If the user explicitly configured an environment variable or launcher setting, explicit user values take precedence over automatic defaults (`std::getenv` check before applying).

## 4. Deliverables

1. Core implementation: `src/common/engineProfile.h` and `src/common/engineProfile.cpp`.
2. Startup hook: Integrated into `src/emulator.cpp`.
3. Presets directory: `tools/engine-presets/*.json` containing 17 dedicated profiles.
4. Unit tests: `tests/EngineProfileTests.cpp` integrated into `CMakeLists.txt`.
