#pragma once

#include "common/emulatorConfig.h"
#include "common/stringUtils.h"

#include <filesystem>

namespace Emulator {

struct RunOptions {
	Config::ConfigOptions config;
	std::filesystem::path app0_dir;
	std::filesystem::path elf;
	std::filesystem::path game_patch;
	std::filesystem::path addcont_dir;
};

void              Run(const RunOptions& options);
[[nodiscard]] int LoadExec(const char* path, const char* const argv[]);

} // namespace Emulator
