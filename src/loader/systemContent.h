#pragma once

#include "common/abi.h"
#include "common/common.h"
#include "common/stringUtils.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Loader {

void                                     SystemContentLoadParamSfo(const std::filesystem::path& file_name);
[[nodiscard]] std::optional<int32_t>     SystemContentParamSfoGetInt(const char* name);
bool                                     SystemContentParamSfoGetInt(const char* name, int32_t* value);
[[nodiscard]] std::optional<std::string> SystemContentParamSfoGetString(const char* name);
bool                                     SystemContentParamSfoGetString(const char* name, std::string* value);
bool                                     SystemContentParamSfoGetString(const char* name, char* value, size_t value_size);
[[nodiscard]] std::string                SystemContentResolveTitleId();
[[nodiscard]] uint64_t                   SystemContentGetFlexibleMemorySize();
bool                                     SystemContentGetIconPath(std::filesystem::path* path);
bool                                     SystemContentGetChunksNum(uint32_t* num);

} // namespace Loader
