#ifndef EMULATOR_SRC_COMMON_ENGINE_PROFILE_H_
#define EMULATOR_SRC_COMMON_ENGINE_PROFILE_H_

#include "common/u59Preset.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Common::EngineProfile {

enum class EngineFamily : uint8_t {
	Unknown = 0,
	UnrealEngine5,
	Decima,
	Insomniac,
	NaughtyDog,
	SuckerPunch,
	SantaMonica,
	PolyphonyDigital,
	TeamAsobi,
	Bluepoint,
	SanDiegoStudio,
	BubbleBath,
	TigerEngine,
	Valkyrie,
	PhyreEngine,
	JapanStudio,
	PlayStationStudiosGeneric,
	Rage
};

struct EngineDetectionResult {
	EngineFamily                       family = EngineFamily::Unknown;
	std::string                        engine_name;
	std::string                        detected_reason;
	std::vector<Common::U59Preset::Entry> tuning_overrides;
};

[[nodiscard]] std::string_view EngineFamilyToString(EngineFamily family);
[[nodiscard]] EngineFamily     StringToEngineFamily(std::string_view name);

[[nodiscard]] std::vector<Common::U59Preset::Entry> GetEnginePresetOverrides(EngineFamily family);

[[nodiscard]] EngineDetectionResult DetectAndProfile(const std::filesystem::path& app0_dir,
                                                    const std::string&           title_id = {},
                                                    const std::string&           app_name = {});

// Injects the engine's tuned environment variables into the process environment.
// Existing environment variables (explicit user overrides) are never overwritten.
void ApplyEngineProfile(const EngineDetectionResult& result, bool log_changes = true);

} // namespace Common::EngineProfile

#endif // EMULATOR_SRC_COMMON_ENGINE_PROFILE_H_
