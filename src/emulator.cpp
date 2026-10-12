#include "emulator.h"

#include "common/abi.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/runDiagnostics.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "common/subsystems.h"
#include "common/systemInfo.h"
#include "common/threads.h"
#include "graphics/presentation/systemOverlay.h"
#include "graphics/presentation/window.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "kytyGitVersion.h"
#include "libs/agc.h"
#include "libs/audio.h"
#include "libs/controller.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"
#include "loader/timer.h"

#include <cstdlib>
#include <filesystem>
#include <thread>
#include <utility>
#include <magic_enum.hpp>

namespace Emulator {

static const char* PlatformName() {
#if defined(__APPLE__)
	return "macOS";
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return "Windows";
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	return "Linux";
#else
	return "Unknown";
#endif
}

static void PrintSystemInfo() {
	const Common::SystemInfo info = Common::GetSystemInfo();

	LOGF("Build\n"
	     "  version: %s\n\n"
	     "Host\n"
	     "  os:      %s\n"
	     "  cpu:     %s\n"
	     "  threads: %u\n\n",
	     KYTY_BUILD_LABEL, PlatformName(), info.ProcessorName.c_str(),
	     std::thread::hardware_concurrency());
}

static std::string BoolText(bool value) {
	return value ? "true" : "false";
}

static void InitializeDiagnostics(const Config::ConfigOptions& cfg,
                                  const std::filesystem::path& elf) {
	if (cfg.diagnostics_output_file.empty()) {
		return;
	}

	Diagnostics::RunMetadata metadata {};
	metadata.build_label     = KYTY_BUILD_LABEL;
	metadata.platform        = PlatformName();
	metadata.cpu_name        = Common::GetSystemInfo().ProcessorName;
	metadata.executable_name = elf.filename().string();
	(void)Loader::SystemContentParamSfoGetString("TITLE_ID", &metadata.title_id);
	(void)Loader::SystemContentParamSfoGetString("TITLE", &metadata.title_name);
	(void)Loader::SystemContentParamSfoGetString("APP_VER", &metadata.content_version);
	metadata.configuration = {
	    {"screen_width", std::to_string(cfg.screen_width)},
	    {"screen_height", std::to_string(cfg.screen_height)},
	    {"present_mode", std::string(magic_enum::enum_name(cfg.present_mode))},
	    {"gpu_index", std::to_string(cfg.gpu_index)},
	    {"fullscreen", BoolText(cfg.fullscreen_enabled)},
	    {"hdr", BoolText(cfg.hdr_enabled)},
	    {"vblank_frequency", std::to_string(cfg.vblank_frequency)},
	    {"vulkan_validation", BoolText(cfg.vulkan_validation_enabled)},
	    {"shader_validation", BoolText(cfg.shader_validation_enabled)},
	    {"shader_optimization",
	     std::string(magic_enum::enum_name(cfg.shader_optimization_type))},
	    {"tessellation", BoolText(cfg.tessellation_enabled)},
	    {"readback_linear_images", BoolText(cfg.readback_linear_images)},
	    {"sync_raw_image_buffers", BoolText(cfg.sync_raw_image_buffers)},
	};

	Diagnostics::Initialize(cfg.diagnostics_output_file, std::move(metadata));
	Diagnostics::Record("lifecycle", "diagnostics_initialized", "run report started");
}

static void KytyClose() {
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	rt->Clear();
	Diagnostics::Finish(Diagnostics::Outcome::Completed, "emulator shutdown completed");

	LOGF("done!\n");

	Common::Subsystems::EmergencyShutdownActive();
}

static void MountOrCreateDir(const std::filesystem::path& dir, const std::string& point) {
	if (!Common::File::IsDirectoryExisting(dir)) {
		Common::File::CreateDirectories(dir);
	}

	EXIT_NOT_IMPLEMENTED(!Common::File::IsDirectoryExisting(dir));

	Libs::LibKernel::FileSystem::Mount(dir, point);
	auto dir_text = Common::PathToString(dir);
	LOGF("Mounted %s -> %s\n", point.c_str(), dir_text.c_str());
}

static void MountSandboxDirs() {
	std::string title_id;
	if (!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) {
		title_id = "UNKNOWN";
	}

	MountOrCreateDir("_DownloadData/" + title_id, "/download0");
	MountOrCreateDir("_TempData/" + title_id, "/temp0");
	MountOrCreateDir("_TempData/" + title_id, "/temp");
}

static bool ClearDirectoryContents(const std::filesystem::path& dir) {
	bool ok = true;

	for (const auto& entry: Common::File::GetDirEntries(dir)) {
		if (entry.name == "." || entry.name == "..") {
			continue;
		}

		auto path = dir / entry.name;

		if (entry.is_file) {
			Common::File::RemoveReadonly(path);
			ok = Common::File::DeleteFile(path) && ok;
		} else {
			ok = ClearDirectoryContents(path) && ok;
			ok = Common::File::DeleteDirectory(path) && ok;
		}
	}

	return ok;
}

static void ClearDebugTextureFolder() {
	const std::string debug_texture_folder = "_Textures";

	if (!Common::File::IsDirectoryExisting(debug_texture_folder)) {
		Common::File::CreateDirectories(debug_texture_folder);
		return;
	}

	if (!ClearDirectoryContents(debug_texture_folder)) {
		LOGF_COLOR(Log::Color::BrightYellow, "TextureDump: failed to completely clear %s\n",
		           debug_texture_folder.c_str());
	}
}

static void Init(const Config::ConfigOptions& cfg, const std::filesystem::path& param_json,
                 const std::filesystem::path& elf, Common::Subsystems& subsystems) {
	EXIT_IF(!Common::Thread::IsMainThread());

	subsystems.Initialize<Config::Lifecycle>();
	Config::Load(cfg);
	subsystems.Initialize<Log::Lifecycle>();

	if (Common::File::IsFileExisting(param_json)) {
		Loader::SystemContentLoadParamSfo(param_json);
		if (const auto flexible_memory_size = Loader::SystemContentGetFlexibleMemorySize();
		    flexible_memory_size != 0) {
			Libs::LibKernel::Memory::SetFlexibleMemorySize(flexible_memory_size);
		}
	}
	InitializeDiagnostics(cfg, elf);
	if (cfg.splash_enabled) {
		Libs::Graphics::InitializeSplashScreen(param_json.parent_path());
	}

	// Initialization order is explicit; destruction is automatic and reversed.
	subsystems.Initialize<Loader::Timer::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::PthreadLifecycle>();
	subsystems.Initialize<Profiler::Lifecycle>();
	subsystems.Initialize<Libs::Network::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::FileSystem::Lifecycle>();
	subsystems.Initialize<Libs::Controller::Lifecycle>();
	subsystems.Initialize<Libs::Audio::Lifecycle>();
	subsystems.Initialize<Libs::Graphics::Lifecycle>();
	Diagnostics::Record("lifecycle", "subsystems_initialized", "initialization complete");
}

static void LoadElf(const std::filesystem::path& elf, bool dbg_print_reloc = false,
                    const std::filesystem::path& save_name = {}) {
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	auto* program = rt->LoadProgram(
	    Libs::LibKernel::FileSystem::GetRealFilename(Common::PathToGenericString(elf)));
	Diagnostics::Record("loader", "program_loaded", elf.filename().string());

	if (dbg_print_reloc) {
		program->dbg_print_reloc = true;
	}

	if (!save_name.empty()) {
		rt->SaveProgram(program, Libs::LibKernel::FileSystem::GetRealFilename(
		                             Common::PathToGenericString(save_name)));
	}
}

static void Execute(const std::filesystem::path& game_patch) {
	auto           patch_path = game_patch;
	Diagnostics::Record("lifecycle", "guest_execution_started", "guest thread starting");
	Common::Thread guest_thread(
	    [](void* param) {
		    auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
		    rt->Execute(*static_cast<const std::filesystem::path*>(param));
	    },
	    &patch_path);
	Libs::Graphics::WindowRun();
	Diagnostics::Finish(Diagnostics::Outcome::Completed, "presentation loop ended");
	std::quick_exit(0);
}

void Run(const RunOptions& options) {
	if (options.app0_dir.empty()) {
		EXIT("app0 directory is required\n");
	}

	if (options.elf.empty()) {
		EXIT("ELF is required\n");
	}

	const auto         param_json = options.app0_dir / "sce_sys" / "param.json";
	Common::Subsystems subsystems(true);
	Init(options.config, param_json, options.elf, subsystems);

	ClearDebugTextureFolder();

	PrintSystemInfo();
	std::string title_id;
	if (Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) && !title_id.empty()) {
		Log::WriteToConsoleAndLog(fmt::format("Title ID: {}\n", title_id));
	}

	int ok = atexit(KytyClose);
	EXIT_NOT_IMPLEMENTED(ok != 0);

	// Guest threads are still running, so skip KytyClose() and only flush emergency state.
	ok = at_quick_exit(Common::Subsystems::EmergencyShutdownActive);
	EXIT_NOT_IMPLEMENTED(ok != 0);

	Libs::LibKernel::FileSystem::Mount(options.app0_dir, "/app0");
	Libs::LibKernel::FileSystem::Mount(options.app0_dir, "/hostapp");

	MountSandboxDirs();

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	Libs::InitAll(rt->Symbols());

	LoadElf(options.elf);

	Execute(options.game_patch);
}

} // namespace Emulator
