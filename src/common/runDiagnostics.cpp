#include "common/runDiagnostics.h"

#include <exception>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace Diagnostics {

using Json = nlohmann::ordered_json;

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

	try {
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
	} catch (const std::exception& exception) {
		if (error != nullptr) {
			*error = exception.what();
		}
		return false;
	}
}

} // namespace Diagnostics
