# DLSS, FSR, and Frame Generation Subsystem Design

**Date:** 2026-10-24  
**Target:** KytyPS5 (Vulkan 1.3 Host Renderer & Presentation Pipeline + Qt6 Launcher)  
**Status:** Draft (Pending User Approval)

---

## 1. Intent and Why

PlayStation 5 titles rendered through KytyPS5 place heavy demands on host GPUs, particularly at 1440p and 4K output resolutions or in compute/ray-tracing heavy games (*Demon's Souls*, *Astro Bot*, *Gran Turismo 7*, *Marvel's Spider-Man 2*). Previously, `Swapchain::RecordPresentCommands` in [`src/graphics/presentation/window/swapchain.cpp`](file:///c:/Users/TAPIOCA/Desktop/repos/KytyPS5/src/graphics/presentation/window/swapchain.cpp) only supported a raw linear `vkCmdBlitImage` (or a 2-texel box downscale filter when `KYTY_PRESENT_BOX_DOWNSCALE=1`).

Players want:
1. **Higher FPS and smoother gameplay** via lower internal rendering costs and intermediate frame synthesis.
2. **Crisp visual quality with zero graphical glitches** (no HUD warping, no ghosting across camera cuts, no color-space corruption in SDR or HDR10).
3. **Both DLSS and FSR upscaling plus Frame Generation**, each independently optional and toggleable by the user via the Launcher GUI, CLI arguments, live environment switches, and in-game hotkeys.
4. **Universal GPU compatibility across NVIDIA RTX/GTX, AMD Radeon RX, and Intel Arc/iGPU hardware** without locking any feature to a single GPU vendor or requiring proprietary external DLLs to function.

---

## 2. Goals, Non-Goals, and Anti-Goals

### Goals
- **Independent Upscaler Selection (`Off`, `FSR`, `DLSS`)**:
  - **FSR (AMD FidelityFX Super Resolution)**: Two-pass Vulkan 1.3 pipeline implementing **EASU** (12-tap Edge-Adaptive Spatial Upsampling) followed by **RCAS** (Robust Contrast-Adaptive Sharpening).
  - **DLSS (Deep Learning / Temporal Super Resolution)**: Dual-path architecture:
    1. **Optional Native NGX Dynamic Loader (Windows NVIDIA RTX)**: Dynamically probes for `nvngx_dlss.dll` / `_nvngx.dll` at runtime via `LoadLibraryW` without build-time SDK dependencies.
    2. **Universal Vulkan 1.3 Temporal Super Resolution (TSR / DLSS Compute Path)**: Built-in multi-pass Vulkan pipeline (hierarchical block motion estimation, bicubic/Catmull-Rom temporal history reprojection, YCoCg $3\times3$ neighborhood variance clamping, and RCAS sharpening) that runs on **all** NVIDIA RTX/GTX, AMD Radeon RX, and Intel GPUs (Windows, Linux, macOS).
- **Quality Presets & Sharpness Control**:
  - Presets: `NativeAA` ($1.0\times$), `Quality` ($1.5\times$), `Balanced` ($1.7\times$), `Performance` ($2.0\times$), `UltraPerformance` ($3.0\times$).
  - Sharpness: Configurable $0\text{--}100\%$ (default $50\%$).
  - Internal render resolution coordination with `VideoOut`: When upscaling with a scale factor $> 1.0\times$ is active, `VideoOut` reports the scaled internal dimensions (`screen_width / scale`, `screen_height / scale`) so PS5 titles supporting dynamic/1080p output automatically drop their internal rendering workload while the host window presents at full target resolution. For titles that still allocate fixed-size buffers, the upscaler samples at the scaled effective resolution and reconstructs to the swapchain extent.
- **Glitch-Free Optional Frame Generation (`On` / `Off`)**:
  - Works on all NVIDIA, AMD, and Intel Vulkan 1.3 GPUs, with or without upscaling enabled.
  - Synthesizes an intermediate midpoint frame $F_{t-0.5}$ between consecutive completed main-bus guest flips $F_{t-1}$ and $F_t$ using GPU hierarchical motion estimation and bidirectional motion-compensated blending.
  - **Anti-Glitch Safeguards**:
    - **Scene-cut & disocclusion detection**: Computes per-tile luminance/color difference and motion divergence; automatically falls back to clean frame repetition/unwarped output on camera cuts, menu transitions, blank frames, or extent/HDR changes.
    - **UI / Overlay isolation**: Only the main game layer (`layers[0]`) is upscaled and motion-interpolated. Guest overlay (`layers[1]`) and host `SystemOverlay` (ImGui, trophy toasts, IME dialogs) are composited *after* upscaling and frame generation onto each presented swapchain image so UI text and overlays never smear or warp.
- **Complete User Controls**:
  - **Qt Launcher GUI**: New controls in `ConfigurationEditDialog` (`Graphics & display` group) saved globally and per-game (`Configuration`).
  - **CLI Flags**: `--upscaler <off|fsr|dlss>`, `--upscale-quality <native|quality|balanced|performance|ultra_performance>`, `--upscale-sharpness <0-100>`, `--frame-gen <on|off>`.
  - **Live Switches (`Common::Live::Switch`)**: `KYTY_UPSCALER`, `KYTY_UPSCALE_QUALITY`, `KYTY_UPSCALE_SHARPNESS`, `KYTY_FRAME_GEN` for zero-restart live toggling and A/B benchmarking.
  - **In-Game Hotkeys & Window Title Telemetry**: `F9` cycles Upscaler (`Off -> FSR -> DLSS`), `F10` toggles Frame Generation (`On <-> Off`), and the window title bar displays active upscaler/frame-gen status alongside FPS.

### Non-Goals
- Modifying guest PS5 shader code to inject engine-level jitter into proprietary guest projection matrices (emulator-level upscaling and frame generation operate at the `VideoOut` / `Presenter` boundary so every game works without per-title二进制 patches).
- Requiring users to download proprietary NVIDIA Streamline/NGX or AMD FidelityFX SDKs to build or run KytyPS5.

### Anti-Goals (Failure Conditions Even If It Technically Works)
- **Vendor Lock-In**: Any mode (`DLSS`, `FSR`, or `Frame Generation`) failing, crashing, or refusing to run on AMD RX, Intel Arc, or older RTX/GTX GPUs.
- **Graphical Glitches**: Ghosting across scene cuts, warped HUD/trophy overlays, broken HDR10 PQ color curves, or Vulkan validation errors on layout transitions.
- **Presentation Thread Stalls**: Re-introducing global renderer mutex contention during swapchain recording (`KYTY_PRESENT_UNLOCKED` compatibility must be preserved by keeping upscaler/frame-gen resources owned by the presenter/swapchain).

---

## 3. Constraints & Codebase Integration

1. **Build System & Shader Compilation ([`CMakeLists.txt`](file:///c:/Users/TAPIOCA/Desktop/repos/KytyPS5/CMakeLists.txt))**:
   - Host shaders live in `src/graphics/host_gpu/shaders/` and are compiled to SPIR-V headers via `glslangValidator -V --target-env vulkan1.3 -Os` using `add_gpu_blit_shader` and `embed_spirv.cmake`.
   - Must compile cleanly with `clang`, `clang-cl`, and `gcc` under C++20 with zero external SDK dependencies required at build time.
   - Must not touch files listed in `kyty_codegen_sources` (`src/codegen_version.cmake`) so the guest shader disk cache fingerprint remains stable.
2. **Vulkan 1.3 Presentation Pipeline ([`src/graphics/presentation/window/swapchain.cpp`](file:///c:/Users/TAPIOCA/Desktop/repos/KytyPS5/src/graphics/presentation/window/swapchain.cpp))**:
   - Uses dynamic rendering (`vkCmdBeginRendering` / `vkCmdEndRendering`), push descriptors (`VK_KHR_push_descriptor`), and `synchronization2` (`vkCmdPipelineBarrier2`).
   - All upscaler and frame-generation images, views, samplers, descriptor layouts, and pipelines must be owned by `Swapchain` / `UpscalerPass` / `FrameGenPass` and recorded on `present_scheduler` (the presenter's own `CommandScheduler`), preserving lock-free operation under `KYTY_PRESENT_UNLOCKED=1`.
3. **Color Space & Format Handling**:
   - Swapchain formats may be SDR (`eB8G8R8A8Unorm`, `eR8G8B8A8Unorm`) or HDR10 (`eA2B10G10R10UnormPack32`, `eR16G16B16A16Sfloat`).
   - All shaders must preserve alpha and operate cleanly on normalized linear/sRGB/PQ values without clamping HDR highlights prematurely.

---

## 4. Architecture & Component Design

### 4.1 Configuration & Live State (`src/common/emulatorConfig.h`, `src/common/emulatorConfig.cpp`)

New enums and fields in `Config`:
```cpp
enum class UpscalerType : uint8_t { Off, Fsr, Dlss };
enum class UpscaleQuality : uint8_t {
    NativeAA,         // 1.0x scale
    Quality,          // 1.5x scale
    Balanced,         // 1.7x scale
    Performance,      // 2.0x scale
    UltraPerformance, // 3.0x scale
};
```
In `Config::ConfigOptions`:
- `UpscalerType upscaler_type = UpscalerType::Off;`
- `UpscaleQuality upscale_quality = UpscaleQuality::Quality;`
- `uint32_t upscale_sharpness = 50;` (0..100)
- `bool frame_generation_enabled = false;`

Helper functions in `Config`:
- `UpscalerType GetUpscalerType();` / `void SetUpscalerType(UpscalerType type);`
- `UpscaleQuality GetUpscaleQuality();` / `void SetUpscaleQuality(UpscaleQuality quality);`
- `uint32_t GetUpscaleSharpness();` / `void SetUpscaleSharpness(uint32_t sharpness);`
- `bool FrameGenerationEnabled();` / `void SetFrameGenerationEnabled(bool enabled);`
- `float GetUpscaleFactor(UpscaleQuality quality) noexcept;`
- `void GetInternalRenderResolution(uint32_t display_width, uint32_t display_height, uint32_t& out_width, uint32_t& out_height) noexcept;`

Live switches (`Live::Switch`) in `swapchain.cpp` / `emulatorConfig.cpp` synchronize with `KYTY_UPSCALER`, `KYTY_UPSCALE_QUALITY`, `KYTY_UPSCALE_SHARPNESS`, and `KYTY_FRAME_GEN`, and runtime hotkeys (`F9` / `F10` in `window.cpp`) update the atomic active state immediately.

### 4.2 Internal Resolution Scaling (`src/libs/agc.cpp`, `src/graphics/presentation/videoOut.cpp`)

- `WindowInit(display_width, display_height)` always creates the host window at the user's configured screen resolution (`Config::GetScreenWidth()`, `Config::GetScreenHeight()`).
- `VideoOutInit(render_width, render_height, presenter)` receives the scaled internal resolution computed by `Config::GetInternalRenderResolution(...)` (aligned to even pixels, with standard 16:9 snapping such as $1920\times1080$ for $2560\times1440$ Quality or $3840\times2160$ Performance).
- In `VideoOutGetOutputStatus`, when `UpscalerType != Off` and the internal render resolution is $\le 1920\times1080$, `status->resolution` reports `1u` (1080p) so games with native 1080p performance modes render at 1080p while the host upscales to 1440p/4K.

### 4.3 Upscaler Pipeline (`src/graphics/presentation/window/upscaler.h`, `upscaler.cpp`)

Owned by `Swapchain` and invoked during `Swapchain::RecordPresentCommands`:

1. **FSR Mode (`UpscalerType::Fsr`)**:
   - **Pass 1 — EASU (`gpu_upscale_fsr_easu.frag`)**:
     - 12-tap isotropic directional Lanczos2 kernel adapted from AMD FidelityFX FSR 1.0.
     - Analyzes local luminance gradients in a $4\times4$ texel neighborhood to determine edge direction and elongation, preventing jagged edges and ringing.
     - Renders into an intermediate `eR16G16B16A16Sfloat` (or swapchain-format matched) target at `m_extent`.
   - **Pass 2 — RCAS (`gpu_upscale_fsr_rcas.frag`)**:
     - 5-tap cross neighborhood contrast-adaptive sharpening pass parameterized by `upscale_sharpness` ($0\text{--}100\%$ mapped to attenuation stops).
     - Writes directly to the swapchain image (`m_image_views[m_image_index]`).

2. **DLSS Mode (`UpscalerType::Dlss`)**:
   - **Optional Native NGX Probe**: Checks once per device creation for `nvngx_dlss.dll` on NVIDIA GPUs (`vendorID == 0x10de`). Logs whether native NGX is loaded or if the universal Vulkan TSR compute/fragment pipeline is active.
   - **Universal Vulkan 1.3 Temporal Super Resolution (`gpu_upscale_dlss_tsr.frag` + `gpu_upscale_fsr_rcas.frag`)**:
     - Maintains a per-swapchain-extent history texture (`m_history_image`) and previous low-res frame (`m_prev_input_image`).
     - **Motion Vector Estimation**: Searches a local $3\times3$ coarse + refined neighborhood between current source and `m_prev_input_image` in luma space to find sub-pixel screen-space velocity without requiring guest engine motion buffers.
     - **Temporal Reprojection & YCoCg Variance Clamping**: Samples the current frame with a 5-tap Catmull-Rom filter, converts the $3\times3$ neighborhood to YCoCg color space, computes mean $\mu$ and standard deviation $\sigma$ ($AABB = [\mu - \gamma\sigma, \mu + \gamma\sigma]$), clamps the motion-reprojected history sample to the neighborhood box (eliminating ghosting on moving objects and disocclusions), and blends with temporal weight $\alpha \in [0.08, 0.25]$ modulated by motion confidence and scene-cut detection.
     - Copies the reconstructed output to `m_history_image` and runs the **RCAS** sharpening pass into the swapchain attachment.

### 4.4 Frame Generation Pipeline (`src/graphics/presentation/window/frameGen.h`, `frameGen.cpp`)

Owned by `Swapchain` / `Presenter`:
- Maintains the previous completed main-bus frame copy (`m_prev_frame`) and a synthesized intermediate frame (`m_interp_frame`).
- **Synthesis Shader (`gpu_frame_gen_interpolate.frag`)**:
  - Given `prev_frame` ($F_{t-1}$) and `curr_frame` ($F_t$):
  - Computes hierarchical block motion vectors (coarse mip/strided $5\times5$ search + fine $3\times3$ refinement) forward and backward between $F_{t-1}$ and $F_t$.
  - **Scene-Cut & Glitch Guard**: Calculates local photometric error $|L(F_{t-1}(p - v/2)) - L(F_t(p + v/2))|$ and motion gradient consistency. When error exceeds the scene-cut threshold (e.g., camera cut, UI screen flash, fast occlusion), blending weight smoothly falls back to $F_t$ so no torn blocks or ghosted silhouettes ever appear.
  - Produces the $t = 0.5$ midpoint frame $F_{t-0.5}$.
- **Presentation Pacing (`Presenter::Impl::Present`)**:
  - When Frame Generation is active and a new real main-bus frame $F_t$ is presented after a compatible $F_{t-1}$:
    1. Synthesizes $F_{t-0.5}$ on the GPU, upscales it (if Upscaler is active), composites `layers[1]` (guest overlay) and `SystemOverlay` on top, and presents it to the swapchain (incrementing `presented_frames`).
    2. Paces the midpoint interval (up to half the vblank period in `Mailbox`/`Immediate` mode, or queued cleanly in `Fifo`) before presenting the real frame $F_t$ (also upscaled + overlays composited, incrementing `presented_frames`).
  - Updates `m_prev_frame` with $F_t$.

### 4.5 Launcher UI, CLI, and Hotkeys

1. **Launcher (`src/launcher/include/configuration.h`, `src/launcher/src/configuration.cpp`, `src/launcher/src/configurationEditDialog.cpp`, `src/launcher/forms/configuration_edit_dialog.ui`, `src/launcher/src/mainDialog.cpp`)**:
   - New enums on `Configuration`: `UpscalerType` (`Off`, `Fsr`, `Dlss`) and `UpscaleQuality` (`NativeAA`, `Quality`, `Balanced`, `Performance`, `UltraPerformance`).
   - New fields on `Configuration`: `upscaler_type`, `upscale_quality`, `upscale_sharpness` (0..100, default 50), `frame_generation_enabled` (bool, default false).
   - UI widgets in `graphicsLayout`:
     - `comboBox_upscaler` ("Upscaler (FSR / DLSS):")
     - `comboBox_upscale_quality` ("Upscale quality:")
     - `slider_upscale_sharpness` + `label_upscale_sharpness_value` ("Upscale sharpness:")
     - `checkBox_frame_generation` ("Enable frame generation (smooth FPS boost)")
   - Serialized in `Configuration::GameSettings()` / `ReadGameSettingsValues()` with backward-compatible defaults so older exported JSONs and `QSettings` load without error.
   - Passed to `kyty_emulator` in `CreateEmulatorArgs()`.
2. **CLI (`src/main.cpp`)**:
   - `--upscaler <off|fsr|dlss>`
   - `--upscale-quality <native|quality|balanced|performance|ultra_performance>`
   - `--upscale-sharpness <0-100>`
   - `--frame-gen <on|off|true|false>`
3. **In-Game Hotkeys & Window Title (`src/graphics/presentation/window/window.cpp`)**:
   - `F9`: Cycles `UpscalerType` (`Off -> FSR -> DLSS -> Off`).
   - `F10`: Toggles `FrameGenerationEnabled` (`Off <-> On`).
   - `WindowContext::UpdateTitle`: Appends active tag, e.g. `[FSR Quality | FG]` or `[DLSS Balanced]` when enabled.

---

## 5. What Is Left to the Builder

- Exact GLSL constants for Lanczos2 / EASU lobe weights, RCAS stop attenuation scale, and YCoCg variance clipping $\gamma$ factor (standard values: $\gamma = 1.25$, RCAS stop scale $0.0\text{--}1.0$).
- Unit test coverage in `tests/UpscalerFrameGenTests.cpp` verifying:
  - `Config` resolution scaling math across all quality presets and aspect ratios.
  - CLI argument parsing and live switch overrides (`KYTY_UPSCALER`, `KYTY_UPSCALE_QUALITY`, `KYTY_UPSCALE_SHARPNESS`, `KYTY_FRAME_GEN`).
  - Scene-cut fallback threshold math and state transitions when toggling upscaler/frame-gen modes or resizing the swapchain.
  - SPIR-V shader binary embedding verification for all new upscaler and frame-generation shaders.
