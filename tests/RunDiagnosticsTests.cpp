#include "common/runDiagnostics.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "RunDiagnosticsTests: failed: %s\n", message);
    std::abort();
  }
}

Diagnostics::RunReport MakeReport() {
  Diagnostics::RunReport report{};
  report.metadata.build_label = "KytyPS5-test";
  report.metadata.platform = "Windows";
  report.metadata.cpu_name = "Test CPU";
  report.metadata.title_id = "PPSA00000";
  report.metadata.title_name = "Synthetic title";
  report.metadata.content_version = "01.000.000";
  report.metadata.executable_name = "eboot.bin";
  report.metadata.configuration.emplace("present_mode", "Mailbox");
  report.outcome = Diagnostics::Outcome::Completed;
  report.outcome_detail = "synthetic completion";
  report.events.push_back({42, 7, "lifecycle", "ready", "synthetic event"});
  return report;
}

void TestSerializesRequiredMetadata() {
  const auto json = nlohmann::json::parse(Diagnostics::Serialize(MakeReport()));

  Check(json.at("schema_version") == 1, "schema version is not 1");
  Check(json.at("metadata").at("build_label") == "KytyPS5-test",
        "build label changed");
  Check(json.at("metadata").at("title").at("id") == "PPSA00000",
        "title id changed");
  Check(json.at("metadata").at("title").at("executable") == "eboot.bin",
        "executable leaf changed");
  Check(json.at("metadata").at("configuration").at("present_mode") == "Mailbox",
        "configuration changed");
  Check(json.at("outcome") == "completed",
        "outcome was not serialized by name");
  Check(json.at("events").size() == 1, "event was not serialized");
  Check(json.at("events").at(0).at("offset_micros") == 42,
        "event offset changed");
}

void TestOmitsEmptyOptionalGpuFields() {
  const auto json = nlohmann::json::parse(Diagnostics::Serialize(MakeReport()));

  Check(!json.at("metadata").contains("gpu"),
        "empty GPU metadata was serialized");
}

void TestCreatesParentDirectoryAndReplacesFile() {
  const auto root =
      std::filesystem::temp_directory_path() / "kyty_run_diagnostics_tests";
  const auto file = root / "nested" / "report.json";
  std::error_code error;
  std::filesystem::remove_all(root, error);

  std::string write_error;
  auto report = MakeReport();
  Check(Diagnostics::WriteReport(report, file, &write_error),
        write_error.c_str());
  Check(std::filesystem::is_regular_file(file), "report file was not created");

  report.outcome_detail = "replacement";
  Check(Diagnostics::WriteReport(report, file, &write_error),
        write_error.c_str());
  std::ifstream input(file, std::ios::binary);
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  const auto json = nlohmann::json::parse(contents);
  Check(json.at("outcome_detail") == "replacement",
        "existing report was not replaced");
  Check(!std::filesystem::exists(file.string() + ".tmp"),
        "temporary file was left behind");

  std::filesystem::remove_all(root, error);
}

void TestDoesNotSerializeSourcePaths() {
  auto report = MakeReport();
  const std::string private_path = "C:/Users/example/private/game/eboot.bin";
  report.metadata.executable_name =
      std::filesystem::path(private_path).filename().string();
  const auto serialized = Diagnostics::Serialize(report);

  Check(serialized.find(private_path) == std::string::npos,
        "absolute executable path was serialized");
  Check(serialized.find("eboot.bin") != std::string::npos,
        "sanitized executable leaf was not serialized");
}

} // namespace

int main() {
  TestSerializesRequiredMetadata();
  TestOmitsEmptyOptionalGpuFields();
  TestCreatesParentDirectoryAndReplacesFile();
  TestDoesNotSerializeSourcePaths();
  std::puts("RunDiagnosticsTests: all cases passed");
  return 0;
}
