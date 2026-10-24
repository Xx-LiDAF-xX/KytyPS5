# KytyPS5 v1.1.0 - DLSS, FSR, Frame Generation & Engine Optimization

KytyPS5 v1.1.0 by Xx-LiDAF-xX brings Vulkan DLSS & AMD FSR upscaling, motion-interpolated Frame Generation, an engine-aware optimization subsystem covering 17 engine families, major Unreal Engine 5 stability improvements, host VRAM budgeting, extensive memory and handle leak fixes, and modern C++20 architectural refactoring.

### Features & Improvements

- **DLSS & AMD FSR Upscaling:** Integrated Vulkan 1.3 AMD FSR (EASU & RCAS) spatial upscaler and DLSS TSR temporal reconstruction into the swapchain presentation pipeline (`KYTY_UPSCALER=fsr|dlss|off`) with contrast-adaptive sharpness control (`KYTY_SHARPNESS=0.0-1.0`) and dynamic VideoOut scaling.
- **Vulkan Frame Generation:** Added motion-interpolated frame generation (`KYTY_FRAME_GEN=1`) doubling visual presentation fluidity and frame pacing on high-refresh displays.
- **Bandwidth Spike Elimination:** Optimized fragment shader samplers (`gpu_frame_gen_interpolate.frag` and `gpu_upscale_dlss_tsr.frag`) and queue synchronization, eliminating heavy-scene bandwidth spikes and stalls during dynamic resolution changes.
- **Qt6 Launcher Controls:** Added interactive settings controls in the launcher dialog for upscaler selection (Off / FSR / DLSS), sharpness slider, and Frame Generation toggle. Added command-line flags (`--upscaler`, `--dlss`, `--fsr`, `--sharpness`, `--frame-gen`).
- **Launcher Preset Discovery:** Automated preset staging on build and multi-directory fallback discovery across application, parent, tools, and current directories.
- **Engine Profile Subsystem:** Added executable fingerprinting and engine-tailored configurations for 17 engine families (Unreal Engine 5, Decima, Insomniac, Team Asobi, RAGE, Frostbite, RE Engine, Unity, id Tech, Source 2, CryEngine, REDengine, Anvil, Snowdrop, Fox Engine, and Northlight) under `tools/engine-presets/`.
- **Host GPU VRAM Budgeting:** Integrated host performance memory budgeting (`vramBudget.h`, `vma.cpp`) monitoring allocation limits against physical VRAM to safely evict scratch resources before driver memory exhaustion.
- **Window Title Branding:** Labeled `[KytyPS5 (Xx-LiDAF-xX) Source build <hash> | Release]` in the running game window caption.

### Fixes & Stability

- **Unreal Engine 5 (UE5) Fixes:** Resolved Halloween frame stalls and RenderThread fence deadlock (`ppsa29340`), enabled RT stubbing and predication no-drain for UE5 compute dispatches, allowed host DLL SEH exceptions, added `tools/uecommandline.txt` presets, and disabled fast-first/array shrinking during compute to guarantee shader execution stability.
- **Memory & Resource Leak Fixes:** Resolved thread handle, runtime memory page, and heap ownership leaks across pthread lifecycle, the runtime dynamic linker, filesystem read buffers, video decoder (Dec2), and audio subsystems.
- **Command Processor (CP) Conditional Execution:** Fixed CP conditional execution and re-initialization of live switch parameters on runtime resets.
- **Modern C++20 Refactoring:** Cache-line aligned dense slot indexing in `SlotVector`, centralized runtime environment flags, thread-safe singletons replacing malloc singletons, and modern `std::variant`/`std::span`/`std::ranges` dispatch across ELF loading and system content parsing.
