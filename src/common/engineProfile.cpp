#include "common/engineProfile.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Common::EngineProfile {

namespace {

std::string ToLower(std::string_view str) {
	std::string result(str);
	std::transform(result.begin(), result.end(), result.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return result;
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	return std::equal(a.begin(), a.end(), b.begin(), [](char ca, char cb) {
		return std::tolower(static_cast<unsigned char>(ca)) ==
		       std::tolower(static_cast<unsigned char>(cb));
	});
}

bool ContainsNoCase(std::string_view haystack, std::string_view needle) {
	if (needle.empty()) {
		return true;
	}
	const std::string h = ToLower(haystack);
	const std::string n = ToLower(needle);
	return h.find(n) != std::string::npos;
}

void SetEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

} // namespace

std::string_view EngineFamilyToString(EngineFamily family) {
	switch (family) {
	case EngineFamily::UnrealEngine5:
		return "Unreal Engine 5";
	case EngineFamily::Decima:
		return "Decima Engine";
	case EngineFamily::Insomniac:
		return "Insomniac Engine";
	case EngineFamily::NaughtyDog:
		return "Naughty Dog Engine";
	case EngineFamily::SuckerPunch:
		return "Sucker Punch Engine";
	case EngineFamily::SantaMonica:
		return "Santa Monica Studio Engine";
	case EngineFamily::PolyphonyDigital:
		return "Polyphony Digital Engine";
	case EngineFamily::TeamAsobi:
		return "Team ASOBI Engine";
	case EngineFamily::Bluepoint:
		return "Bluepoint Engine";
	case EngineFamily::SanDiegoStudio:
		return "San Diego Studio Engine";
	case EngineFamily::BubbleBath:
		return "BubbleBath Engine";
	case EngineFamily::TigerEngine:
		return "Tiger / Destiny Engine";
	case EngineFamily::Valkyrie:
		return "Valkyrie Engine";
	case EngineFamily::PhyreEngine:
		return "PhyreEngine";
	case EngineFamily::JapanStudio:
		return "Japan Studio Engine";
	case EngineFamily::PlayStationStudiosGeneric:
		return "PlayStation Studios (Generic)";
	case EngineFamily::Rage:
		return "Rockstar Advanced Game Engine (RAGE)";
	case EngineFamily::Unknown:
	default:
		return "Unknown";
	}
}

EngineFamily StringToEngineFamily(std::string_view name) {
	if (EqualsNoCase(name, "UnrealEngine5") || EqualsNoCase(name, "Unreal Engine 5") ||
	    EqualsNoCase(name, "UE5") || EqualsNoCase(name, "Unreal")) {
		return EngineFamily::UnrealEngine5;
	}
	if (EqualsNoCase(name, "Decima") || EqualsNoCase(name, "Decima Engine")) {
		return EngineFamily::Decima;
	}
	if (EqualsNoCase(name, "Insomniac") || EqualsNoCase(name, "Insomniac Engine")) {
		return EngineFamily::Insomniac;
	}
	if (EqualsNoCase(name, "NaughtyDog") || EqualsNoCase(name, "Naughty Dog") ||
	    EqualsNoCase(name, "Naughty Dog Engine")) {
		return EngineFamily::NaughtyDog;
	}
	if (EqualsNoCase(name, "SuckerPunch") || EqualsNoCase(name, "Sucker Punch") ||
	    EqualsNoCase(name, "Sucker Punch Engine")) {
		return EngineFamily::SuckerPunch;
	}
	if (EqualsNoCase(name, "SantaMonica") || EqualsNoCase(name, "Santa Monica") ||
	    EqualsNoCase(name, "Santa Monica Studio Engine")) {
		return EngineFamily::SantaMonica;
	}
	if (EqualsNoCase(name, "PolyphonyDigital") || EqualsNoCase(name, "Polyphony") ||
	    EqualsNoCase(name, "Polyphony Digital Engine")) {
		return EngineFamily::PolyphonyDigital;
	}
	if (EqualsNoCase(name, "TeamAsobi") || EqualsNoCase(name, "Team ASOBI") ||
	    EqualsNoCase(name, "Asobi") || EqualsNoCase(name, "Team ASOBI Engine")) {
		return EngineFamily::TeamAsobi;
	}
	if (EqualsNoCase(name, "Bluepoint") || EqualsNoCase(name, "Bluepoint Engine")) {
		return EngineFamily::Bluepoint;
	}
	if (EqualsNoCase(name, "SanDiegoStudio") || EqualsNoCase(name, "San Diego Studio") ||
	    EqualsNoCase(name, "San Diego") || EqualsNoCase(name, "San Diego Studio Engine")) {
		return EngineFamily::SanDiegoStudio;
	}
	if (EqualsNoCase(name, "BubbleBath") || EqualsNoCase(name, "BubbleBath Engine") ||
	    EqualsNoCase(name, "Dreams")) {
		return EngineFamily::BubbleBath;
	}
	if (EqualsNoCase(name, "TigerEngine") || EqualsNoCase(name, "Tiger") ||
	    EqualsNoCase(name, "Destiny") || EqualsNoCase(name, "Tiger / Destiny Engine")) {
		return EngineFamily::TigerEngine;
	}
	if (EqualsNoCase(name, "Valkyrie") || EqualsNoCase(name, "Valkyrie Engine")) {
		return EngineFamily::Valkyrie;
	}
	if (EqualsNoCase(name, "PhyreEngine") || EqualsNoCase(name, "Phyre")) {
		return EngineFamily::PhyreEngine;
	}
	if (EqualsNoCase(name, "JapanStudio") || EqualsNoCase(name, "Japan Studio") ||
	    EqualsNoCase(name, "Japan Studio Engine")) {
		return EngineFamily::JapanStudio;
	}
	if (EqualsNoCase(name, "PlayStationStudios") || EqualsNoCase(name, "PlayStation Studios") ||
	    EqualsNoCase(name, "PlayStation Studios (Generic)")) {
		return EngineFamily::PlayStationStudiosGeneric;
	}
	if (EqualsNoCase(name, "Rage") || EqualsNoCase(name, "RAGE") ||
	    EqualsNoCase(name, "Rockstar Advanced Game Engine") || EqualsNoCase(name, "Rockstar")) {
		return EngineFamily::Rage;
	}
	return EngineFamily::Unknown;
}

std::vector<Common::U59Preset::Entry> GetEnginePresetOverrides(EngineFamily family) {
	std::vector<Common::U59Preset::Entry> entries;

	switch (family) {
	case EngineFamily::UnrealEngine5:
		entries = {
		    {"KYTY_BDA_WRITES", "1"},
		    {"KYTY_BDA_INCREMENTAL_SYNC", "1"},
		    {"KYTY_RT_SOFTWARE", "1"},
		    {"KYTY_RT_NODE_BUDGET", "16384"},
		    {"KYTY_PIPELINE_PREFETCH", "1"},
		    {"KYTY_PIPELINE_PREFETCH_PROGRAMS", "1"},
		    {"KYTY_DRAW_RUN", "1"},
		    {"KYTY_DRAW_RUN_ACQUIRE", "1"},
		    {"KYTY_DRAW_RUN_PUSH", "1"},
		    {"KYTY_SUBMISSION_COALESCE", "1"},
		    {"KYTY_SRT_ARITHMETIC_TAPES", "1"},
		    {"KYTY_VRAM_GC_BUDGET", "1"},
		    {"KYTY_PIPELINE_FAST_FIRST", "0"},
		    {"KYTY_FUNCTION_ARRAY_SHRINK", "0"},
		    {"KYTY_PIPELINE_PREWARM_COMPUTE", "0"},
		};
		break;

	case EngineFamily::Decima:
		entries = {
		    {"KYTY_DCC_GPU", "1"},
		    {"KYTY_DCC_GPU_REFRESH", "1"},
		    {"KYTY_TEXTURE_SPARSE_RESIDENCY", "1"},
		    {"KYTY_DIRTY_IMAGE_QUERY", "direct"},
		    {"KYTY_FAST_BACKING_TRANSFERS", "1"},
		    {"KYTY_SUBMISSION_MODE", "queued"},
		};
		break;

	case EngineFamily::Insomniac:
		entries = {
		    {"KYTY_RT_SOFTWARE", "auto"},
		    {"KYTY_RT_NODE_BUDGET", "8192"},
		    {"KYTY_MESH_RESTART", "1"},
		    {"KYTY_PIPELINE_FAST_FIRST", "1"},
		    {"KYTY_DEFER_PROTECTS", "all"},
		    {"KYTY_RESOURCE_MATERIALIZATION", "optimized"},
		};
		break;

	case EngineFamily::NaughtyDog:
		entries = {
		    {"KYTY_CP_COMMIT", "all"},
		    {"KYTY_CP_SEQ", "1"},
		    {"KYTY_CP_SEQ_PREFETCH", "1"},
		    {"KYTY_SUBMISSION_COALESCE", "1"},
		    {"KYTY_VRAM_IDLE_FRAMES", "600"},
		    {"KYTY_EOP_TIMESTAMPS", "gpu"},
		};
		break;

	case EngineFamily::SuckerPunch:
		entries = {
		    {"KYTY_PIPELINE_PREWARM_COMPUTE", "1"},
		    {"KYTY_SHADER_PRECOMPILE", "1"},
		    {"KYTY_DRAW_PREP_BINDINGS", "1"},
		    {"KYTY_RECORDER_DESCRIPTOR_SETS", "1"},
		};
		break;

	case EngineFamily::SantaMonica:
		entries = {
		    {"KYTY_META_CLEAR_MEMO", "1"},
		    {"KYTY_PENDING_REFRESH_US", "150"},
		    {"KYTY_BINDING_MEMO_TOUCH", "1"},
		    {"KYTY_PIPELINE_PREFETCH", "1"},
		};
		break;

	case EngineFamily::PolyphonyDigital:
		entries = {
		    {"KYTY_RT_SOFTWARE", "auto"},
		    {"KYTY_CB_SHADER_MASK_EXPORTS", "1"},
		    {"KYTY_TEXTURE_CLEAN_PROOFS", "1"},
		    {"KYTY_EOP_TIMESTAMPS", "gpu"},
		};
		break;

	case EngineFamily::TeamAsobi:
		entries = {
		    {"KYTY_RT_SOFTWARE", "1"},
		    {"KYTY_RT_NODE_BUDGET", "8192"},
		    {"KYTY_DCC_GPU", "1"},
		    {"KYTY_DCC_GPU_REFRESH", "1"},
		    {"KYTY_NATIVE_IMAGE_POOL", "1"},
		    {"KYTY_SRT_ARITHMETIC_TAPES", "1"},
		};
		break;

	case EngineFamily::Bluepoint:
		entries = {
		    {"KYTY_LOD_STATS_MODE", "gpu"},
		    {"KYTY_PIPELINE_FAST_FIRST", "1"},
		    {"KYTY_BINDING_LEAN_MISS", "1"},
		    {"KYTY_DIRTY_IMAGE_QUERY", "direct"},
		};
		break;

	case EngineFamily::SanDiegoStudio:
		entries = {
		    {"KYTY_DRAW_RUN", "1"},
		    {"KYTY_CP_OWNED_COUNTERS", "1"},
		    {"KYTY_DRAW_PREP_CLEAN_SINCE", "1"},
		};
		break;

	case EngineFamily::BubbleBath:
		entries = {
		    {"KYTY_BDA_PAGETABLE_SPARSE", "1"},
		    {"KYTY_PIPELINE_PREWARM_COMPUTE", "1"},
		    {"KYTY_TILER_SCRATCH_POOL_IDLE_MS", "2000"},
		};
		break;

	case EngineFamily::TigerEngine:
		entries = {
		    {"KYTY_SUBMISSION_COALESCE", "1"},
		    {"KYTY_CP_RECORDER", "1"},
		    {"KYTY_CP_SEQ", "1"},
		    {"KYTY_RENDERER_BATCH", "1"},
		};
		break;

	case EngineFamily::Valkyrie:
		entries = {
		    {"KYTY_DRAW_PREP_BINDINGS", "1"},
		    {"KYTY_PUSH_SHADOW_FRESH_SKIP", "1"},
		    {"KYTY_OCCLUSION_BATCH", "1"},
		};
		break;

	case EngineFamily::PhyreEngine:
		entries = {
		    {"KYTY_PROGRAM_CACHE", "1"},
		    {"KYTY_IMAGE_LOOKUP", "fast"},
		    {"KYTY_FAST_BACKING_TRANSFERS", "1"},
		};
		break;

	case EngineFamily::JapanStudio:
		entries = {
		    {"KYTY_CB_SHADER_MASK_EXPORTS", "1"},
		    {"KYTY_PIPELINE_JOURNAL", "1"},
		    {"KYTY_RAM_SMALL_UPLOAD_RING", "1"},
		};
		break;

	case EngineFamily::PlayStationStudiosGeneric:
		entries = {
		    {"KYTY_SUBMISSION_COALESCE", "1"},
		    {"KYTY_PROGRAM_CACHE", "1"},
		    {"KYTY_PIPELINE_PREFETCH", "1"},
		};
		break;

	case EngineFamily::Rage:
		entries = {
		    {"KYTY_RT_SOFTWARE", "auto"},
		    {"KYTY_RT_NODE_BUDGET", "12288"},
		    {"KYTY_DRAW_RUN", "1"},
		    {"KYTY_DRAW_RUN_ACQUIRE", "1"},
		    {"KYTY_DRAW_RUN_PUSH", "1"},
		    {"KYTY_DRAW_RUN_WRITES", "1"},
		    {"KYTY_PIPELINE_PREFETCH", "1"},
		    {"KYTY_PIPELINE_PREFETCH_PROGRAMS", "1"},
		    {"KYTY_SUBMISSION_COALESCE", "1"},
		    {"KYTY_CP_SEQ", "1"},
		    {"KYTY_CP_SEQ_PREFETCH", "1"},
		    {"KYTY_TEXTURE_SPARSE_RESIDENCY", "1"},
		    {"KYTY_FAST_BACKING_TRANSFERS", "1"},
		    {"KYTY_DCC_GPU", "1"},
		    {"KYTY_DCC_GPU_REFRESH", "1"},
		    {"KYTY_VRAM_GC_BUDGET", "1"},
		};
		break;

	case EngineFamily::Unknown:
	default:
		break;
	}

	return entries;
}

EngineDetectionResult DetectAndProfile(const std::filesystem::path& app0_dir,
                                      const std::string&           title_id,
                                      const std::string&           app_name) {
	EngineDetectionResult result;
	std::error_code       ec;

	const std::string upper_title_id = ToLower(title_id);
	const std::string lower_app_name = ToLower(app_name);

	// 1. Filesystem Fingerprinting
	if (!app0_dir.empty() && std::filesystem::exists(app0_dir, ec)) {
		bool has_uecommandline = false;
		bool has_engine_dir    = false;
		bool has_decima_bin    = false;
		bool has_insomniac_toc = false;
		bool has_naughty_dog   = false;
		bool has_phyre         = false;
		bool has_tiger         = false;
		bool has_rage_rpf      = false;

		for (const auto& entry: std::filesystem::directory_iterator(app0_dir, ec)) {
			if (ec) {
				break;
			}
			const auto name = entry.path().filename().string();
			const auto lower = ToLower(name);

			if (lower == "uecommandline.txt") {
				has_uecommandline = true;
			} else if (lower == "engine" && entry.is_directory()) {
				has_engine_dir = true;
			} else if (lower == "ds2.bin" || lower == "core" ||
			           (entry.is_regular_file() && lower.ends_with(".bin") && ContainsNoCase(lower, "decima"))) {
				has_decima_bin = true;
			} else if (lower == "toc" || (entry.is_regular_file() && lower.ends_with(".cache") &&
			                              (ContainsNoCase(lower, "dx12") || ContainsNoCase(lower, "ps5")))) {
				has_insomniac_toc = true;
			} else if (lower == "pak23.psarc" || (ContainsNoCase(lower, "psarc") && ContainsNoCase(lower, "tlou"))) {
				has_naughty_dog = true;
			} else if (lower.ends_with(".phyre") || lower.ends_with(".pfx")) {
				has_phyre = true;
			} else if (lower == "packages" && entry.is_directory()) {
				has_tiger = true;
			} else if (lower.ends_with(".rpf")) {
				has_rage_rpf = true;
			}
		}

		if (has_uecommandline || has_engine_dir) {
			result.family          = EngineFamily::UnrealEngine5;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = has_uecommandline ? "detected uecommandline.txt in app0"
			                                           : "detected engine/ directory in app0";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		if (has_decima_bin) {
			result.family          = EngineFamily::Decima;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "detected Decima core/archive manifest in app0";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		if (has_insomniac_toc) {
			result.family          = EngineFamily::Insomniac;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "detected Insomniac TOC/cache archives in app0";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		if (has_naughty_dog) {
			result.family          = EngineFamily::NaughtyDog;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "detected Naughty Dog package archive signature in app0";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		if (has_phyre) {
			result.family          = EngineFamily::PhyreEngine;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "detected PhyreEngine asset signature in app0";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		if (has_tiger) {
			result.family          = EngineFamily::TigerEngine;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "detected Destiny/Tiger packages/ structure in app0";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		if (has_rage_rpf) {
			result.family          = EngineFamily::Rage;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "detected Rockstar RPF package archive in app0";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}
	}

	// 2. Title ID & App Name Heuristics
	if (!upper_title_id.empty()) {
		// Team ASOBI (Astro Bot / Astro's Playroom)
		if (upper_title_id == "ppsa21567" || ContainsNoCase(lower_app_name, "astro")) {
			result.family          = EngineFamily::TeamAsobi;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "matched Team ASOBI title (Astro Bot)";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		// Bluepoint (Demon's Souls)
		if (upper_title_id == "ppsa01325" || ContainsNoCase(lower_app_name, "demon's souls") ||
		    ContainsNoCase(lower_app_name, "demons souls")) {
			result.family          = EngineFamily::Bluepoint;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "matched Bluepoint title (Demon's Souls)";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		// Insomniac (Spider-Man, Ratchet & Clank)
		if (upper_title_id == "ppsa01859" || upper_title_id == "ppsa01860" ||
		    upper_title_id == "ppsa01487" || ContainsNoCase(lower_app_name, "spider-man") ||
		    ContainsNoCase(lower_app_name, "ratchet")) {
			result.family          = EngineFamily::Insomniac;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "matched Insomniac title ID / signature";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		// Polyphony Digital (Gran Turismo 7)
		if (upper_title_id == "ppsa01317" || ContainsNoCase(lower_app_name, "gran turismo")) {
			result.family          = EngineFamily::PolyphonyDigital;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "matched Polyphony Digital title (Gran Turismo 7)";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		// Santa Monica Studio (God of War Ragnarok)
		if (upper_title_id == "ppsa08329" || upper_title_id == "ppsa08330" ||
		    ContainsNoCase(lower_app_name, "god of war")) {
			result.family          = EngineFamily::SantaMonica;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "matched Santa Monica Studio title (God of War)";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		// Sucker Punch (Ghost of Tsushima)
		if (upper_title_id == "ppsa03209" || ContainsNoCase(lower_app_name, "ghost of tsushima") ||
		    ContainsNoCase(lower_app_name, "yotei")) {
			result.family          = EngineFamily::SuckerPunch;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "matched Sucker Punch title (Ghost of Tsushima)";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		// Naughty Dog (The Last of Us Part I/II Remastered)
		if (upper_title_id == "ppsa03396" || upper_title_id == "ppsa10662" ||
		    ContainsNoCase(lower_app_name, "last of us") || ContainsNoCase(lower_app_name, "uncharted")) {
			result.family          = EngineFamily::NaughtyDog;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "matched Naughty Dog title ID / signature";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		// San Diego Studio (MLB The Show)
		if (upper_title_id == "ppsa01521" || ContainsNoCase(lower_app_name, "mlb the show")) {
			result.family          = EngineFamily::SanDiegoStudio;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "matched San Diego Studio title (MLB The Show)";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		// BubbleBath (Dreams)
		if (ContainsNoCase(lower_app_name, "dreams")) {
			result.family          = EngineFamily::BubbleBath;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "matched Media Molecule title (Dreams)";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		// Japan Studio (Gravity Rush 2, The Last Guardian)
		if (ContainsNoCase(lower_app_name, "gravity rush") || ContainsNoCase(lower_app_name, "last guardian")) {
			result.family          = EngineFamily::JapanStudio;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "matched Japan Studio title";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		// Rockstar Games / RAGE (Grand Theft Auto V, RDR)
		if (upper_title_id == "ppsa03420" || upper_title_id == "ppsa03421" ||
		    upper_title_id == "ppsa01757" || ContainsNoCase(lower_app_name, "grand theft auto") ||
		    ContainsNoCase(lower_app_name, "gta") || ContainsNoCase(lower_app_name, "red dead") ||
		    ContainsNoCase(lower_app_name, "rockstar")) {
			result.family          = EngineFamily::Rage;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "matched Rockstar Games / RAGE title ID or signature";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}

		// General PlayStation Studios Check
		if (upper_title_id.starts_with("ppsa") || ContainsNoCase(lower_app_name, "playstation") ||
		    ContainsNoCase(lower_app_name, "sony")) {
			result.family          = EngineFamily::PlayStationStudiosGeneric;
			result.engine_name     = EngineFamilyToString(result.family);
			result.detected_reason = "generic PlayStation Studios title identifier";
			result.tuning_overrides = GetEnginePresetOverrides(result.family);
			return result;
		}
	}

	result.family          = EngineFamily::Unknown;
	result.engine_name     = EngineFamilyToString(result.family);
	result.detected_reason = "no specific engine signature matched";
	result.tuning_overrides.clear();
	return result;
}

void ApplyEngineProfile(const EngineDetectionResult& result, bool log_changes) {
	if (result.family == EngineFamily::Unknown || result.tuning_overrides.empty()) {
		return;
	}

	uint32_t applied_count = 0;
	uint32_t skipped_count = 0;

	for (const auto& entry: result.tuning_overrides) {
		const char* current = std::getenv(entry.key.c_str());
		if (current != nullptr && *current != '\0') {
			// Explicit user environment variable exists: do not clobber.
			skipped_count++;
			continue;
		}
		SetEnvVar(entry.key.c_str(), entry.value.c_str());
		applied_count++;
	}

	if (log_changes) {
		Log::WriteToConsoleAndLog(
		    fmt::format("EngineProfile: {} (reason: {}), applied {} engine optimizations ({} user overrides preserved)\n",
		                result.engine_name, result.detected_reason, applied_count, skipped_count));
	}
}

} // namespace Common::EngineProfile
