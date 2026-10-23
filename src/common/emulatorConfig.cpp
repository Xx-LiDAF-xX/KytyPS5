#include "common/emulatorConfig.h"

#include "common/assert.h"
#include "common/liveSwitch.h"

#include <algorithm>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fmt/format.h>
#include <memory>

#include <algorithm>
#include <memory>

namespace Config {

static std::unique_ptr<ConfigOptions> g_config;

static std::atomic<uint8_t>  g_runtime_upscaler_type {static_cast<uint8_t>(UpscalerType::Off)};
static std::atomic<uint8_t>  g_runtime_upscale_quality {static_cast<uint8_t>(UpscaleQuality::Quality)};
static std::atomic<uint32_t> g_runtime_upscale_sharpness {50u};
static std::atomic<bool>     g_runtime_frame_generation {false};

static std::string NormalizeToken(std::string_view str) {
	std::string out;
	out.reserve(str.size());
	for (char ch : str) {
		const auto uch = static_cast<unsigned char>(ch);
		if (std::isspace(uch) != 0) {
			continue;
		}
		if (ch == '-') {
			out.push_back('_');
		} else {
			out.push_back(static_cast<char>(std::tolower(uch)));
		}
	}
	return out;
}

static Live::Switch g_live_upscaler(
    "KYTY_UPSCALER",
    [](const char* value) -> int64_t {
	    if (value == nullptr || *value == '\0') {
		    return -1;
	    }
	    return static_cast<int64_t>(ParseUpscalerType(value));
    },
    [](int64_t /*previous*/, int64_t value) {
	    if (value >= 0) {
		    g_runtime_upscaler_type.store(static_cast<uint8_t>(value), std::memory_order_relaxed);
	    }
    });

static Live::Switch g_live_upscale_quality(
    "KYTY_UPSCALE_QUALITY",
    [](const char* value) -> int64_t {
	    if (value == nullptr || *value == '\0') {
		    return -1;
	    }
	    return static_cast<int64_t>(ParseUpscaleQuality(value));
    },
    [](int64_t /*previous*/, int64_t value) {
	    if (value >= 0) {
		    g_runtime_upscale_quality.store(static_cast<uint8_t>(value), std::memory_order_relaxed);
	    }
    });

static Live::Switch g_live_upscale_sharpness(
    "KYTY_UPSCALE_SHARPNESS",
    [](const char* value) -> int64_t {
	    if (value == nullptr || *value == '\0') {
		    return -1;
	    }
	    char* end = nullptr;
	    long  v   = std::strtol(value, &end, 10);
	    if (end == value) {
		    return -1;
	    }
	    return std::clamp<int64_t>(v, 0, 100);
    },
    [](int64_t /*previous*/, int64_t value) {
	    if (value >= 0) {
		    g_runtime_upscale_sharpness.store(static_cast<uint32_t>(value), std::memory_order_relaxed);
	    }
    });

static Live::Switch g_live_frame_gen(
    "KYTY_FRAME_GEN",
    [](const char* value) -> int64_t {
	    if (value == nullptr || *value == '\0') {
		    return -1;
	    }
	    std::string norm = NormalizeToken(value);
	    if (norm == "1" || norm == "true" || norm == "on" || norm == "yes") {
		    return 1;
	    }
	    if (norm == "0" || norm == "false" || norm == "off" || norm == "no") {
		    return 0;
	    }
	    return -1;
    },
    [](int64_t /*previous*/, int64_t value) {
	    if (value >= 0) {
		    g_runtime_frame_generation.store(value != 0, std::memory_order_relaxed);
	    }
    });

static void SyncRuntimeUpscalerStateFromConfig(const ConfigOptions& cfg) {
	g_runtime_upscaler_type.store(
	    static_cast<uint8_t>(g_live_upscaler.Get() >= 0
	                             ? static_cast<UpscalerType>(g_live_upscaler.Get())
	                             : cfg.upscaler_type),
	    std::memory_order_relaxed);
	g_runtime_upscale_quality.store(
	    static_cast<uint8_t>(g_live_upscale_quality.Get() >= 0
	                             ? static_cast<UpscaleQuality>(g_live_upscale_quality.Get())
	                             : cfg.upscale_quality),
	    std::memory_order_relaxed);
	g_runtime_upscale_sharpness.store(
	    g_live_upscale_sharpness.Get() >= 0
	        ? static_cast<uint32_t>(g_live_upscale_sharpness.Get())
	        : std::min(cfg.upscale_sharpness, 100u),
	    std::memory_order_relaxed);
	g_runtime_frame_generation.store(
	    g_live_frame_gen.Get() >= 0 ? (g_live_frame_gen.Get() != 0) : cfg.frame_generation_enabled,
	    std::memory_order_relaxed);
}

void Initialize() {
	EXIT_IF(g_config != nullptr);

	g_config = std::make_unique<ConfigOptions>();
	SyncRuntimeUpscalerStateFromConfig(*g_config);
}

void Shutdown() {
	g_config.reset();
}

void Load(const ConfigOptions& cfg) {
	EXIT_IF(g_config == nullptr);
	EXIT_IF(cfg.user_name.empty() || cfg.user_name.size() > MAX_USER_NAME_LENGTH);
	EXIT_IF(!IsConfiguredUserIdValid(cfg.user_id));
	EXIT_IF(cfg.controller_speaker_volume > 100 || cfg.controller_vibration_intensity > 100);
	EXIT_IF(cfg.audio_master_volume > MAX_AUDIO_VOLUME || cfg.audio_main_volume > MAX_AUDIO_VOLUME ||
	        cfg.audio_music_volume > MAX_AUDIO_VOLUME ||
	        cfg.audio_pad_speaker_main_volume > MAX_AUDIO_VOLUME ||
	        cfg.audio_objects_volume > MAX_AUDIO_VOLUME);

	*g_config = cfg;
	Detail::g_graphics_debug_dump_enabled = cfg.graphics_debug_dump_enabled;
	Detail::g_sync_raw_image_buffers      = cfg.sync_raw_image_buffers;
	g_config->upscale_sharpness           = std::min(cfg.upscale_sharpness, 100u);
	SyncRuntimeUpscalerStateFromConfig(*g_config);
}

uint32_t GetScreenWidth() {
	return g_config->screen_width;
}

uint32_t GetScreenHeight() {
	return g_config->screen_height;
}

const std::string& GetUserName() {
	return g_config->user_name;
}

int32_t GetUserId() {
	return g_config->user_id;
}

const std::string& GetAudioInputDevice() {
	return g_config->audio_input_device;
}

const std::optional<ControllerColor>& GetControllerColor() {
	return g_config->controller_color;
}

uint32_t GetControllerSpeakerVolume() {
	return g_config->controller_speaker_volume;
}

uint32_t GetControllerVibrationIntensity() {
	return g_config->controller_vibration_intensity;
}

uint32_t GetAudioMasterVolume() {
	return g_config->audio_master_volume;
}

uint32_t GetAudioMainVolume() {
	return g_config->audio_main_volume;
}

uint32_t GetAudioMusicVolume() {
	return g_config->audio_music_volume;
}

uint32_t GetAudioPadSpeakerOnMainVolume() {
	return g_config->audio_pad_speaker_main_volume;
}

uint32_t GetAudioObjectsVolume() {
	return g_config->audio_objects_volume;
}

bool AudioObjectsEnabled() {
	return g_config->audio_objects_enabled;
}

UpscalerType GetUpscalerType() {
	return static_cast<UpscalerType>(g_runtime_upscaler_type.load(std::memory_order_relaxed));
}

void SetUpscalerType(UpscalerType type) {
	g_runtime_upscaler_type.store(static_cast<uint8_t>(type), std::memory_order_relaxed);
	if (g_config != nullptr) {
		g_config->upscaler_type = type;
	}
}

UpscalerType CycleUpscalerType() {
	UpscalerType next = UpscalerType::Off;
	switch (GetUpscalerType()) {
	case UpscalerType::Off:
		next = UpscalerType::Fsr;
		break;
	case UpscalerType::Fsr:
		next = UpscalerType::Dlss;
		break;
	case UpscalerType::Dlss:
	default:
		next = UpscalerType::Off;
		break;
	}
	SetUpscalerType(next);
	return next;
}

std::string_view UpscalerTypeToString(UpscalerType type) noexcept {
	switch (type) {
	case UpscalerType::Fsr:
		return "FSR";
	case UpscalerType::Dlss:
		return "DLSS";
	case UpscalerType::Off:
	default:
		return "Off";
	}
}

UpscalerType ParseUpscalerType(std::string_view str) noexcept {
	const std::string norm = NormalizeToken(str);
	if (norm == "fsr" || norm == "fidelityfx" || norm == "1") {
		return UpscalerType::Fsr;
	}
	if (norm == "dlss" || norm == "tsr" || norm == "2") {
		return UpscalerType::Dlss;
	}
	return UpscalerType::Off;
}

UpscaleQuality GetUpscaleQuality() {
	return static_cast<UpscaleQuality>(g_runtime_upscale_quality.load(std::memory_order_relaxed));
}

void SetUpscaleQuality(UpscaleQuality quality) {
	g_runtime_upscale_quality.store(static_cast<uint8_t>(quality), std::memory_order_relaxed);
	if (g_config != nullptr) {
		g_config->upscale_quality = quality;
	}
}

std::string_view UpscaleQualityToString(UpscaleQuality quality) noexcept {
	switch (quality) {
	case UpscaleQuality::NativeAA:
		return "NativeAA";
	case UpscaleQuality::Quality:
		return "Quality";
	case UpscaleQuality::Balanced:
		return "Balanced";
	case UpscaleQuality::Performance:
		return "Performance";
	case UpscaleQuality::UltraPerformance:
		return "UltraPerformance";
	default:
		return "Quality";
	}
}

UpscaleQuality ParseUpscaleQuality(std::string_view str) noexcept {
	const std::string norm = NormalizeToken(str);
	if (norm == "native" || norm == "nativeaa" || norm == "native_aa" || norm == "dlaa") {
		return UpscaleQuality::NativeAA;
	}
	if (norm == "quality") {
		return UpscaleQuality::Quality;
	}
	if (norm == "balanced") {
		return UpscaleQuality::Balanced;
	}
	if (norm == "performance" || norm == "perf") {
		return UpscaleQuality::Performance;
	}
	if (norm == "ultra_performance" || norm == "ultraperformance" || norm == "ultra") {
		return UpscaleQuality::UltraPerformance;
	}
	return UpscaleQuality::Quality;
}

uint32_t GetUpscaleSharpness() {
	return g_runtime_upscale_sharpness.load(std::memory_order_relaxed);
}

void SetUpscaleSharpness(uint32_t sharpness) {
	const uint32_t clamped = std::min(sharpness, 100u);
	g_runtime_upscale_sharpness.store(clamped, std::memory_order_relaxed);
	if (g_config != nullptr) {
		g_config->upscale_sharpness = clamped;
	}
}

bool FrameGenerationEnabled() {
	return g_runtime_frame_generation.load(std::memory_order_relaxed);
}

void SetFrameGenerationEnabled(bool enabled) {
	g_runtime_frame_generation.store(enabled, std::memory_order_relaxed);
	if (g_config != nullptr) {
		g_config->frame_generation_enabled = enabled;
	}
}

bool ToggleFrameGeneration() {
	const bool next = !FrameGenerationEnabled();
	SetFrameGenerationEnabled(next);
	return next;
}

float GetUpscaleFactor(UpscaleQuality quality) noexcept {
	switch (quality) {
	case UpscaleQuality::NativeAA:
		return 1.0f;
	case UpscaleQuality::Quality:
		return 1.5f;
	case UpscaleQuality::Balanced:
		return 1.7f;
	case UpscaleQuality::Performance:
		return 2.0f;
	case UpscaleQuality::UltraPerformance:
		return 3.0f;
	default:
		return 1.5f;
	}
}

void GetInternalRenderResolution(uint32_t display_width, uint32_t display_height,
                                 uint32_t& out_width, uint32_t& out_height) noexcept {
	const uint32_t safe_w = std::max(2u, (display_width + 1u) & ~1u);
	const uint32_t safe_h = std::max(2u, (display_height + 1u) & ~1u);
	if (GetUpscalerType() == UpscalerType::Off) {
		out_width  = safe_w;
		out_height = safe_h;
		return;
	}
	const float factor = GetUpscaleFactor(GetUpscaleQuality());
	if (factor <= 1.0f) {
		out_width  = safe_w;
		out_height = safe_h;
		return;
	}
	const auto rw = static_cast<uint32_t>(
	    std::lround(static_cast<double>(safe_w) / static_cast<double>(factor)));
	const auto rh = static_cast<uint32_t>(
	    std::lround(static_cast<double>(safe_h) / static_cast<double>(factor)));
	out_width  = std::max(2u, (rw + 1u) & ~1u);
	out_height = std::max(2u, (rh + 1u) & ~1u);
}

std::string FormatUpscalerStatusTag() {
	const UpscalerType   upscaler = GetUpscalerType();
	const UpscaleQuality quality  = GetUpscaleQuality();
	const bool           fg       = FrameGenerationEnabled();
	if (upscaler == UpscalerType::Off && !fg) {
		return {};
	}
	if (upscaler != UpscalerType::Off && fg) {
		return fmt::format("[{} {} | FG]", UpscalerTypeToString(upscaler),
		                   UpscaleQualityToString(quality));
	}
	if (upscaler != UpscalerType::Off) {
		return fmt::format("[{} {}]", UpscalerTypeToString(upscaler),
		                   UpscaleQualityToString(quality));
	}
	return "[FG]";
}

PresentMode GetPresentMode() {
	return g_config->present_mode;
}

int32_t GetGpuIndex() {
	return g_config->gpu_index;
}

bool FullscreenEnabled() {
	return g_config->fullscreen_enabled;
}

bool HdrEnabled() {
	return g_config->hdr_enabled;
}

bool HideCursorEnabled() {
	return g_config->hide_cursor_enabled;
}

bool VrEnabled() {
	return g_config->vr_enabled;
}

bool AmdCpuEnabled() {
	return g_config->amd_cpu_enabled;
}

const std::string& GetAmdCpuDecision() {
	return g_config->amd_cpu_decision;
}

uint32_t GetVblankFrequency() {
	return std::clamp(g_config->vblank_frequency, 30u, 360u);
}

uint32_t GetConsoleLanguage() {
	return g_config->console_language;
}

bool VulkanValidationEnabled() {
	return g_config->vulkan_validation_enabled;
}

bool IsNetworkConnected() {
	return g_config->network_connected;
}

bool ShaderValidationEnabled() {
	return g_config->shader_validation_enabled;
}

ShaderOptimizationType GetShaderOptimizationType() {
	return g_config->shader_optimization_type;
}

LogDirection GetShaderLogDirection() {
	return g_config->shader_log_direction;
}

std::filesystem::path GetShaderLogFolder() {
	return g_config->shader_log_folder;
}

bool CommandBufferDumpEnabled() {
	return g_config->command_buffer_dump_enabled;
}

std::filesystem::path GetCommandBufferDumpFolder() {
	return g_config->command_buffer_dump_folder;
}

LogDirection GetPrintfDirection() {
	return g_config->printf_direction;
}

std::filesystem::path GetPrintfOutputFile() {
	return g_config->printf_output_file;
}

bool ProfilerEnabled() {
	return g_config->profiler_enabled;
}

bool SpirvDebugPrintfEnabled() {
	return g_config->spirv_debug_printf_enabled;
}

bool GpuAssistedValidationEnabled() {
	return g_config->gpu_assisted_validation_enabled && g_config->vulkan_validation_enabled;
}

bool RenderDocEnabled() {
	return g_config->renderdoc_enabled;
}

bool ReadbackLinearImagesEnabled() {
	return g_config->readback_linear_images;
}

bool TessellationEnabled() {
	return g_config->tessellation_enabled;
}

bool TrophyEnabled() {
	return g_config->trophy_enabled;
}

uint32_t GetTrophyDurationSeconds() {
	return std::clamp(g_config->trophy_duration_seconds, 5u, 10u);
}

bool PlayGoHackEnabled() {
	return g_config->playgo_hack_enabled;
}

bool SkipNoticeScreen() {
	return g_config->skip_notice_screen;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled() {
	return g_config->red_zone_protection_enabled;
}
#endif

const Keymap& GetKeymap() {
	return g_config->keymap;
}

const std::vector<std::string>& GetGuestArguments() {
	return g_config->guest_arguments;
}

} // namespace Config
