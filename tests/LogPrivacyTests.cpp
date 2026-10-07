#include "common/emulatorConfig.h"
#include "common/logging/log.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
void Check(bool value, const char* message) {
	if (!value) { std::cerr << "LogPrivacyTests: " << message << '\n'; std::exit(1); }
}
}

int main() {
	Check(Config::ConfigOptions {}.printf_direction == Config::LogDirection::File, "default is file");
	const std::string input =
	    "Loading C:\\Users\\private-person\\My Games\\eboot.bin\n"
	    "Loading C:/Users/private-person/Game/eboot.bin\n"
	    "Loading \\\\private-server\\share\\game.bin\n"
	    "Loading /home/private-person/game/eboot.bin\n"
	    "Loading ../private-folder/game.bin\n"
	    "email = person@example.com\npassword = secret\n"
	    "url = https://account.example/path?token=secret\n"
	    "peer = 192.168.1.25\nmac = aa:bb:cc:dd:ee:ff\n"
	    "Vulkan result=ErrorDeviceLost (-4), requested=123 known-gpu=100\n"
	    "driver=32.0.16.1714\n in masterSemaphore.cpp:53\n";
	const auto safe = Log::RedactPrivateInfo(input);
	for (const char* private_value : {"private-person", "private-server", "private-folder", "secret",
	                                 "person@example.com", "192.168.1.25", "aa:bb:cc:dd:ee:ff"}) {
		Check(safe.find(private_value) == std::string::npos, "private value survives filtering");
	}
	Check(safe.find("ErrorDeviceLost (-4), requested=123 known-gpu=100") != std::string::npos,
	      "Vulkan failure identity is preserved");
	Check(safe.find("masterSemaphore.cpp:53") != std::string::npos, "source location preserved");
	Check(safe.find("32.0.16.1714") != std::string::npos, "driver version preserved");
	const auto first = Log::ResolveOutputPath("_kyty.txt");
	const auto second = Log::ResolveOutputPath("_kyty.txt");
	Check(first != second && first.filename().string().starts_with("_kyty_"), "dated logs unique");
	Check(Log::ResolveOutputPath("chosen.txt") == "chosen.txt", "explicit custom filename preserved");
	const auto root = std::filesystem::temp_directory_path() / (first.stem().string() + "-test");
	std::filesystem::create_directories(root);
	Config::Initialize();
	Config::ConfigOptions options;
	options.printf_output_file = root / "result.txt";
	Config::Load(options);
	Log::Initialize();
	Log::Printf("Loading %s\n", "C:\\Users\\private-person\\game.bin");
	Log::WriteGuest("C:\\Us");
	Log::WriteGuest("ers\\private-person\\game.bin\n");
	Log::WriteFatal("failure at /Users/private-person/game\nresult=-4\n");
	Log::Shutdown();
	std::ifstream file(root / "result.txt", std::ios::binary);
	const std::string output((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	Check(output.find("private-person") == std::string::npos && output.find("result=-4") != std::string::npos,
	      "file and fatal output filtered");
	file.close();
	options.printf_output_file = root / "_kyty.txt";
	for (int run = 0; run < 2; ++run) {
		Config::Load(options);
		Log::Initialize();
		Log::Write("dated launch\n");
		Log::Shutdown();
	}
	Config::Shutdown();
	size_t dated_count = 0;
	for (const auto& entry : std::filesystem::directory_iterator(root)) {
		if (entry.path().filename().string().starts_with("_kyty_")) { ++dated_count; }
		std::filesystem::remove(entry.path());
	}
	Check(dated_count == 2, "successive launches create separate dated files");
	std::filesystem::remove(root);
	std::cout << "LogPrivacyTests: passed\n";
}
