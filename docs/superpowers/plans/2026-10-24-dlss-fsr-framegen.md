# DLSS, FSR, and Frame Generation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement optional, cross-vendor DLSS (Temporal Super Resolution + optional NGX runtime probe), FSR (EASU + RCAS), and glitch-free motion-compensated Frame Generation across the KytyPS5 Vulkan 1.3 presentation pipeline, CLI, live switches, hotkeys, and Qt6 Launcher.

**Architecture:** Configuration and live switches (`Config::UpscalerType`, `Config::UpscaleQuality`, sharpness, frame generation, and `Config::GetInternalRenderResolution`) coordinate internal render resolution in `VideoOut` with target window resolution in `Swapchain`. Four Vulkan 1.3 GLSL fragment shaders (`gpu_upscale_fsr_easu.frag`, `gpu_upscale_fsr_rcas.frag`, `gpu_upscale_dlss_tsr.frag`, `gpu_frame_gen_interpolate.frag`) are compiled to SPIR-V headers at build time and executed by presenter-owned `UpscalerPass` and `FrameGenPass` classes on the main game layer (`layers[0]`) before guest overlays (`layers[1]`) and host `SystemOverlay` are composited.

**Tech Stack:** C++20, Vulkan 1.3 (`VK_KHR_dynamic_rendering`, `VK_KHR_push_descriptor`, `VK_KHR_synchronization2`), GLSL 450 (`glslangValidator`), SDL2, Qt6 Widgets, CMake/Ninja.

**Spec:** [`docs/superpowers/specs/2026-10-24-dlss-fsr-framegen-design.md`](file:///c:/Users/TAPIOCA/Desktop/repos/KytyPS5/docs/superpowers/specs/2026-10-24-dlss-fsr-framegen-design.md)

## Global Constraints

- Host shaders live in `src/graphics/host_gpu/shaders/` and are compiled to SPIR-V headers via `glslangValidator -V --target-env vulkan1.3 -Os` using `add_gpu_blit_shader` and `embed_spirv.cmake`.
- Must compile cleanly with `clang`, `clang-cl`, and `gcc` under C++20 with zero external SDK dependencies required at build time.
- Must not touch files listed in `kyty_codegen_sources` (`src/codegen_version.cmake`) so the guest shader disk cache fingerprint remains stable.
- All upscaler and frame-generation images, views, samplers, descriptor layouts, and pipelines must be owned by `Swapchain` / `UpscalerPass` / `FrameGenPass` and recorded on `present_scheduler` (the presenter's own `CommandScheduler`), preserving lock-free operation under `KYTY_PRESENT_UNLOCKED=1`.
- Swapchain formats may be SDR (`eB8G8R8A8Unorm`, `eR8G8B8A8Unorm`) or HDR10 (`eA2B10G10R10UnormPack32`, `eR16G16B16A16Sfloat`); all shaders must preserve alpha and operate cleanly on normalized values without clamping HDR highlights prematurely.
- Universal GPU compatibility across NVIDIA RTX/GTX, AMD Radeon RX, and Intel Arc/iGPU hardware without locking any feature to a single GPU vendor or requiring proprietary external DLLs to function.

## Review Focus

1. Zero or tiny display dimensions (`0x0` or `1x1`) passed to `Config::GetInternalRenderResolution`: must clamp safely to minimum valid even dimensions (`>= 2x2`) without division-by-zero or integer underflow (tested in Task 1 `TestZeroAndSmallResolutionEdgeCases`).
2. Out-of-range sharpness values (`> 100`, e.g. `250` or `999`) or case/hyphen/whitespace variations in mode strings (`" DLSS "`, `"ultra-performance"`): must clamp sharpness to `0..100` and parse strings case-insensitively with hyphen/underscore tolerance, falling back cleanly on unknown strings (tested in Task 1 `TestSharpnessClampingAndStringParsingEdgeCases`).
3. Non-16:9 aspect ratios (`3440x1440` ultrawide 21:9, `1280x800` 16:10) under `Quality` or `Performance` presets: must scale proportionally with even-pixel alignment instead of snapping to 16:9 `1920x1080` and distorting aspect ratio (tested in Task 1 `TestNon16By9UltrawideAnd16By10ResolutionScaling`).
4. Scene cuts, camera flashes, or swapchain resize during Frame Generation: when photometric luma difference exceeds the scene-cut threshold or extent changes, frame-gen blend weight must drop to `0.0f` (100% current frame $F_t$) so zero ghosting or torn blocks appear (tested in Task 3 `TestFrameGenSceneCutAndResizeReset`).
5. Legacy Launcher configurations or JSON exports missing the new upscaler/frame-gen keys: must load with safe defaults (`Off`, `Quality`, sharpness `50`, frame generation `false`) without failing or clobbering existing settings (tested in Task 4).

---

### Task 1: Emulator Configuration, Resolution Scaling, Live Switches, and CLI Flags

**Files:**
- Modify: `src/common/emulatorConfig.h:1-85`
- Modify: `src/common/emulatorConfig.cpp:1-225`
- Modify: `src/main.cpp:60-260`
- Modify: `src/libs/agc.cpp:70-100`
- Modify: `src/graphics/presentation/videoOut.cpp:1950-1975`
- Create: `tests/UpscalerFrameGenTests.cpp`
- Modify: `CMakeLists.txt:790-810`

**Interfaces:**
- Consumes: `Common::Live::Switch` (`src/common/liveSwitch.h`)
- Produces:
  ```cpp
  namespace Config {
  enum class UpscalerType : uint8_t { Off = 0, Fsr = 1, Dlss = 2 };
  enum class UpscaleQuality : uint8_t {
      NativeAA = 0,         // 1.0x scale
      Quality = 1,          // 1.5x scale
      Balanced = 2,         // 1.7x scale
      Performance = 3,      // 2.0x scale
      UltraPerformance = 4, // 3.0x scale
  };

  UpscalerType GetUpscalerType();
  void SetUpscalerType(UpscalerType type);
  UpscalerType CycleUpscalerType();
  std::string_view UpscalerTypeToString(UpscalerType type) noexcept;
  UpscalerType ParseUpscalerType(std::string_view str) noexcept;

  UpscaleQuality GetUpscaleQuality();
  void SetUpscaleQuality(UpscaleQuality quality);
  std::string_view UpscaleQualityToString(UpscaleQuality quality) noexcept;
  UpscaleQuality ParseUpscaleQuality(std::string_view str) noexcept;

  uint32_t GetUpscaleSharpness();
  void SetUpscaleSharpness(uint32_t sharpness);

  bool FrameGenerationEnabled();
  void SetFrameGenerationEnabled(bool enabled);
  bool ToggleFrameGeneration();

  float GetUpscaleFactor(UpscaleQuality quality) noexcept;
  void GetInternalRenderResolution(uint32_t display_width, uint32_t display_height,
                                   uint32_t& out_width, uint32_t& out_height) noexcept;
  std::string FormatUpscalerStatusTag();
  } // namespace Config
  ```

- [ ] **Step 1: Add `upscaler_frame_gen_tests` target to `CMakeLists.txt` and write failing config/resolution tests in `tests/UpscalerFrameGenTests.cpp`**

Register `upscaler_frame_gen_tests` in `CMakeLists.txt` right after `engine_profile_tests`:
```cmake
add_executable(upscaler_frame_gen_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/UpscalerFrameGenTests.cpp"
)
target_link_libraries(upscaler_frame_gen_tests common)
target_include_directories(upscaler_frame_gen_tests PRIVATE ${inc_headers})
if(LINUX)
	target_link_libraries(upscaler_frame_gen_tests Threads::Threads)
endif()
```

Create `tests/UpscalerFrameGenTests.cpp` testing:
```cpp
void TestUpscalerEnumsAndParsing() {
	using namespace Config;
	Check(ParseUpscalerType("off") == UpscalerType::Off, "parse off");
	Check(ParseUpscalerType("fsr") == UpscalerType::Fsr, "parse fsr");
	Check(ParseUpscalerType("DLSS") == UpscalerType::Dlss, "parse DLSS uppercase");
	Check(UpscalerTypeToString(UpscalerType::Fsr) == "FSR", "FSR to string");
	Check(UpscalerTypeToString(UpscalerType::Dlss) == "DLSS", "DLSS to string");

	Check(ParseUpscaleQuality("native") == UpscaleQuality::NativeAA, "parse native");
	Check(ParseUpscaleQuality("quality") == UpscaleQuality::Quality, "parse quality");
	Check(ParseUpscaleQuality("balanced") == UpscaleQuality::Balanced, "parse balanced");
	Check(ParseUpscaleQuality("performance") == UpscaleQuality::Performance, "parse performance");
	Check(ParseUpscaleQuality("ultra_performance") == UpscaleQuality::UltraPerformance, "parse ultra_performance");
}

void TestSharpnessClampingAndStringParsingEdgeCases() {
	using namespace Config;
	Check(ParseUpscalerType("  DlSs ") == UpscalerType::Dlss, "trimmed DLSS");
	Check(ParseUpscalerType("unknown_mode") == UpscalerType::Off, "unknown defaults to Off");
	Check(ParseUpscaleQuality("Ultra-Performance") == UpscaleQuality::UltraPerformance, "hyphenated ultra-performance");
	Check(ParseUpscaleQuality("invalid") == UpscaleQuality::Quality, "invalid quality defaults to Quality");

	SetUpscaleSharpness(250);
	Check(GetUpscaleSharpness() == 100u, "sharpness clamped to 100");
	SetUpscaleSharpness(50);
	Check(GetUpscaleSharpness() == 50u, "sharpness restored to 50");
}

void TestInternalResolutionScaling16By9() {
	using namespace Config;
	SetUpscalerType(UpscalerType::Dlss);
	SetUpscaleQuality(UpscaleQuality::Quality);
	uint32_t w = 0, h = 0;
	GetInternalRenderResolution(3840, 2160, w, h);
	Check(w == 2560 && h == 1440, "4K Quality -> 2560x1440");

	SetUpscaleQuality(UpscaleQuality::Performance);
	GetInternalRenderResolution(3840, 2160, w, h);
	Check(w == 1920 && h == 1080, "4K Performance -> 1920x1080");

	SetUpscaleQuality(UpscaleQuality::UltraPerformance);
	GetInternalRenderResolution(3840, 2160, w, h);
	Check(w == 1280 && h == 720, "4K UltraPerformance -> 1280x720");

	SetUpscalerType(UpscalerType::Off);
	GetInternalRenderResolution(3840, 2160, w, h);
	Check(w == 3840 && h == 2160, "Off -> native 3840x2160");
}

void TestNon16By9UltrawideAnd16By10ResolutionScaling() {
	using namespace Config;
	SetUpscalerType(UpscalerType::Fsr);
	SetUpscaleQuality(UpscaleQuality::Performance); // 2.0x
	uint32_t w = 0, h = 0;
	GetInternalRenderResolution(3440, 1440, w, h);
	Check(w == 1720 && h == 720, "3440x1440 Performance preserves 21:9 -> 1720x720");
	GetInternalRenderResolution(1280, 800, w, h);
	Check(w == 640 && h == 400, "1280x800 Performance preserves 16:10 -> 640x400");
	SetUpscalerType(UpscalerType::Off);
}

void TestZeroAndSmallResolutionEdgeCases() {
	using namespace Config;
	SetUpscalerType(UpscalerType::Dlss);
	SetUpscaleQuality(UpscaleQuality::UltraPerformance);
	uint32_t w = 0, h = 0;
	GetInternalRenderResolution(0, 0, w, h);
	Check(w >= 2 && h >= 2 && (w % 2 == 0) && (h % 2 == 0), "0x0 clamps to >= 2x2 even");
	GetInternalRenderResolution(1, 1, w, h);
	Check(w >= 2 && h >= 2 && (w % 2 == 0) && (h % 2 == 0), "1x1 clamps to >= 2x2 even");
	SetUpscalerType(UpscalerType::Off);
}

void TestHotkeyCycleAndStatusTag() {
	using namespace Config;
	SetUpscalerType(UpscalerType::Off);
	SetUpscaleQuality(UpscaleQuality::Quality);
	SetFrameGenerationEnabled(false);
	Check(FormatUpscalerStatusTag().empty(), "empty tag when both off");

	Check(CycleUpscalerType() == UpscalerType::Fsr, "Off -> Fsr");
	Check(FormatUpscalerStatusTag() == "[FSR Quality]", "FSR Quality tag");

	Check(ToggleFrameGeneration() == true, "FG Off -> On");
	Check(FormatUpscalerStatusTag() == "[FSR Quality | FG]", "FSR Quality + FG tag");

	Check(CycleUpscalerType() == UpscalerType::Dlss, "Fsr -> Dlss");
	Check(FormatUpscalerStatusTag() == "[DLSS Quality | FG]", "DLSS Quality + FG tag");

	Check(CycleUpscalerType() == UpscalerType::Off, "Dlss -> Off");
	Check(FormatUpscalerStatusTag() == "[FG]", "FG only tag");
	SetFrameGenerationEnabled(false);
}
```

- [ ] **Step 2: Run build on `upscaler_frame_gen_tests` to verify it fails**

Run: `ninja -C _Build upscaler_frame_gen_tests`
Expected: FAIL with undeclared identifiers `UpscalerType`, `UpscaleQuality`, `GetInternalRenderResolution`, etc.

- [ ] **Step 3: Implement `Config` functions, CLI options, and `VideoOut` resolution coordination**

1. In `src/common/emulatorConfig.h` and `src/common/emulatorConfig.cpp`:
   - Add `UpscalerType` and `UpscaleQuality` enums, `ConfigOptions` fields (`upscaler_type`, `upscale_quality`, `upscale_sharpness`, `frame_generation_enabled`), atomic runtime state backed by `Common::Live::Switch` (`KYTY_UPSCALER`, `KYTY_UPSCALE_QUALITY`, `KYTY_UPSCALE_SHARPNESS`, `KYTY_FRAME_GEN`), string parsing/formatting, `GetUpscaleFactor`, `GetInternalRenderResolution`, and `FormatUpscalerStatusTag`.
   - In `GetInternalRenderResolution`: clamp `display_width` and `display_height` to at least `2`, return even-aligned native dimensions when `GetUpscalerType() == UpscalerType::Off` or `scale <= 1.0f`, otherwise divide by `scale` and round to even integers (`>= 2`).
2. In `src/main.cpp`:
   - Add CLI options in `cxxopts` under `"Graphics"`:
     - `("upscaler", "Upscaler mode: off, fsr, dlss", cxxopts::value<std::string>())`
     - `("upscale-quality", "Upscale quality: native, quality, balanced, performance, ultra_performance", cxxopts::value<std::string>())`
     - `("upscale-sharpness", "Upscale sharpness (0-100)", cxxopts::value<uint32_t>())`
     - `("frame-gen", "Enable frame generation (on/off)", cxxopts::value<std::string>())`
   - Parse them before `Emulator::Run`.
3. In `src/libs/agc.cpp` (`Libs::Graphics::Initialize`):
   - Pass `display_width = Config::GetScreenWidth()` and `display_height = Config::GetScreenHeight()` to `WindowInit(display_width, display_height)`.
   - Compute `uint32_t render_width = display_width, render_height = display_height; Config::GetInternalRenderResolution(display_width, display_height, render_width, render_height);` and pass `(render_width, render_height, presenter)` to `VideoOut::VideoOutInit`.
4. In `src/graphics/presentation/videoOut.cpp` (`VideoOutGetOutputStatus`):
   - When `Config::GetUpscalerType() != Config::UpscalerType::Off && ctx->width <= 1920 && ctx->height <= 1080`, set `status->resolution = 1u;` (1080p), else keep `status->resolution = (ctx->width < 3840 && ctx->height < 2160) ? 0u : 2u;`.

- [ ] **Step 4: Build and run `upscaler_frame_gen_tests` to verify it passes**

Run: `ninja -C _Build upscaler_frame_gen_tests && .\_Build\upscaler_frame_gen_tests.exe`
Expected: PASS (`UpscalerFrameGenTests: all checks passed`)

- [ ] **Step 5: Commit Task 1**

```bash
git add CMakeLists.txt src/common/emulatorConfig.h src/common/emulatorConfig.cpp src/main.cpp src/libs/agc.cpp src/graphics/presentation/videoOut.cpp tests/UpscalerFrameGenTests.cpp
git commit -m "feat(config): add DLSS/FSR upscaler, frame generation config, CLI flags, and VideoOut scaling"
```

---

### Task 2: Vulkan 1.3 GLSL Shaders for FSR (EASU + RCAS), DLSS (TSR), and Frame Generation

**Files:**
- Create: `src/graphics/host_gpu/shaders/gpu_upscale_fsr_easu.frag`
- Create: `src/graphics/host_gpu/shaders/gpu_upscale_fsr_rcas.frag`
- Create: `src/graphics/host_gpu/shaders/gpu_upscale_dlss_tsr.frag`
- Create: `src/graphics/host_gpu/shaders/gpu_frame_gen_interpolate.frag`
- Modify: `CMakeLists.txt:459-467`
- Modify: `tests/UpscalerFrameGenTests.cpp`

**Interfaces:**
- Consumes: `gpu_blit_fs_triangle.vert` (`layout(location = 0) in vec2 in_uv; layout(location = 0) out vec4 out_color;`)
- Produces:
  - Generated SPIR-V headers in `${PROJECT_BINARY_DIR}/gpu_blit_shaders/`:
    - `gpu_blit_shaders/gpu_upscale_fsr_easu_spv.h` (`GPU_UPSCALE_FSR_EASU_SPV`)
    - `gpu_blit_shaders/gpu_upscale_fsr_rcas_spv.h` (`GPU_UPSCALE_FSR_RCAS_SPV`)
    - `gpu_blit_shaders/gpu_upscale_dlss_tsr_spv.h` (`GPU_UPSCALE_DLSS_TSR_SPV`)
    - `gpu_blit_shaders/gpu_frame_gen_interpolate_spv.h` (`GPU_FRAME_GEN_INTERPOLATE_SPV`)

- [ ] **Step 1: Add SPIR-V header verification test in `tests/UpscalerFrameGenTests.cpp` and wire headers into `upscaler_frame_gen_tests` in `CMakeLists.txt`**

Add `add_gpu_blit_shader` entries in `CMakeLists.txt`:
```cmake
add_gpu_blit_shader(gpu_upscale_fsr_easu gpu_upscale_fsr_easu.frag)
add_gpu_blit_shader(gpu_upscale_fsr_rcas gpu_upscale_fsr_rcas.frag)
add_gpu_blit_shader(gpu_upscale_dlss_tsr gpu_upscale_dlss_tsr.frag)
add_gpu_blit_shader(gpu_frame_gen_interpolate gpu_frame_gen_interpolate.frag)
```
Append `${gpu_blit_shader_headers}` to `upscaler_frame_gen_tests` sources in `CMakeLists.txt`, and add `TestEmbeddedSpirvShaders()` in `tests/UpscalerFrameGenTests.cpp`:
```cpp
void TestEmbeddedSpirvShaders() {
	constexpr uint32_t kSpirvMagic = 0x07230203u;
	Check(sizeof(GPU_UPSCALE_FSR_EASU_SPV) > 64 && GPU_UPSCALE_FSR_EASU_SPV[0] == kSpirvMagic, "FSR EASU SPIR-V valid");
	Check(sizeof(GPU_UPSCALE_FSR_RCAS_SPV) > 64 && GPU_UPSCALE_FSR_RCAS_SPV[0] == kSpirvMagic, "FSR RCAS SPIR-V valid");
	Check(sizeof(GPU_UPSCALE_DLSS_TSR_SPV) > 64 && GPU_UPSCALE_DLSS_TSR_SPV[0] == kSpirvMagic, "DLSS TSR SPIR-V valid");
	Check(sizeof(GPU_FRAME_GEN_INTERPOLATE_SPV) > 64 && GPU_FRAME_GEN_INTERPOLATE_SPV[0] == kSpirvMagic, "FrameGen SPIR-V valid");
}
```

- [ ] **Step 2: Run build to verify it fails on missing shader files**

Run: `ninja -C _Build upscaler_frame_gen_tests`
Expected: FAIL with missing `gpu_upscale_fsr_easu.frag`

- [ ] **Step 3: Implement the four GLSL 450 shaders in `src/graphics/host_gpu/shaders/`**

1. `gpu_upscale_fsr_easu.frag`:
   - Descriptor `set = 0, binding = 0`: `uniform sampler2D src_tex;`
   - Push constants (`16` bytes): `vec2 inv_src_size; vec2 inv_dst_size;`
   - Implements 12-tap Edge-Adaptive Spatial Upsampling (EASU): samples the $4\times4$ neighborhood around `in_uv`, computes local luma gradients (`dot(rgb, vec3(0.299, 0.587, 0.114))`), determines edge direction vector `dir` and anisotropy length `len`, evaluates approximated Lanczos2 weights for the 12 taps, and clamps output to the neighborhood min/max ring-suppression box while preserving alpha.
2. `gpu_upscale_fsr_rcas.frag`:
   - Descriptor `set = 0, binding = 0`: `uniform sampler2D src_tex;`
   - Push constants (`16` bytes): `vec2 inv_size; float sharpness; float pad;`
   - Implements 5-tap cross neighborhood Robust Contrast-Adaptive Sharpening (RCAS): fetches center `e` and 4 neighbors (`b`, `d`, `f`, `h`), computes local contrast adaptive lobe weight bounded by `sharpness` ($0.0\text{--}1.0$) and local min/max headroom so neither SDR `[0,1]` nor HDR10 values clip or ring.
3. `gpu_upscale_dlss_tsr.frag`:
   - Descriptor `set = 0, binding = 0`: `uniform sampler2D curr_tex;`
   - Descriptor `set = 0, binding = 1`: `uniform sampler2D history_tex;`
   - Descriptor `set = 0, binding = 2`: `uniform sampler2D prev_input_tex;`
   - Push constants (`24` bytes): `vec2 inv_src_size; vec2 inv_dst_size; float history_weight; float reset_history;`
   - Performs 5-tap Catmull-Rom reconstruction of `curr_tex`, estimates screen-space motion vector by searching a $3\times3$ neighborhood between `curr_tex` and `prev_input_tex` in luma space, samples `history_tex` at `in_uv - best_motion`, converts the $3\times3$ current neighborhood to YCoCg, computes mean $\mu$ and standard deviation $\sigma$ with $\gamma = 1.25$, clamps the reprojected history in YCoCg space, converts back to RGB, and blends with `curr` using confidence-weighted `history_weight` (or `0.0` when `reset_history > 0.5` or scene-cut luma error is high).
4. `gpu_frame_gen_interpolate.frag`:
   - Descriptor `set = 0, binding = 0`: `uniform sampler2D prev_tex;`
   - Descriptor `set = 0, binding = 1`: `uniform sampler2D curr_tex;`
   - Push constants (`16` bytes): `vec2 inv_size; float blend_alpha; float scene_cut_threshold;`
   - Performs coarse-to-fine ($5\times5$ strided + $3\times3$ local) symmetric block matching between `prev_tex` ($F_{t-1}$) and `curr_tex` ($F_t$) around `in_uv` to estimate motion vector $v$. Evaluates bidirectional samples $F_{t-1}(\text{uv} - 0.5v)$ and $F_t(\text{uv} + 0.5v)$, measures photometric discrepancy and local variance, and smoothly attenuates the interpolation weight toward `curr_tex` ($F_t$) whenever discrepancy exceeds `scene_cut_threshold` so disocclusions and camera cuts never ghost or tear.

- [ ] **Step 4: Build and run `upscaler_frame_gen_tests` to verify all shaders compile and pass**

Run: `ninja -C _Build upscaler_frame_gen_tests && .\_Build\upscaler_frame_gen_tests.exe`
Expected: PASS (`glslangValidator` compiles all 4 shaders without warnings/errors and `TestEmbeddedSpirvShaders` passes)

- [ ] **Step 5: Commit Task 2**

```bash
git add CMakeLists.txt src/graphics/host_gpu/shaders/gpu_upscale_fsr_easu.frag src/graphics/host_gpu/shaders/gpu_upscale_fsr_rcas.frag src/graphics/host_gpu/shaders/gpu_upscale_dlss_tsr.frag src/graphics/host_gpu/shaders/gpu_frame_gen_interpolate.frag tests/UpscalerFrameGenTests.cpp
git commit -m "feat(shaders): add Vulkan 1.3 FSR EASU/RCAS, DLSS TSR, and Frame Generation shaders"
```

---

### Task 3: Vulkan UpscalerPass, FrameGenPass, Swapchain/Presenter Integration, and Hotkeys

**Files:**
- Create: `src/graphics/presentation/window/upscaler.h`
- Create: `src/graphics/presentation/window/upscaler.cpp`
- Create: `src/graphics/presentation/window/frameGen.h`
- Create: `src/graphics/presentation/window/frameGen.cpp`
- Modify: `src/graphics/presentation/window/swapchain.cpp:1-920`
- Modify: `src/graphics/presentation/window/windowInternal.h:1-60`
- Modify: `src/graphics/presentation/window/window.cpp:175-210, 975-1015`
- Modify: `CMakeLists.txt:550-610`
- Modify: `tests/UpscalerFrameGenTests.cpp`

**Interfaces:**
- Consumes: `Config` API from Task 1, SPIR-V headers from Task 2, `HostGpu::CommandBuffer`, `Presenter::Frame`
- Produces:
  ```cpp
  namespace Window {

  // Pure helper testable without a live VkDevice:
  float ComputeFrameGenBlendWeight(float luma_diff, float scene_cut_threshold, bool history_valid) noexcept;

  class UpscalerPass {
  public:
      UpscalerPass(vk::Device device, vk::PhysicalDevice physical_device);
      ~UpscalerPass();
      void InvalidateHistory() noexcept;
      bool IsNativeNgxAvailable() const noexcept;
      void Record(HostGpu::CommandBuffer& command,
                  vk::ImageView src_view, vk::Extent2D src_extent,
                  vk::Image dst_image, vk::ImageView dst_view, vk::Extent2D dst_extent,
                  vk::Format dst_format, Config::UpscalerType mode, uint32_t sharpness_percent);
  };

  class FrameGenPass {
  public:
      FrameGenPass(vk::Device device, vk::PhysicalDevice physical_device);
      ~FrameGenPass();
      void Reset() noexcept;
      bool HasValidHistory(vk::Extent2D extent, vk::Format format) const noexcept;
      // Synthesizes midpoint frame from prev_frame and curr_image into internal interpolated image:
      vk::ImageView SynthesizeMidpoint(HostGpu::CommandBuffer& command,
                                       vk::Image curr_image, vk::ImageView curr_view,
                                       vk::Extent2D extent, vk::Format format);
      vk::Image GetInterpolatedImage() const noexcept;
      // Copies curr_image into internal prev_frame history for the next flip:
      void UpdateHistory(HostGpu::CommandBuffer& command,
                         vk::Image curr_image, vk::Extent2D extent, vk::Format format);
  };

  } // namespace Window
  ```

- [ ] **Step 1: Write failing unit tests for `ComputeFrameGenBlendWeight` and scene-cut/resize reset logic in `tests/UpscalerFrameGenTests.cpp`**

Add tests in `tests/UpscalerFrameGenTests.cpp`:
```cpp
void TestFrameGenSceneCutAndResizeReset() {
	Check(Window::ComputeFrameGenBlendWeight(0.02f, 0.35f, false) == 0.0f, "invalid history -> 0.0 blend");
	Check(Window::ComputeFrameGenBlendWeight(0.01f, 0.35f, true) == 0.5f, "low luma diff -> 0.5 midpoint blend");
	Check(Window::ComputeFrameGenBlendWeight(0.50f, 0.35f, true) == 0.0f, "scene cut above threshold -> 0.0 fallback");
	float transition = Window::ComputeFrameGenBlendWeight(0.28f, 0.35f, true);
	Check(transition > 0.0f && transition < 0.5f, "near-threshold smoothly attenuates blend weight");
}
```

- [ ] **Step 2: Run build to verify it fails**

Run: `ninja -C _Build upscaler_frame_gen_tests`
Expected: FAIL with missing `graphics/presentation/window/frameGen.h`

- [ ] **Step 3: Implement `UpscalerPass`, `FrameGenPass`, `Swapchain` integration, and `F9`/`F10` hotkeys**

1. Create `src/graphics/presentation/window/upscaler.h` and `src/graphics/presentation/window/upscaler.cpp`:
   - On Windows when `physical_device` has `vendorID == 0x10de` (NVIDIA), perform a one-time non-blocking runtime probe for `nvngx_dlss.dll` / `_nvngx.dll` via `LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)` and log detection status; always keep the universal Vulkan 1.3 TSR (`GPU_UPSCALE_DLSS_TSR_SPV`) + RCAS (`GPU_UPSCALE_FSR_RCAS_SPV`) and FSR EASU (`GPU_UPSCALE_FSR_EASU_SPV`) + RCAS pipelines ready on all NVIDIA, AMD, and Intel GPUs.
   - Manage intermediate upscaled image (`m_intermediate_image`), TSR history image (`m_history_image`), and previous low-res input image (`m_prev_input_image`) allocated via `Renderer::GetInstance().GetDeviceMemory()` (or `vk::Device` memory allocation matching `swapchain.cpp`), automatically recreating them when `dst_extent`, `src_extent`, or `dst_format` changes.
   - Record EASU -> RCAS for `UpscalerType::Fsr`, and TSR -> History Copy -> RCAS for `UpscalerType::Dlss`, using `vkCmdBeginRendering`, `vkCmdPushDescriptorSetKHR`, and `vkCmdPushConstants`.
2. Create `src/graphics/presentation/window/frameGen.h` and `src/graphics/presentation/window/frameGen.cpp`:
   - Implement `ComputeFrameGenBlendWeight(float luma_diff, float scene_cut_threshold, bool history_valid) noexcept`.
   - Manage `m_prev_image` and `m_interp_image` (device-local `eSampled | eTransferDst | eTransferSrc | eColorAttachment`).
   - `SynthesizeMidpoint` renders `GPU_FRAME_GEN_INTERPOLATE_SPV` into `m_interp_image` using `m_prev_view` and `curr_view`.
   - `UpdateHistory` copies `curr_image` to `m_prev_image` via `vkCmdCopyImage` with proper `synchronization2` barriers.
3. Update `src/graphics/presentation/window/swapchain.cpp`:
   - Own `std::unique_ptr<UpscalerPass> m_upscaler;` and `std::unique_ptr<FrameGenPass> m_frame_gen;` in `Swapchain`.
   - In `Swapchain::RecordPresentCommands`: if `Config::GetUpscalerType() != Config::UpscalerType::Off`, invoke `m_upscaler->Record(...)` to upscale the main layer (`source` or synthesized midpoint image) into `m_images[m_image_index]`; otherwise keep the existing `m_filter` / `vkCmdBlitImage` path. Then composite `overlay` (`layers[1]`) and `m_system_overlay` *after* upscaling so UI and overlays are never warped.
   - In `Presenter::Impl::Present`: when `Config::FrameGenerationEnabled()` is true and `m_swapchain->CanGenerateFrame(source)` is true (valid previous frame with matching extent and format, and `source->image != last_presented_image` or new flip):
     1. Acquire a swapchain image and record `SynthesizeMidpoint` + `UpscalerPass` (if enabled) + `overlay` (`layers[1]`) + `SystemOverlay`, submit, and present the generated midpoint frame (incrementing `presented_frames`).
     2. Record `UpdateHistory` with `source->image`, then acquire the next swapchain image and present the real frame `source` with `UpscalerPass` (if enabled) + `overlay` + `SystemOverlay` (incrementing `presented_frames`).
     - When `Config::FrameGenerationEnabled()` is toggled off or swapchain is recreated, call `m_frame_gen->Reset()`.
4. Update `src/graphics/presentation/window/window.cpp`:
   - In `ProcessEvents` (`SDL_KEYDOWN` with `event.key.repeat == 0`):
     - `SDLK_F9`: call `Config::CycleUpscalerType()` and log the new mode.
     - `SDLK_F10`: call `Config::ToggleFrameGeneration()` and log the new state.
   - In `WindowContext::UpdateTitle`: append `Config::FormatUpscalerStatusTag()` to the window title when non-empty.
5. Add `upscaler.cpp` and `frameGen.cpp` to `kyty_emulator_src` in `CMakeLists.txt`.

- [ ] **Step 4: Build `upscaler_frame_gen_tests` and `kyty_emulator` to verify everything compiles and tests pass**

Run: `ninja -C _Build upscaler_frame_gen_tests kyty_emulator && .\_Build\upscaler_frame_gen_tests.exe`
Expected: PASS (`upscaler_frame_gen_tests` passes and `kyty_emulator.exe` links cleanly)

- [ ] **Step 5: Commit Task 3**

```bash
git add CMakeLists.txt src/graphics/presentation/window/upscaler.h src/graphics/presentation/window/upscaler.cpp src/graphics/presentation/window/frameGen.h src/graphics/presentation/window/frameGen.cpp src/graphics/presentation/window/swapchain.cpp src/graphics/presentation/window/windowInternal.h src/graphics/presentation/window/window.cpp tests/UpscalerFrameGenTests.cpp
git commit -m "feat(presentation): integrate Vulkan FSR/DLSS upscaler, frame generation, and F9/F10 hotkeys"
```

---

### Task 4: Qt6 Launcher GUI Controls, Per-Game Settings Persistence, and CLI Argument Forwarding

**Files:**
- Modify: `src/launcher/include/configuration.h:60-185`
- Modify: `src/launcher/src/configuration.cpp:1-740`
- Modify: `src/launcher/forms/configuration_edit_dialog.ui:230-310`
- Modify: `src/launcher/src/configurationEditDialog.cpp:40-260`
- Modify: `src/launcher/src/mainDialog.cpp:580-660`

**Interfaces:**
- Consumes: CLI flags `--upscaler`, `--upscale-quality`, `--upscale-sharpness`, `--frame-gen` from Task 1
- Produces:
  - `Configuration::UpscalerType` (`Off`, `Fsr`, `Dlss`)
  - `Configuration::UpscaleQuality` (`NativeAA`, `Quality`, `Balanced`, `Performance`, `UltraPerformance`)
  - `Configuration` fields: `upscaler_type`, `upscale_quality`, `upscale_sharpness`, `frame_generation_enabled`
  - Qt widgets in `ConfigurationEditDialog`: `comboBox_upscaler`, `comboBox_upscale_quality`, `horizontalSlider_upscale_sharpness`, `checkBox_frame_generation`

- [ ] **Step 1: Add `Configuration` enums, fields, serialization, and string helpers in `src/launcher/include/configuration.h` and `src/launcher/src/configuration.cpp`**

1. In `src/launcher/include/configuration.h`:
   - Define `enum class UpscalerType { Off, Fsr, Dlss };` and `enum class UpscaleQuality { NativeAA, Quality, Balanced, Performance, UltraPerformance };`.
   - Add member fields to `Configuration`:
     ```cpp
     UpscalerType upscaler_type = UpscalerType::Off;
     UpscaleQuality upscale_quality = UpscaleQuality::Quality;
     int upscale_sharpness = 50;
     bool frame_generation_enabled = false;
     ```
   - Declare `UpscalerTypeToStr`, `StrToUpscalerType`, `UpscaleQualityToStr`, `StrToUpscaleQuality`.
2. In `src/launcher/src/configuration.cpp`:
   - Implement the string conversions with safe fallback defaults (`Off` and `Quality`) when reading legacy configs that lack these keys.
   - Persist `upscaler_type`, `upscale_quality`, `upscale_sharpness` (clamped `0..100`), and `frame_generation_enabled` in `SaveSettings`, `LoadSettings`, `GameSettings`, and `ReadGameSettingsValues`.

- [ ] **Step 2: Add UI controls to `src/launcher/forms/configuration_edit_dialog.ui` and bind them in `src/launcher/src/configurationEditDialog.cpp`**

1. In `src/launcher/forms/configuration_edit_dialog.ui` inside `graphicsLayout`:
   - Add `comboBox_upscaler` (`Off`, `AMD FSR (FidelityFX Super Resolution)`, `NVIDIA DLSS / Temporal Super Resolution`).
   - Add `comboBox_upscale_quality` (`Native AA (1.0x)`, `Quality (1.5x)`, `Balanced (1.7x)`, `Performance (2.0x)`, `Ultra Performance (3.0x)`).
   - Add `horizontalSlider_upscale_sharpness` (`0..100`, default `50`) with value label.
   - Add `checkBox_frame_generation` (`Enable Frame Generation (smooth FPS interpolation)`).
2. In `src/launcher/src/configurationEditDialog.cpp`:
   - Populate and synchronize the new controls in `SetConfiguration` and `GetConfiguration`, and enable/disable quality & sharpness controls based on whether `comboBox_upscaler` is active (or keep them editable at all times with clear tooltips).

- [ ] **Step 3: Forward `--upscaler`, `--upscale-quality`, `--upscale-sharpness`, and `--frame-gen` in `src/launcher/src/mainDialog.cpp`**

In `CreateEmulatorArgs(const Configuration& cfg)` in `src/launcher/src/mainDialog.cpp`:
- When `cfg.upscaler_type != Configuration::UpscalerType::Off`:
  - Append `"--upscaler"` and `Configuration::UpscalerTypeToStr(cfg.upscaler_type)`.
  - Append `"--upscale-quality"` and `Configuration::UpscaleQualityToStr(cfg.upscale_quality)`.
  - Append `"--upscale-sharpness"` and `QString::number(std::clamp(cfg.upscale_sharpness, 0, 100))`.
- When `cfg.frame_generation_enabled`:
  - Append `"--frame-gen"` and `"on"`.

- [ ] **Step 4: Build `kyty_launcher` and `kyty_emulator` and run `upscaler_frame_gen_tests`**

Run: `ninja -C _Build kyty_launcher kyty_emulator upscaler_frame_gen_tests && .\_Build\upscaler_frame_gen_tests.exe`
Expected: PASS (`kyty_launcher.exe`, `kyty_emulator.exe`, and `upscaler_frame_gen_tests.exe` build and pass with zero errors)

- [ ] **Step 5: Commit Task 4**

```bash
git add src/launcher/include/configuration.h src/launcher/src/configuration.cpp src/launcher/forms/configuration_edit_dialog.ui src/launcher/src/configurationEditDialog.cpp src/launcher/src/mainDialog.cpp
git commit -m "feat(launcher): add DLSS/FSR upscaling and frame generation settings to Qt6 Launcher"
```
