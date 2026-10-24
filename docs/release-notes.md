# KytyPS5 v1.1.1 — Diagnostics Privacy, Tessellation Branching & Engine Presets

KytyPS5 v1.1.1 by Xx-LiDAF-xX brings host directory path sanitization across all diagnostics and assertion reports, compile-time macro prefix remapping, relaxed tessellation recompiler branch handling for titles like *Death Stranding Director's Cut* (PPSA01317), runtime font search sanitization, Polyphony Digital engine preset optimizations, and CI release workflow improvements.

### Features & Fixes in v1.1.1

- **Diagnostics & Assertion Path Sanitization:** Sanitized file path reporting in `BuildFatalReport`, `DbgReport`, `DbgExitHandler`, and `HangWatchdog::NoteFatal`. Host machine directories (`C:\Users\...`, `/home/...`) are stripped to clean repository-relative paths (`src/...`), eliminating privacy exposure when crash logs are shared across different systems.
- **Build-Time Macro Remapping:** Added `-fmacro-prefix-map` and `/clang:-fmacro-prefix-map` flags across both `clang-cl` and GCC/Clang in `src/utils.cmake`, stripping build checkout paths directly from binary string tables.
- **Tessellation Recompiler Branch Handling:** Relaxed rigid `EXIT_NOT_IMPLEMENTED` on internal shader branches during `ReflectTessellationStride` in `src/graphics/shader/recompiler/Tessellation.cpp`, allowing tessellation shaders with branches to successfully resolve control point strides without premature emulator termination.
- **Runtime Asset Probing Clean-up:** Removed compile-time `__FILE__` traversal from `fallback_font_candidates` in `src/libs/libFont.cpp`. Fonts are now cleanly located relative to the application executable directory, system fonts, or current working directory.
- **Polyphony Digital Preset Updates:** Added RT stubbing, disabled software RT, and disabled LDS device buffer in `tools/engine-presets/polyphony.json` and `src/common/engineProfile.cpp`.
- **Pipeline Program Disk Cache:** Added `KYTY_PROGRAM_CACHE_MAX_MB` configurable limit defaulting to 2048 MB in `src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp`.
- **Controller Handle Error Handling:** Enhanced `PadOpenArgsAreValid` error return handling in `src/libs/controller.cpp`.
- **CI / Release Workflow:** Enabled release artifact publishing on fork repositories and added custom tag dispatch support in `.github/workflows/build.yml`.

---

## Prior Releases

### KytyPS5 v1.1.0 — DLSS, FSR, Frame Generation & Engine Optimization

KytyPS5 v1.1.0 brings Vulkan DLSS & AMD FSR upscaling, motion-interpolated Frame Generation, an engine-aware optimization subsystem covering 17 engine families, major Unreal Engine 5 stability improvements, host VRAM budgeting, extensive memory and handle leak fixes, and modern C++20 architectural refactoring.

- **DLSS & AMD FSR Upscaling:** Integrated Vulkan 1.3 AMD FSR (EASU & RCAS) spatial upscaler and DLSS TSR temporal reconstruction into the swapchain presentation pipeline (`KYTY_UPSCALER=fsr|dlss|off`) with contrast-adaptive sharpness control (`KYTY_SHARPNESS=0.0-1.0`) and dynamic VideoOut scaling.
- **Vulkan Frame Generation:** Added motion-interpolated frame generation (`KYTY_FRAME_GEN=1`) doubling visual presentation fluidity and frame pacing on high-refresh displays.
- **Bandwidth Spike Elimination:** Optimized fragment shader samplers (`gpu_frame_gen_interpolate.frag` and `gpu_upscale_dlss_tsr.frag`) and queue synchronization, eliminating heavy-scene bandwidth spikes and stalls during dynamic resolution changes.
- **Qt6 Launcher Controls:** Added interactive settings controls in the launcher dialog for upscaler selection (Off / FSR / DLSS), sharpness slider, and Frame Generation toggle. Added command-line flags (`--upscaler`, `--dlss`, `--fsr`, `--sharpness`, `--frame-gen`).
- **Launcher Preset Discovery:** Automated preset staging on build and multi-directory fallback discovery across application, parent, tools, and current directories.
- **Engine Profile Subsystem:** Added executable fingerprinting and engine-tailored configurations for 17 engine families (Unreal Engine 5, Decima, Insomniac, Team Asobi, RAGE, Frostbite, RE Engine, Unity, id Tech, Source 2, CryEngine, REDengine, Anvil, Snowdrop, Fox Engine, and Northlight) under `tools/engine-presets/`.
- **Host GPU VRAM Budgeting:** Integrated host performance memory budgeting (`vramBudget.h`, `vma.cpp`) monitoring allocation limits against physical VRAM to safely evict scratch resources before driver memory exhaustion.
- **Unreal Engine 5 (UE5) Fixes:** Resolved Halloween frame stalls and RenderThread fence deadlock (`ppsa29340`), enabled RT stubbing and predication no-drain for UE5 compute dispatches, allowed host DLL SEH exceptions, added `tools/uecommandline.txt` presets, and disabled fast-first/array shrinking during compute to guarantee shader execution stability.
- **Memory & Resource Leak Fixes:** Resolved thread handle, runtime memory page, and heap ownership leaks across pthread lifecycle, the runtime dynamic linker, filesystem read buffers, video decoder (Dec2), and audio subsystems.
