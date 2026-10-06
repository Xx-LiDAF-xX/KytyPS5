#include "common/emulatorConfig.h"

#include <cstdio>
#include <cstdlib>

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
	Config::Shutdown();
	return 0;
}
