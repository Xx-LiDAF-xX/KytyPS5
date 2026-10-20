#include "common/engineProfile.h"
#include "common/u59Preset.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define KYTY_TEST_PID _getpid()
#else
#include <unistd.h>
#define KYTY_TEST_PID getpid()
#endif

namespace {

using namespace Common::EngineProfile;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "EngineProfileTests: FAILED: %s\n", message);
		std::abort();
	}
}

void WriteEmptyFile(const std::filesystem::path& path) {
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	Check(static_cast<bool>(file), "test can create file");
}

void TestEngineStringConversions() {
	Check(EngineFamilyToString(EngineFamily::UnrealEngine5) == "Unreal Engine 5", "UE5 string");
	Check(EngineFamilyToString(EngineFamily::Decima) == "Decima Engine", "Decima string");
	Check(EngineFamilyToString(EngineFamily::Insomniac) == "Insomniac Engine", "Insomniac string");
	Check(EngineFamilyToString(EngineFamily::NaughtyDog) == "Naughty Dog Engine", "Naughty Dog string");
	Check(EngineFamilyToString(EngineFamily::SuckerPunch) == "Sucker Punch Engine", "Sucker Punch string");
	Check(EngineFamilyToString(EngineFamily::SantaMonica) == "Santa Monica Studio Engine", "Santa Monica string");
	Check(EngineFamilyToString(EngineFamily::PolyphonyDigital) == "Polyphony Digital Engine", "Polyphony string");
	Check(EngineFamilyToString(EngineFamily::TeamAsobi) == "Team ASOBI Engine", "Team ASOBI string");
	Check(EngineFamilyToString(EngineFamily::Bluepoint) == "Bluepoint Engine", "Bluepoint string");
	Check(EngineFamilyToString(EngineFamily::SanDiegoStudio) == "San Diego Studio Engine", "San Diego string");
	Check(EngineFamilyToString(EngineFamily::BubbleBath) == "BubbleBath Engine", "BubbleBath string");
	Check(EngineFamilyToString(EngineFamily::TigerEngine) == "Tiger / Destiny Engine", "Tiger string");
	Check(EngineFamilyToString(EngineFamily::Valkyrie) == "Valkyrie Engine", "Valkyrie string");
	Check(EngineFamilyToString(EngineFamily::PhyreEngine) == "PhyreEngine", "PhyreEngine string");
	Check(EngineFamilyToString(EngineFamily::JapanStudio) == "Japan Studio Engine", "Japan Studio string");
	Check(EngineFamilyToString(EngineFamily::PlayStationStudiosGeneric) == "PlayStation Studios (Generic)", "PS Studios string");
	Check(EngineFamilyToString(EngineFamily::Rage) == "Rockstar Advanced Game Engine (RAGE)", "RAGE string");
	Check(EngineFamilyToString(EngineFamily::Unknown) == "Unknown", "Unknown string");

	Check(StringToEngineFamily("UnrealEngine5") == EngineFamily::UnrealEngine5, "parse UE5");
	Check(StringToEngineFamily("ue5") == EngineFamily::UnrealEngine5, "parse ue5 lowercase");
	Check(StringToEngineFamily("DECIMA") == EngineFamily::Decima, "parse decima uppercase");
	Check(StringToEngineFamily("Dreams") == EngineFamily::BubbleBath, "parse Dreams alias");
	Check(StringToEngineFamily("Destiny") == EngineFamily::TigerEngine, "parse Destiny alias");
	Check(StringToEngineFamily("Team ASOBI") == EngineFamily::TeamAsobi, "parse ASOBI space");
	Check(StringToEngineFamily("RAGE") == EngineFamily::Rage, "parse RAGE uppercase");
	Check(StringToEngineFamily("Rockstar") == EngineFamily::Rage, "parse Rockstar alias");
	Check(StringToEngineFamily("NonExistentEngine") == EngineFamily::Unknown, "parse unknown");
}

void TestPresetOverridesForEveryEngine() {
	const std::vector<EngineFamily> all_families = {
	    EngineFamily::UnrealEngine5,
	    EngineFamily::Decima,
	    EngineFamily::Insomniac,
	    EngineFamily::NaughtyDog,
	    EngineFamily::SuckerPunch,
	    EngineFamily::SantaMonica,
	    EngineFamily::PolyphonyDigital,
	    EngineFamily::TeamAsobi,
	    EngineFamily::Bluepoint,
	    EngineFamily::SanDiegoStudio,
	    EngineFamily::BubbleBath,
	    EngineFamily::TigerEngine,
	    EngineFamily::Valkyrie,
	    EngineFamily::PhyreEngine,
	    EngineFamily::JapanStudio,
	    EngineFamily::PlayStationStudiosGeneric,
	    EngineFamily::Rage
	};

	for (const auto family: all_families) {
		const auto overrides = GetEnginePresetOverrides(family);
		Check(!overrides.empty(), "all 17 engine families must define tuning overrides");
		for (const auto& entry: overrides) {
			Check(!entry.key.empty() && entry.key.starts_with("KYTY_"), "all keys must start with KYTY_");
			Check(!entry.value.empty(), "all values must be non-empty strings");
		}
	}

	Check(GetEnginePresetOverrides(EngineFamily::Unknown).empty(), "Unknown family has empty overrides");
}

void TestFilesystemDetection() {
	const auto root = std::filesystem::temp_directory_path() /
	                  ("kyty_engine_profile_test_" + std::to_string(KYTY_TEST_PID));
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	std::filesystem::create_directories(root, ec);
	Check(!ec, "created test root");

	// UE5 via uecommandline.txt
	{
		const auto app_ue5 = root / "app_ue5";
		std::filesystem::create_directories(app_ue5, ec);
		WriteEmptyFile(app_ue5 / "uecommandline.txt");
		const auto res = DetectAndProfile(app_ue5);
		Check(res.family == EngineFamily::UnrealEngine5, "detected UE5 from uecommandline.txt");
	}

	// UE5 via engine/ folder
	{
		const auto app_ue5_dir = root / "app_ue5_dir";
		std::filesystem::create_directories(app_ue5_dir / "engine", ec);
		const auto res = DetectAndProfile(app_ue5_dir);
		Check(res.family == EngineFamily::UnrealEngine5, "detected UE5 from engine/ dir");
	}

	// Decima via ds2.bin
	{
		const auto app_decima = root / "app_decima";
		std::filesystem::create_directories(app_decima, ec);
		WriteEmptyFile(app_decima / "ds2.bin");
		const auto res = DetectAndProfile(app_decima);
		Check(res.family == EngineFamily::Decima, "detected Decima from ds2.bin");
	}

	// Insomniac via toc
	{
		const auto app_insomniac = root / "app_insomniac";
		std::filesystem::create_directories(app_insomniac, ec);
		WriteEmptyFile(app_insomniac / "toc");
		const auto res = DetectAndProfile(app_insomniac);
		Check(res.family == EngineFamily::Insomniac, "detected Insomniac from toc");
	}

	// Naughty Dog via pak23.psarc
	{
		const auto app_nd = root / "app_nd";
		std::filesystem::create_directories(app_nd, ec);
		WriteEmptyFile(app_nd / "pak23.psarc");
		const auto res = DetectAndProfile(app_nd);
		Check(res.family == EngineFamily::NaughtyDog, "detected Naughty Dog from pak23.psarc");
	}

	// PhyreEngine via .phyre
	{
		const auto app_phyre = root / "app_phyre";
		std::filesystem::create_directories(app_phyre, ec);
		WriteEmptyFile(app_phyre / "game.phyre");
		const auto res = DetectAndProfile(app_phyre);
		Check(res.family == EngineFamily::PhyreEngine, "detected PhyreEngine from .phyre");
	}

	// Tiger via packages/
	{
		const auto app_tiger = root / "app_tiger";
		std::filesystem::create_directories(app_tiger / "packages", ec);
		const auto res = DetectAndProfile(app_tiger);
		Check(res.family == EngineFamily::TigerEngine, "detected Tiger from packages/");
	}

	// RAGE via .rpf
	{
		const auto app_rage = root / "app_rage";
		std::filesystem::create_directories(app_rage, ec);
		WriteEmptyFile(app_rage / "common.rpf");
		const auto res = DetectAndProfile(app_rage);
		Check(res.family == EngineFamily::Rage, "detected RAGE from common.rpf");
	}

	std::filesystem::remove_all(root, ec);
}

void TestTitleIdAndAppNameDetection() {
	// Astro Bot -> Team ASOBI
	Check(DetectAndProfile({}, "PPSA21567").family == EngineFamily::TeamAsobi, "PPSA21567 is Team ASOBI");
	Check(DetectAndProfile({}, "PPSA99999", "Astro Bot Rescue").family == EngineFamily::TeamAsobi, "Astro name match");

	// Demon's Souls -> Bluepoint
	Check(DetectAndProfile({}, "PPSA01325").family == EngineFamily::Bluepoint, "PPSA01325 is Bluepoint");
	Check(DetectAndProfile({}, "PPSA99999", "Demon's Souls").family == EngineFamily::Bluepoint, "Demon's Souls name match");

	// Spider-Man -> Insomniac
	Check(DetectAndProfile({}, "PPSA01859").family == EngineFamily::Insomniac, "PPSA01859 is Insomniac");
	Check(DetectAndProfile({}, "PPSA99999", "Marvel's Spider-Man 2").family == EngineFamily::Insomniac, "Spider-Man name match");

	// Gran Turismo 7 -> Polyphony
	Check(DetectAndProfile({}, "PPSA01317").family == EngineFamily::PolyphonyDigital, "PPSA01317 is Polyphony");

	// God of War -> Santa Monica
	Check(DetectAndProfile({}, "PPSA08329").family == EngineFamily::SantaMonica, "PPSA08329 is Santa Monica");

	// Ghost of Tsushima -> Sucker Punch
	Check(DetectAndProfile({}, "PPSA03209").family == EngineFamily::SuckerPunch, "PPSA03209 is Sucker Punch");

	// The Last of Us -> Naughty Dog
	Check(DetectAndProfile({}, "PPSA03396").family == EngineFamily::NaughtyDog, "PPSA03396 is Naughty Dog");

	// MLB The Show -> San Diego Studio
	Check(DetectAndProfile({}, "PPSA01521").family == EngineFamily::SanDiegoStudio, "PPSA01521 is San Diego Studio");

	// Dreams -> BubbleBath
	Check(DetectAndProfile({}, "PPSA99999", "Dreams").family == EngineFamily::BubbleBath, "Dreams is BubbleBath");

	// Gravity Rush -> Japan Studio
	Check(DetectAndProfile({}, "PPSA99999", "Gravity Rush 2").family == EngineFamily::JapanStudio, "Gravity Rush is Japan Studio");

	// Rockstar Games (GTA V) -> RAGE
	Check(DetectAndProfile({}, "PPSA03420").family == EngineFamily::Rage, "PPSA03420 is RAGE");
	Check(DetectAndProfile({}, "PPSA99999", "Grand Theft Auto V").family == EngineFamily::Rage, "GTA V name match");

	// Generic PlayStation Studios fallback
	Check(DetectAndProfile({}, "PPSA99999", "Generic First Party").family == EngineFamily::PlayStationStudiosGeneric,
	      "PPSA generic is PlayStation Studios");

	// Unknown
	Check(DetectAndProfile({}, "CUSA00000", "Random Title").family == EngineFamily::Unknown, "Unknown fallback");
}

void TestApplyEngineProfileRespectsUserOverrides() {
#if defined(_WIN32)
	_putenv_s("KYTY_TEST_OVERRIDE_FLAG", "USER_VALUE");
#else
	setenv("KYTY_TEST_OVERRIDE_FLAG", "USER_VALUE", 1);
#endif

	EngineDetectionResult res;
	res.family          = EngineFamily::UnrealEngine5;
	res.engine_name     = "Unreal Engine 5";
	res.detected_reason = "unit test";
	res.tuning_overrides = {
	    {"KYTY_TEST_OVERRIDE_FLAG", "ENGINE_VALUE"},
	    {"KYTY_TEST_NEW_FLAG", "ENGINE_VALUE"}
	};

	ApplyEngineProfile(res, false);

	const char* override_val = std::getenv("KYTY_TEST_OVERRIDE_FLAG");
	Check(override_val != nullptr && std::strcmp(override_val, "USER_VALUE") == 0,
	      "ApplyEngineProfile never overwrites existing user environment variables");

	const char* new_val = std::getenv("KYTY_TEST_NEW_FLAG");
	Check(new_val != nullptr && std::strcmp(new_val, "ENGINE_VALUE") == 0,
	      "ApplyEngineProfile applies new engine environment variables");
}

void TestShippedEnginePresetsValidation() {
#if defined(KYTY_ENGINE_PRESETS_DIR)
	const std::filesystem::path dir(KYTY_ENGINE_PRESETS_DIR);
	Check(std::filesystem::exists(dir), "engine presets directory exists");

	const std::vector<std::string> expected_files = {
	    "ue5.json",
	    "decima.json",
	    "insomniac.json",
	    "naughty-dog.json",
	    "sucker-punch.json",
	    "santa-monica.json",
	    "polyphony.json",
	    "team-asobi.json",
	    "bluepoint.json",
	    "san-diego.json",
	    "bubblebath.json",
	    "tiger.json",
	    "valkyrie.json",
	    "phyreengine.json",
	    "japan-studio.json",
	    "playstation-studios.json",
	    "rage.json"
	};

	for (const auto& file_name: expected_files) {
		const auto path = dir / file_name;
		Check(std::filesystem::exists(path), ("preset file exists: " + file_name).c_str());

		std::ifstream file(path);
		std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		std::vector<Common::U59Preset::Entry> entries;
		std::string error;
		Check(Common::U59Preset::Parse(content, entries, error),
		      ("preset file parses as valid U59Preset schema: " + file_name + " (" + error + ")").c_str());
		Check(!entries.empty(), ("preset file has non-empty entries: " + file_name).c_str());
	}
#endif
}

} // namespace

int main() {
	TestEngineStringConversions();
	TestPresetOverridesForEveryEngine();
	TestFilesystemDetection();
	TestTitleIdAndAppNameDetection();
	TestApplyEngineProfileRespectsUserOverrides();
	TestShippedEnginePresetsValidation();

	std::printf("EngineProfileTests: all passed\n");
	return 0;
}
