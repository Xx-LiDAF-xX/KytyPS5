#ifndef KYTY_COMMON_RUN_DIAGNOSTICS_H_
#define KYTY_COMMON_RUN_DIAGNOSTICS_H_

#include "common/common.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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

struct PresentationMetadata {
	uint64_t presented_frames = 0;
	double   current_fps      = 0.0;
};

struct UnsupportedImport {
	std::string symbol;
	std::string program;
	std::string type;
	std::string binding;
	uint64_t    call_count = 0;
};

struct UnsupportedImportSummary {
	std::vector<UnsupportedImport> entries;
	uint64_t                       dropped_unique = 0;
};

struct RunReport {
	RunMetadata                        metadata;
	std::optional<PresentationMetadata> presentation;
	UnsupportedImportSummary           unsupported_imports;
	Outcome                            outcome = Outcome::Running;
	std::string                        outcome_detail;
	std::vector<Event>                 events;
};

[[nodiscard]] std::string Serialize(const RunReport& report);
[[nodiscard]] bool        WriteReport(const RunReport& report, const std::filesystem::path& file,
                                      std::string* error);

void               Initialize(const std::filesystem::path& file, RunMetadata metadata);
[[nodiscard]] bool Enabled() noexcept;
void Record(std::string_view category, std::string_view code, std::string_view message) noexcept;
[[nodiscard]] uint64_t RecordUnsupportedImport(std::string_view symbol, std::string_view program,
                                               std::string_view type,
                                               std::string_view binding) noexcept;
void SetGpu(std::string_view name, uint32_t vendor_id, uint32_t device_id,
            uint32_t driver_version) noexcept;
void UpdatePresentation(uint64_t presented_frames, double current_fps) noexcept;
void Checkpoint() noexcept;
void Finish(Outcome outcome, std::string_view detail) noexcept;
void Shutdown() noexcept;

} // namespace Diagnostics

#endif /* KYTY_COMMON_RUN_DIAGNOSTICS_H_ */
