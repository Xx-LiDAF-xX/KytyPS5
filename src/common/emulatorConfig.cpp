#include "common/emulatorConfig.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/settingsFile.h"
#include "common/systemInfo.h"
#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef GetUserName
#endif

namespace Config {

static std::unique_ptr<ConfigOptions> g_config;

void Initialize() {
	EXIT_IF(g_config != nullptr);

	g_config = std::make_unique<ConfigOptions>();
}

void Shutdown() {
	g_config.reset();
}

void Load(const ConfigOptions& cfg) {
	EXIT_IF(g_config == nullptr);
	EXIT_IF(cfg.user_name.empty() || cfg.user_name.size() > MAX_USER_NAME_LENGTH);
	EXIT_IF(!IsConfiguredUserIdValid(cfg.user_id));

	*g_config = cfg;
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

PresentMode GetPresentMode() {
	return g_config->present_mode;
}

BdaSyncMode GetBdaSyncMode() {
	return g_config->bda_sync_mode;
}

int32_t GetGpuIndex() {
	return g_config->gpu_index;
}

bool FullscreenEnabled() {
	return g_config->fullscreen_enabled;
}

bool VrEnabled() {
	return g_config->vr_enabled;
}

int GetOsdMode() {
	return g_config->osd_mode;
}

int GetOsdAlignment() {
	return g_config->osd_alignment;
}

bool AmdCpuEnabled() {
	return g_config->amd_cpu_enabled;
}

uint32_t GetVblankFrequency() {
	if (g_config->vblank_frequency == 0) return 0; // Uncapped
	return g_config->vblank_frequency;
}

uint32_t GetConsoleLanguage() {
	return g_config->console_language;
}

bool VulkanValidationEnabled() {
	return g_config->vulkan_validation_enabled;
}

bool ShaderValidationEnabled() {
	return g_config->shader_validation_enabled;
}

bool ShaderPrecompileEnabled() {
	return g_config->shader_precompile_enabled;
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

bool GraphicsDebugDumpEnabled() {
	return g_config->graphics_debug_dump_enabled;
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

bool PlayGoHackEnabled() {
	return g_config->playgo_hack_enabled;
}

uint32_t GetDrainStatsInterval() {
	return g_config->drain_stats_interval;
}

bool DccGpuClearEnabled() {
	return g_config->dcc_gpu_clear_enabled;
}

bool AsyncSubmitEnabled() {
	return g_config->async_submit_enabled;
}

bool GpuMeshIndirectEnabled() {
	return g_config->gpu_mesh_indirect_enabled;
}

uint32_t GetGpuFramesAhead() {
	return g_config->gpu_frames_ahead;
}


uint32_t GetLabelFlushIntervalUs() {
	return g_config->label_flush_interval_us;
}

// A runtime change from the settings panel; 0 means the configured value.
static std::atomic<uint32_t> g_gpu_timestamp_scale_override {0};

uint32_t GetGpuTimestampScalePercent() {
	const auto percent = g_gpu_timestamp_scale_override.load(std::memory_order_relaxed);
	return percent != 0 ? percent : g_config->gpu_timestamp_scale_percent;
}

void SetGpuTimestampScalePercent(uint32_t percent) {
	g_gpu_timestamp_scale_override.store(std::clamp(percent, 100u, 200u),
	                                     std::memory_order_relaxed);
}

// A runtime change from the settings panel: -1 means the configured value.
static std::atomic<int> g_pipeline_libraries_override {-1};

bool PipelineLibrariesEnabled() {
	const auto enabled = g_pipeline_libraries_override.load(std::memory_order_relaxed);
	return enabled >= 0 ? enabled != 0 : g_config->pipeline_libraries_enabled;
}

void SetPipelineLibrariesEnabled(bool enabled) {
	g_pipeline_libraries_override.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

static std::atomic<int> g_async_pipelines_override {-1};

bool AsyncPipelinesEnabled() {
	const auto enabled = g_async_pipelines_override.load(std::memory_order_relaxed);
	return enabled >= 0 ? enabled != 0 : g_config->async_pipelines_enabled;
}

void SetAsyncPipelinesEnabled(bool enabled) {
	g_async_pipelines_override.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

static std::atomic<int> g_speculative_draws_override {-1};

bool SpeculativeDrawsEnabled() {
	const auto enabled = g_speculative_draws_override.load(std::memory_order_relaxed);
	return enabled >= 0 ? enabled != 0 : g_config->speculative_draws_enabled;
}

void SetSpeculativeDrawsEnabled(bool enabled) {
	g_speculative_draws_override.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

bool RecordThreadEnabled() {
	return g_config->record_thread_enabled;
}

bool HardwareBufferBoundsEnabled() {
	return g_config->hardware_buffer_bounds;
}

static std::atomic<int> g_relaxed_readback_override {-1};

bool RelaxedReadbackEnabled() {
	const auto enabled = g_relaxed_readback_override.load(std::memory_order_relaxed);
	return enabled >= 0 ? enabled != 0 : g_config->relaxed_readback_enabled;
}

void SetRelaxedReadbackEnabled(bool enabled) {
	g_relaxed_readback_override.store(enabled ? 1 : 0, std::memory_order_relaxed);
}


#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled() {
	return g_config->red_zone_protection_enabled;
}
#endif

static std::atomic<int> g_master_volume_override {-1};
static std::atomic<int> g_audio_muted_override {-1};
static std::atomic<int> g_anisotropic_filtering_override {-99};
static std::atomic<int> g_resolution_scale_override {-1};
static std::atomic<int> g_motion_blur_override {-1};
static std::atomic<int> g_depth_of_field_override {-1};
static std::atomic<int> g_bloom_override {-1};
static std::atomic<int> g_ambient_occlusion_override {-1};
static std::atomic<int> g_ray_tracing_override {-1};
static std::atomic<int> g_auto_spec_optimization_override {-1};

uint32_t GetMasterVolume() {
	const auto val = g_master_volume_override.load(std::memory_order_relaxed);
	return val >= 0 ? static_cast<uint32_t>(val) : (g_config ? g_config->master_volume : 100u);
}

void SetMasterVolume(uint32_t volume) {
	g_master_volume_override.store(static_cast<int>(std::clamp(volume, 0u, 100u)), std::memory_order_relaxed);
}

bool AudioMuted() {
	const auto val = g_audio_muted_override.load(std::memory_order_relaxed);
	return val >= 0 ? val != 0 : (g_config ? g_config->audio_muted : false);
}

void SetAudioMuted(bool muted) {
	g_audio_muted_override.store(muted ? 1 : 0, std::memory_order_relaxed);
}

int32_t GetAnisotropicFiltering() {
	const auto val = g_anisotropic_filtering_override.load(std::memory_order_relaxed);
	return val != -99 ? val : (g_config ? g_config->anisotropic_filtering : -1);
}

void SetAnisotropicFiltering(int32_t aniso) {
	g_anisotropic_filtering_override.store(aniso, std::memory_order_relaxed);
}

uint32_t GetResolutionScalePercent() {
	const auto val = g_resolution_scale_override.load(std::memory_order_relaxed);
	return val >= 0 ? static_cast<uint32_t>(val) : (g_config ? g_config->resolution_scale_percent : 100u);
}

void SetResolutionScalePercent(uint32_t percent) {
	g_resolution_scale_override.store(static_cast<int>(percent), std::memory_order_relaxed);
}

bool MotionBlurEnabled() {
	const auto val = g_motion_blur_override.load(std::memory_order_relaxed);
	return val >= 0 ? val != 0 : (g_config ? g_config->motion_blur_enabled : true);
}

void SetMotionBlurEnabled(bool enabled) {
	g_motion_blur_override.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

bool DepthOfFieldEnabled() {
	const auto val = g_depth_of_field_override.load(std::memory_order_relaxed);
	return val >= 0 ? val != 0 : (g_config ? g_config->depth_of_field_enabled : true);
}

void SetDepthOfFieldEnabled(bool enabled) {
	g_depth_of_field_override.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

bool BloomEnabled() {
	const auto val = g_bloom_override.load(std::memory_order_relaxed);
	return val >= 0 ? val != 0 : (g_config ? g_config->bloom_enabled : true);
}

void SetBloomEnabled(bool enabled) {
	g_bloom_override.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

bool AmbientOcclusionEnabled() {
	const auto val = g_ambient_occlusion_override.load(std::memory_order_relaxed);
	return val >= 0 ? val != 0 : (g_config ? g_config->ambient_occlusion_enabled : true);
}

void SetAmbientOcclusionEnabled(bool enabled) {
	g_ambient_occlusion_override.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

bool RayTracingEnabled() {
	const auto val = g_ray_tracing_override.load(std::memory_order_relaxed);
	return val >= 0 ? val != 0 : (g_config ? g_config->ray_tracing_enabled : false);
}

void SetRayTracingEnabled(bool enabled) {
	g_ray_tracing_override.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

bool AutoSpecOptimizationEnabled() {
	const auto val = g_auto_spec_optimization_override.load(std::memory_order_relaxed);
	return val >= 0 ? val != 0 : (g_config ? g_config->auto_spec_optimization : false);
}

void SetAutoSpecOptimizationEnabled(bool enabled) {
	g_auto_spec_optimization_override.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

void ApplyAutoOptimization(bool log_reason) {
	uint32_t cores = std::thread::hardware_concurrency();
	std::string cpu_name = Common::GetSystemInfo().ProcessorName;
	bool is_amd = false;
	std::string cpu_lower = cpu_name;
	std::transform(cpu_lower.begin(), cpu_lower.end(), cpu_lower.begin(), ::tolower);
	if (cpu_lower.find("amd") != std::string::npos || cpu_lower.find("ryzen") != std::string::npos) {
		is_amd = true;
	}
	if (g_config && is_amd) {
		g_config->amd_cpu_enabled = true;
	}

	uint32_t ram_gb = 16;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	MEMORYSTATUSEX mem_status {};
	mem_status.dwLength = sizeof(mem_status);
	if (GlobalMemoryStatusEx(&mem_status)) {
		ram_gb = static_cast<uint32_t>(mem_status.ullTotalPhys / (1024ULL * 1024ULL * 1024ULL));
	}
#endif

	// Turn Ray Tracing OFF across all games by default to avoid BVH performance bottlenecks
	SetRayTracingEnabled(false);

	const char* profile_name = "Balanced";
	if (cores >= 8 && ram_gb >= 16) {
		profile_name = "Quality";
		SetGpuTimestampScalePercent(115);
		SetAnisotropicFiltering(16);
		SetSpeculativeDrawsEnabled(true);
		SetPipelineLibrariesEnabled(true);
		SetAsyncPipelinesEnabled(false);
		SetRelaxedReadbackEnabled(false);
	} else if (cores <= 4 || ram_gb < 12) {
		profile_name = "Performance";
		SetGpuTimestampScalePercent(135);
		SetAnisotropicFiltering(4);
		SetSpeculativeDrawsEnabled(true);
		SetPipelineLibrariesEnabled(true);
		SetAsyncPipelinesEnabled(true);
		SetRelaxedReadbackEnabled(true);
	} else {
		profile_name = "Balanced";
		SetGpuTimestampScalePercent(125);
		SetAnisotropicFiltering(8);
		SetSpeculativeDrawsEnabled(true);
		SetPipelineLibrariesEnabled(true);
		SetAsyncPipelinesEnabled(true);
		SetRelaxedReadbackEnabled(true);
	}

	if (log_reason) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "[Auto-Optimizer] Analyzed PC Specs: CPU='{}' ({} threads), System RAM={} GB.\n"
		    "[Auto-Optimizer] Applied Universal Profile: '{}' (Ray Tracing=OFF, Aniso={}x, Headroom={}%, AsyncPipelines={}).\n",
		    cpu_name, cores, ram_gb, profile_name,
		    GetAnisotropicFiltering() > 0 ? std::to_string(GetAnisotropicFiltering()) : "Auto",
		    GetGpuTimestampScalePercent(), AsyncPipelinesEnabled() ? "ON" : "OFF"));
	}
}

void SaveCurrentSettings() {
	Common::SettingsFile::Save("master-volume", std::to_string(GetMasterVolume()));
	Common::SettingsFile::Save("audio-mute", AudioMuted() ? "true" : "false");
	Common::SettingsFile::Save("aniso", std::to_string(GetAnisotropicFiltering()));
	Common::SettingsFile::Save("res-scale", std::to_string(GetResolutionScalePercent()));
	Common::SettingsFile::Save("ray-tracing", RayTracingEnabled() ? "true" : "false");
	Common::SettingsFile::Save("motion-blur", MotionBlurEnabled() ? "true" : "false");
	Common::SettingsFile::Save("depth-of-field", DepthOfFieldEnabled() ? "true" : "false");
	Common::SettingsFile::Save("bloom", BloomEnabled() ? "true" : "false");
	Common::SettingsFile::Save("ambient-occlusion", AmbientOcclusionEnabled() ? "true" : "false");
	Common::SettingsFile::Save("gpu-timestamp-scale", std::to_string(GetGpuTimestampScalePercent()));
	Common::SettingsFile::Save("pipeline-libraries", PipelineLibrariesEnabled() ? "true" : "false");
	Common::SettingsFile::Save("async-pipelines", AsyncPipelinesEnabled() ? "true" : "false");
	Common::SettingsFile::Save("relaxed-readback", RelaxedReadbackEnabled() ? "true" : "false");
	Common::SettingsFile::Save("speculative-draws", SpeculativeDrawsEnabled() ? "true" : "false");
}

void ReloadFromSettingsFile() {
	std::vector<std::string> args;
	if (!Common::SettingsFile::LoadArguments(args)) {
		return;
	}

	for (size_t i = 0; i < args.size(); ++i) {
		const auto& arg = args[i];
		std::string val;
		if (i + 1 < args.size() && !args[i + 1].starts_with("--")) {
			val = args[++i];
		}
		std::string lower_val = val;
		std::transform(lower_val.begin(), lower_val.end(), lower_val.begin(), ::tolower);
		bool bool_val = (lower_val == "true" || lower_val == "1" || lower_val == "yes" || lower_val == "on");

		if (arg == "--master-volume") {
			try { SetMasterVolume(static_cast<uint32_t>(std::stoi(val))); } catch (...) {}
		} else if (arg == "--audio-mute") {
			SetAudioMuted(bool_val);
		} else if (arg == "--aniso") {
			try { SetAnisotropicFiltering(std::stoi(val)); } catch (...) {}
		} else if (arg == "--res-scale") {
			try { SetResolutionScalePercent(static_cast<uint32_t>(std::stoi(val))); } catch (...) {}
		} else if (arg == "--ray-tracing") {
			SetRayTracingEnabled(bool_val);
		} else if (arg == "--motion-blur") {
			SetMotionBlurEnabled(bool_val);
		} else if (arg == "--depth-of-field") {
			SetDepthOfFieldEnabled(bool_val);
		} else if (arg == "--bloom") {
			SetBloomEnabled(bool_val);
		} else if (arg == "--ambient-occlusion") {
			SetAmbientOcclusionEnabled(bool_val);
		} else if (arg == "--gpu-timestamp-scale") {
			try { SetGpuTimestampScalePercent(static_cast<uint32_t>(std::stoi(val))); } catch (...) {}
		} else if (arg == "--pipeline-libraries") {
			SetPipelineLibrariesEnabled(bool_val);
		} else if (arg == "--async-pipelines") {
			SetAsyncPipelinesEnabled(bool_val);
		} else if (arg == "--relaxed-readback") {
			SetRelaxedReadbackEnabled(bool_val);
		} else if (arg == "--speculative-draws") {
			SetSpeculativeDrawsEnabled(bool_val);
		} else if (arg == "--preset") {
			if (lower_val == "quality") {
				SetGpuTimestampScalePercent(115);
				SetAnisotropicFiltering(16);
				SetRayTracingEnabled(false);
				SetPipelineLibrariesEnabled(true);
				SetAsyncPipelinesEnabled(false);
				SetRelaxedReadbackEnabled(false);
			} else if (lower_val == "balanced") {
				SetGpuTimestampScalePercent(125);
				SetAnisotropicFiltering(8);
				SetRayTracingEnabled(false);
				SetPipelineLibrariesEnabled(true);
				SetAsyncPipelinesEnabled(true);
				SetRelaxedReadbackEnabled(true);
			} else if (lower_val == "performance") {
				SetGpuTimestampScalePercent(135);
				SetAnisotropicFiltering(4);
				SetRayTracingEnabled(false);
				SetPipelineLibrariesEnabled(true);
				SetAsyncPipelinesEnabled(true);
				SetRelaxedReadbackEnabled(true);
			}
		}
	}
}

const Keymap& GetKeymap() {
	return g_config->keymap;
}

} // namespace Config
