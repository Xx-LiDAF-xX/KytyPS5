#include "common/runDiagnostics.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

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

std::filesystem::path LifecycleRoot() {
  return std::filesystem::temp_directory_path() /
         "kyty_run_diagnostics_lifecycle_tests";
}

nlohmann::json ReadJson(const std::filesystem::path &file) {
  std::ifstream input(file, std::ios::binary);
  Check(static_cast<bool>(input), "diagnostics report could not be opened");
  return nlohmann::json::parse(input);
}

void ResetLifecycleRoot() {
  Diagnostics::Shutdown();
  std::error_code error;
  std::filesystem::remove_all(LifecycleRoot(), error);
}

void TestDisabledCallsAreNoops() {
  ResetLifecycleRoot();
  const auto file = LifecycleRoot() / "disabled.json";

  Check(!Diagnostics::Enabled(), "diagnostics unexpectedly started enabled");
  Diagnostics::Record("disabled", "record", "must be ignored");
  Diagnostics::SetGpu("ignored", 1, 2, 3);
  Diagnostics::Finish(Diagnostics::Outcome::Fatal, "must be ignored");
  Diagnostics::Shutdown();

  Check(!std::filesystem::exists(file),
        "disabled diagnostics created a report");
}

void TestConcurrentEventsAreRetained() {
  ResetLifecycleRoot();
  const auto file = LifecycleRoot() / "concurrent.json";
  Diagnostics::Initialize(file, MakeReport().metadata);

  constexpr int kThreadCount = 4;
  constexpr int kEventsPerThread = 50;
  std::vector<std::thread> threads;
  for (int thread = 0; thread < kThreadCount; ++thread) {
    threads.emplace_back([thread]() {
      for (int event = 0; event < kEventsPerThread; ++event) {
        Diagnostics::Record("concurrency", std::to_string(thread),
                            std::to_string(event));
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  Diagnostics::Finish(Diagnostics::Outcome::Completed, "threads joined");

  const auto json = ReadJson(file);
  Check(json.at("events").size() == kThreadCount * kEventsPerThread,
        "concurrent events were lost");
  Diagnostics::Shutdown();
}

void TestFinishIsIdempotent() {
  ResetLifecycleRoot();
  const auto file = LifecycleRoot() / "idempotent.json";
  Diagnostics::Initialize(file, MakeReport().metadata);

  Diagnostics::Finish(Diagnostics::Outcome::Fatal, "first outcome");
  Diagnostics::Finish(Diagnostics::Outcome::Completed, "second outcome");

  const auto json = ReadJson(file);
  Check(json.at("outcome") == "fatal", "second finish replaced the outcome");
  Check(json.at("outcome_detail") == "first outcome",
        "second finish replaced the outcome detail");
  Diagnostics::Shutdown();
}

void TestFatalBeforeInitializeIsSafe() {
  ResetLifecycleRoot();
  Diagnostics::Finish(Diagnostics::Outcome::Fatal,
                      "fatal before initialization");
  Check(!Diagnostics::Enabled(),
        "finish before initialization enabled diagnostics");
}

void TestShutdownFlushesRunningReport() {
  ResetLifecycleRoot();
  const auto file = LifecycleRoot() / "shutdown.json";
  Diagnostics::Initialize(file, MakeReport().metadata);
  Diagnostics::Record("lifecycle", "initialized", "ready");
  Diagnostics::Shutdown();

  const auto json = ReadJson(file);
  Check(json.at("outcome") == "running",
        "shutdown changed an unfinished run outcome");
  Check(json.at("events").size() == 1,
        "shutdown did not flush recorded events");
  Check(!Diagnostics::Enabled(), "shutdown left diagnostics enabled");
}

void TestCheckpointPersistsRunningReport() {
  ResetLifecycleRoot();
  const auto file = LifecycleRoot() / "checkpoint.json";
  Diagnostics::Initialize(file, MakeReport().metadata);
  Diagnostics::Record("lifecycle", "guest_execution_started", "guest thread starting");
  Diagnostics::SetGpu("Test GPU", 0x1234, 0x5678, 9);
  Diagnostics::Checkpoint();

  const auto json = ReadJson(file);
  Check(json.at("outcome") == "running",
        "checkpoint changed the running outcome");
  Check(json.at("events").size() == 1 &&
            json.at("events").at(0).at("code") == "guest_execution_started",
        "checkpoint did not persist the latest event");
  Check(json.at("metadata").at("gpu").at("name") == "Test GPU",
        "checkpoint did not persist GPU metadata");
  Diagnostics::Shutdown();
}

} // namespace

int main() {
  TestSerializesRequiredMetadata();
  TestOmitsEmptyOptionalGpuFields();
  TestCreatesParentDirectoryAndReplacesFile();
  TestDoesNotSerializeSourcePaths();
  TestDisabledCallsAreNoops();
  TestConcurrentEventsAreRetained();
  TestFinishIsIdempotent();
  TestFatalBeforeInitializeIsSafe();
  TestShutdownFlushesRunningReport();
  TestCheckpointPersistsRunningReport();
  ResetLifecycleRoot();
  std::puts("RunDiagnosticsTests: all cases passed");
  return 0;
}
