#ifndef KYTY_COMMON_RUN_DIAGNOSTICS_H_
#define KYTY_COMMON_RUN_DIAGNOSTICS_H_

#include "common/common.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Diagnostics {

enum class Outcome { Running, Completed, Fatal, HostException, Timeout, LaunchError };

struct GpuMetadata {
	std::string name;
	uint32_t    vendor_id      = 0;
	uint32_t    device_id      = 0;
	uint32_t    driver_version = 0;
};

struct RunMetadata {
	std::string                        build_label;
	std::string                        platform;
	std::string                        cpu_name;
	std::string                        title_id;
	std::string                        title_name;
	std::string                        content_version;
	std::string                        executable_name;
	std::map<std::string, std::string> configuration;
	std::optional<GpuMetadata>         gpu;
};

struct Event {
	uint64_t    offset_micros = 0;
	uint64_t    thread_id     = 0;
	std::string category;
	std::string code;
	std::string message;
};

struct RunReport {
	RunMetadata        metadata;
	Outcome            outcome = Outcome::Running;
	std::string        outcome_detail;
	std::vector<Event> events;
};

[[nodiscard]] std::string Serialize(const RunReport& report);
[[nodiscard]] bool        WriteReport(const RunReport& report, const std::filesystem::path& file,
                                      std::string* error);

} // namespace Diagnostics

#endif /* KYTY_COMMON_RUN_DIAGNOSTICS_H_ */
