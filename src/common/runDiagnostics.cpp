#include "common/runDiagnostics.h"

#include <chrono>
#include <cmath>
#include <exception>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace Diagnostics {

using Json = nlohmann::ordered_json;

namespace {

struct LifecycleState {
	std::mutex                            mutex;
	bool                                  enabled  = false;
	bool                                  finished = false;
	std::filesystem::path                 file;
	RunReport                             report;
	std::chrono::steady_clock::time_point started;
};

LifecycleState& State() {
	static LifecycleState state;
	return state;
}

std::string ProgramLeaf(std::string_view program) {
	const auto separator = program.find_last_of("/\\");
	return std::string(separator == std::string_view::npos ? program : program.substr(separator + 1));
}

void WriteCurrentReport(const LifecycleState& state) {
	std::string ignored_error;
	(void)WriteReport(state.report, state.file, &ignored_error);
}

} // namespace

static const char* OutcomeName(Outcome outcome) {
	switch (outcome) {
		case Outcome::Running: return "running";
		case Outcome::Completed: return "completed";
		case Outcome::Fatal: return "fatal";
		case Outcome::HostException: return "host_exception";
		case Outcome::Timeout: return "timeout";
		case Outcome::LaunchError: return "launch_error";
	}
	return "launch_error";
}

std::string Serialize(const RunReport& report) {
	Json metadata = {
	    {"build_label", report.metadata.build_label},
	    {"platform", report.metadata.platform},
	    {"cpu_name", report.metadata.cpu_name},
	    {"title",
	     {{"id", report.metadata.title_id},
	      {"name", report.metadata.title_name},
	      {"content_version", report.metadata.content_version},
	      {"executable", report.metadata.executable_name}}},
	    {"configuration", report.metadata.configuration},
	};

	if (report.metadata.gpu.has_value()) {
		const auto& gpu = report.metadata.gpu.value();
		metadata["gpu"] = {
		    {"name", gpu.name},
		    {"vendor_id", gpu.vendor_id},
		    {"device_id", gpu.device_id},
		    {"driver_version", gpu.driver_version},
		};
	}

	Json events = Json::array();
	for (const auto& event: report.events) {
		events.push_back({
		    {"offset_micros", event.offset_micros},
		    {"thread_id", event.thread_id},
		    {"category", event.category},
		    {"code", event.code},
		    {"message", event.message},
		});
	}

	Json root = {
	    {"schema_version", 1},
	    {"metadata", std::move(metadata)},
	    {"outcome", OutcomeName(report.outcome)},
	    {"outcome_detail", report.outcome_detail},
	    {"events", std::move(events)},
	};
	if (report.presentation.has_value()) {
		const auto& presentation = report.presentation.value();
		root["presentation"] = {
		    {"presented_frames", presentation.presented_frames},
		    {"current_fps", presentation.current_fps},
		};
	}
	if (!report.unsupported_imports.entries.empty() ||
	    report.unsupported_imports.dropped_unique != 0) {
		Json entries = Json::array();
		for (const auto& entry: report.unsupported_imports.entries) {
			entries.push_back({
			    {"symbol", entry.symbol},
			    {"program", ProgramLeaf(entry.program)},
			    {"type", entry.type},
			    {"binding", entry.binding},
			    {"call_count", entry.call_count},
			});
		}
		root["unsupported_imports"] = {
		    {"entries", std::move(entries)},
		    {"dropped_unique", report.unsupported_imports.dropped_unique},
		};
	}
	return root.dump(2) + "\n";
}

static bool ReplaceFile(const std::filesystem::path& source,
                        const std::filesystem::path& destination, std::string* error) {
#if defined(_WIN32)
	if (MoveFileExW(source.c_str(), destination.c_str(),
	                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
		if (error != nullptr) {
			*error =
			    "failed to replace report file: Windows error " + std::to_string(GetLastError());
		}
		return false;
	}
#else
	std::error_code rename_error;
	std::filesystem::rename(source, destination, rename_error);
	if (rename_error) {
		if (error != nullptr) {
			*error = "failed to replace report file: " + rename_error.message();
		}
		return false;
	}
#endif
	return true;
}

bool WriteReport(const RunReport& report, const std::filesystem::path& file, std::string* error) {
	if (error != nullptr) {
		error->clear();
	}

	std::error_code directory_error;
	if (const auto parent = file.parent_path(); !parent.empty()) {
		std::filesystem::create_directories(parent, directory_error);
	}
	if (directory_error) {
		if (error != nullptr) {
			*error = "failed to create report directory: " + directory_error.message();
		}
		return false;
	}

	auto temporary = file;
	temporary += ".tmp";
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output) {
			if (error != nullptr) {
				*error = "failed to open temporary report file";
			}
			return false;
		}
		output << Serialize(report);
		output.flush();
		if (!output) {
			if (error != nullptr) {
				*error = "failed to write temporary report file";
			}
			return false;
		}
	}

	if (!ReplaceFile(temporary, file, error)) {
		std::error_code remove_error;
		std::filesystem::remove(temporary, remove_error);
		return false;
	}
	return true;
}

void Initialize(const std::filesystem::path& file, RunMetadata metadata) {
	metadata.executable_name =
	    std::filesystem::path(metadata.executable_name).filename().string();

	auto&           state = State();
	std::lock_guard lock(state.mutex);
	state.file            = file;
	state.report          = {};
	state.report.metadata = std::move(metadata);
	state.report.outcome  = Outcome::Running;
	state.started         = std::chrono::steady_clock::now();
	state.finished        = false;
	state.enabled         = true;
	WriteCurrentReport(state);
}

bool Enabled() noexcept {
	auto&           state = State();
	std::lock_guard lock(state.mutex);
	return state.enabled;
}

void Record(std::string_view category, std::string_view code, std::string_view message) noexcept {
	auto&           state = State();
	std::lock_guard lock(state.mutex);
	if (!state.enabled || state.finished) {
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	const auto offset =
	    std::chrono::duration_cast<std::chrono::microseconds>(now - state.started).count();
	const auto thread_id = std::hash<std::thread::id> {}(std::this_thread::get_id());
	state.report.events.push_back({static_cast<uint64_t>(offset),
	                               static_cast<uint64_t>(thread_id), std::string(category),
	                               std::string(code), std::string(message)});
}

uint64_t RecordUnsupportedImport(std::string_view symbol, std::string_view program,
                                 std::string_view type, std::string_view binding) noexcept {
	try {
		auto&           state = State();
		std::lock_guard lock(state.mutex);
		if (!state.enabled || state.finished) {
			return 0;
		}

		const auto program_leaf = ProgramLeaf(program);
		for (auto& entry: state.report.unsupported_imports.entries) {
			if (entry.symbol == symbol && entry.program == program_leaf && entry.type == type &&
			    entry.binding == binding) {
				if (entry.call_count != std::numeric_limits<uint64_t>::max()) {
					entry.call_count++;
				}
				return entry.call_count;
			}
		}

		constexpr size_t MaxUniqueEntries = 256;
		if (state.report.unsupported_imports.entries.size() >= MaxUniqueEntries) {
			if (state.report.unsupported_imports.dropped_unique !=
			    std::numeric_limits<uint64_t>::max()) {
				state.report.unsupported_imports.dropped_unique++;
			}
			return 0;
		}

		state.report.unsupported_imports.entries.push_back(
		    {std::string(symbol), program_leaf, std::string(type), std::string(binding), 1});
		return 1;
	} catch (...) {
		return 0;
	}
}

void SetGpu(std::string_view name, uint32_t vendor_id, uint32_t device_id,
            uint32_t driver_version) noexcept {
	auto&           state = State();
	std::lock_guard lock(state.mutex);
	if (!state.enabled || state.finished) {
		return;
	}
	state.report.metadata.gpu =
	    GpuMetadata {std::string(name), vendor_id, device_id, driver_version};
}

void UpdatePresentation(uint64_t presented_frames, double current_fps) noexcept {
	auto&           state = State();
	std::lock_guard lock(state.mutex);
	if (!state.enabled || state.finished) {
		return;
	}
	state.report.presentation = PresentationMetadata {
	    presented_frames,
	    std::isfinite(current_fps) && current_fps >= 0.0 ? current_fps : 0.0,
	};
	WriteCurrentReport(state);
}

void Checkpoint() noexcept {
	auto&           state = State();
	std::lock_guard lock(state.mutex);
	if (!state.enabled || state.finished) {
		return;
	}
	WriteCurrentReport(state);
}

void Finish(Outcome outcome, std::string_view detail) noexcept {
	auto&           state = State();
	std::lock_guard lock(state.mutex);
	if (!state.enabled || state.finished) {
		return;
	}
	state.report.outcome        = outcome;
	state.report.outcome_detail = std::string(detail);
	state.finished              = true;
	WriteCurrentReport(state);
}

void Shutdown() noexcept {
	auto&           state = State();
	std::lock_guard lock(state.mutex);
	if (!state.enabled) {
		return;
	}
	WriteCurrentReport(state);
	state.enabled  = false;
	state.finished = false;
	state.file.clear();
	state.report = {};
}

} // namespace Diagnostics
