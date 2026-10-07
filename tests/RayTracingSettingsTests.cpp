#include "common/emulatorConfig.h"
#include "common/settingsFile.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "RayTracingSettingsTests: %s\n", message);
		std::abort();
	}
}
} // namespace

int main() {
	Config::Initialize();
	Config::ConfigOptions options;
	Check(!options.auto_spec_optimization, "automatic optimization must default to disabled");
	options.ray_tracing_enabled = true;
	Config::Load(options);
	Config::ApplyAutoOptimization(false);
	Check(Config::RayTracingEnabled(), "automatic optimization disabled explicitly requested RT");
	Config::SetRayTracingEnabled(false);
	Config::ApplyAutoOptimization(false);
	Check(!Config::RayTracingEnabled(), "automatic optimization enabled disabled RT");
	Config::SetRayTracingEnabled(true);
	Config::ApplyAutoOptimization(false);
	Check(Config::RayTracingEnabled(), "automatic optimization discarded the runtime RT setting");
	Config::SetRayTracingEnabledOnRestart(false);
	Check(Config::RayTracingEnabled(), "pending RT change altered the active rendering path");
	Check(!Config::RayTracingEnabledOnRestart(), "pending RT disable was not retained");
	Config::SetRayTracingEnabledOnRestart(true);
	Config::SetRayTracingEnabled(false);
	Check(!Config::RayTracingEnabled(), "pending RT enable altered the active rendering path");
	Check(Config::RayTracingEnabledOnRestart(), "pending RT enable was not retained");
	const auto original_directory = std::filesystem::current_path();
	const auto test_directory = std::filesystem::temp_directory_path() /
	    ("kyty-rt-settings-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	Check(std::filesystem::create_directory(test_directory), "could not create isolated settings directory");
	std::filesystem::current_path(test_directory);
	Config::SaveCurrentSettings();
	std::vector<std::string> arguments;
	Check(Common::SettingsFile::LoadArguments(arguments), "saved settings could not be read");
	bool saved_enable = false;
	for (size_t i = 0; i + 1 < arguments.size(); ++i) {
		if (arguments[i] == "--ray-tracing") saved_enable = arguments[i + 1] == "true";
	}
	Check(saved_enable, "saving settings discarded the pending RT enable");
	Check(Common::SettingsFile::Save("ray-tracing", "false"), "could not save external RT change");
	Config::SetRayTracingEnabled(true);
	Config::ReloadFromSettingsFile();
	Check(Config::RayTracingEnabled(), "settings reload changed active RT without a restart");
	Check(!Config::RayTracingEnabledOnRestart(), "settings reload lost the pending RT disable");
	Check(Config::SaveRayTracingEnabledOnRestart(true), "immediate RT enable save failed");
	Check(Config::SaveRayTracingEnabledOnRestart(false), "immediate RT disable save failed");
	Check(Config::RayTracingEnabled(), "immediate save changed the active renderer");
	arguments.clear();
	Check(Common::SettingsFile::LoadArguments(arguments), "immediately saved setting could not be read");
	bool saved_disable = false;
	for (size_t i = 0; i + 1 < arguments.size(); ++i) {
		if (arguments[i] == "--ray-tracing") saved_disable = arguments[i + 1] == "false";
	}
	Check(saved_disable, "immediate RT disable was not persisted before closing the overlay");
	std::filesystem::rename("kyty_settings.ini", "saved-settings.ini");
	Check(std::filesystem::create_directory("kyty_settings.ini"), "could not simulate an unwritable settings path");
	Check(!Config::SaveRayTracingEnabledOnRestart(true), "save reported success for a directory path");
	Check(!Config::RayTracingEnabledOnRestart(), "failed save changed the pending RT state");
	Check(Config::RayTracingEnabled(), "failed save changed the active RT state");
	Check(std::filesystem::remove("kyty_settings.ini"), "could not remove simulated settings directory");
	std::filesystem::rename("saved-settings.ini", "kyty_settings.ini");
	std::filesystem::current_path(original_directory);
	std::filesystem::remove_all(test_directory);
	Config::Shutdown();
	return 0;
}
