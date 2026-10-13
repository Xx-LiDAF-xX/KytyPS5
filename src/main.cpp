#include "common/archive.h"
#include "common/common.h"
#include "common/dateTime.h"
#include "common/debug.h"
#include "common/file.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "emulator.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kytyGitVersion.h"
#include "spirv-tools/libspirv.hpp"

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <vector>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <nlohmann/json.hpp>

using namespace Common;
using namespace Emulator;

static std::string GetBuildString() {
	Date date = Date::FromMacros(std::string(__DATE__));

#if KYTY_BUILD == KYTY_BUILD_DEBUG
	std::string type = "Debug";
#elif KYTY_BUILD == KYTY_BUILD_RELEASE
	std::string type = "Release";
#else
	std::string type = "????";
#endif

	std::string compiler = Debug::GetCompiler() + "-" + Debug::GetLinker();

	std::string str =
	    fmt::format("{}, {}, ver = {}, git = {}, date = {}", type.c_str(), compiler.c_str(),
	                KYTY_VERSION, KYTY_GIT_VERSION, date.ToString().c_str());

	return str;
}

static void PrintUsage() {
	::printf("%s\n", GetBuildString().c_str());
	::printf("kyty_emulator --game <dir|elf|zar> [options]\n\n");
	::printf("Options:\n");
	::printf("  --game <dir|elf|zar>                 Game directory, ELF, or ZArchive to load.\n");
	::printf("  --game-patch <json>                  ETAHen cheat file.\n");
	::printf("  --screen-width <num>                 Window width. Default: 1280.\n");
	::printf("  --screen-height <num>                Window height. Default: 720.\n");
	::printf(
	    "  --user-name <name>                   Local user name (1-16 bytes). Default: Kyty.\n");
	::printf("  --user-id <num>                      Local user ID. Default: %d.\n",
	         Config::DEFAULT_USER_ID);
	::printf("  --mic <name>                        Capture from this microphone; omit for silence.\n");
	::printf("  --controller-color <#RRGGBB>        Override the controller lightbar color.\n");
	::printf("  --controller-volume <0-100>         DualSense speaker volume. Default: 50.\n");
	::printf("  --controller-vibration <0-100>      DualSense vibration intensity. Default: 100.\n");
	::printf(
	    "  --present-mode <value>               Fifo, Mailbox, or Immediate. Default: Mailbox.\n");
	::printf(
	    "  --gpu <index>                        Vulkan physical device index. Default: auto.\n");
	::printf("  --fullscreen                         Run in borderless desktop fullscreen.\n");
	::printf("  --hdr                                Enable HDR10 output when supported.\n");
	::printf(
	    "  --hide-cursor                        Hide the cursor after 2 s idle. Default: off.\n");
	::printf("  --no-splash                          Disable the startup splash screen.\n");
	::printf("  --vr                                 Enable the virtual VR headset.\n");
	::printf("  --amd-cpu                            Apply AMD CPU instruction patches.\n");
	::printf("  --vblank-frequency <num>             Virtual vblank frequency. Default: 60.\n");
	::printf("  --console-language <0-29>            Console language. Default: 1 (English US).\n");
	::printf("  --vulkan-validation <true|false>     Enable Vulkan validation.\n");
	::printf("  --network-connected <true|false>     Use the host local network (default: false).\n");
	::printf("  --gpu-assisted-validation <t|f>      Bounds-check shader accesses on the GPU.\n"
	         "                                       Implies --vulkan-validation; very slow.\n");
	::printf("  --shader-validation <true|false>     Enable shader validation.\n");
	::printf("  --tessellation                      Draw tessellation patches; skipped by default.\n");
	::printf("  --shader-optimization-type <value>   None, Size, or Performance.\n");
	::printf("  --shader-log-direction <value>       Silent, Console, or File.\n");
	::printf("  --shader-log-folder <path>           Shader log output folder.\n");
	::printf("  --command-buffer-dump <true|false>   Enable command buffer dumps.\n");
	::printf("  --command-buffer-dump-folder <path>  Command buffer dump folder.\n");
	::printf("  --graphics-debug-dump <true|false>   Enable graphics debug dumps.\n");
	::printf("  --printf-direction <value>           Silent, Console, or File.\n");
	::printf("  --printf-output-file <path>          Guest printf output file.\n");
	::printf("  --profile                            Enable the Tracy profiler.\n");
	::printf("  --spirv-debug-printf <true|false>    Enable SPIR-V debug printf.\n");
	::printf(
	    "  --readback-linear-images <true|false> Read back writable linear images on submit.\n");
	::printf(
	    "  --sync-raw-image-buffers <true|false> Synchronize raw reads of GPU images. Default: false.\n");
	::printf(
	    "  --trophy-notifications <true|false>   Show trophy unlock toasts and play their sound.\n");
	::printf("  --playgo-hack                       Use the supplied PlayGo stub fallback.\n");
	::printf(
	    "  --skip-notice-screen <true|false>    Skip startup logos and notices in supported games.\n"
	    "                                      Default: false.\n");
	::printf("  --diagnostics-output <json>          Write a structured compatibility report.\n");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	::printf("  --redzone                            Protect the guest SysV red zone.\n");
#endif
	::printf("  --keymap <Control=Input>             DualSense mapping; may be repeated.\n");
	::printf("  --recompile-shader <stage> <in.bin> [--out <out.spv>] [--disasm] [--no-validate]\n"
	         "                                       Recompile guest shader binary to SPIR-V.\n");
	::printf("  --replay-pm4 <stream.bin|stream.log> [--headless]\n"
	         "                                       Replay PM4 command buffer stream offline.\n");
	::printf("  --rd                                 Enable RenderDoc capture.\n");
}

static bool NextArg(int argc, char* argv[], int& index, std::string& out) {
	if (index + 1 >= argc) {
		return false;
	}

	index++;
	out = argv[index];
	return true;
}

static bool ParseBool(const std::string& value, bool& out) {
	if (Common::EqualNoCase(value, "true") || value == "1" || Common::EqualNoCase(value, "yes") ||
	    Common::EqualNoCase(value, "on")) {
		out = true;
		return true;
	}

	if (Common::EqualNoCase(value, "false") || value == "0" || Common::EqualNoCase(value, "no") ||
	    Common::EqualNoCase(value, "off")) {
		out = false;
		return true;
	}

	return false;
}

template <typename E>
static bool ParseEnum(const std::string& value, E& out) {
	auto enum_value = magic_enum::enum_cast<E>(value.c_str());
	if (!enum_value.has_value()) {
		return false;
	}

	out = enum_value.value();
	return true;
}

static bool ParseConsoleLanguage(const std::string& value, uint32_t& out) {
	uint32_t language = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), language);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    language > Config::MAX_CONSOLE_LANGUAGE) {
		return false;
	}
	out = language;
	return true;
}

static bool ParseUint32(const std::string& value, uint32_t& out) {
	uint32_t number   = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = number;
	return true;
}

static bool ParseControllerColor(const std::string& value, Config::ControllerColor& out) {
	if (value.size() != 7 || value[0] != '#') {
		return false;
	}
	uint32_t rgb = 0;
	auto [end, error] = std::from_chars(value.data() + 1, value.data() + value.size(), rgb, 16);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8),
	       static_cast<uint8_t>(rgb)};
	return true;
}

static bool ParseInt32(const std::string& value, int32_t& out) {
	int32_t number    = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = number;
	return true;
}

static bool ParseUserId(const std::string& value, int32_t& out) {
	int32_t user_id   = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), user_id);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    !Config::IsConfiguredUserIdValid(user_id)) {
		return false;
	}
	out = user_id;
	return true;
}

static bool ParseArgs(int argc, char* argv[], RunOptions& options, bool& show_help) {
	show_help = false;

	for (int i = 1; i < argc; i++) {
		std::string arg = std::string(argv[i]);
		std::string value;

		if (arg == "--help" || arg == "-h") {
			show_help = true;
			continue;
		}

		if (arg == "--rd") {
			options.config.renderdoc_enabled = true;
			continue;
		}

		if (arg == "--fullscreen") {
			options.config.fullscreen_enabled = true;
			continue;
		}

		if (arg == "--hdr") {
			options.config.hdr_enabled = true;
			continue;
		}

		if (arg == "--hide-cursor") {
			options.config.hide_cursor_enabled = true;
			continue;
		}

		if (arg == "--no-splash") {
			options.config.splash_enabled = false;
			continue;
		}

		if (arg == "--vr") {
			options.config.vr_enabled = true;
			continue;
		}

		if (arg == "--amd-cpu") {
			options.config.amd_cpu_enabled = true;
			continue;
		}

		if (arg == "--playgo-hack") {
			options.config.playgo_hack_enabled = true;
			continue;
		}

		if (arg == "--tessellation") {
			options.config.tessellation_enabled = true;
			continue;
		}

		if (arg == "--profile") {
			options.config.profiler_enabled = true;
			continue;
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (arg == "--redzone") {
			options.config.red_zone_protection_enabled = true;
			continue;
		}
#endif

		if (!arg.starts_with("--")) {
			::printf("game input must be provided with --game\n");
			return false;
		}

		if (!NextArg(argc, argv, i, value)) {
			::printf("missing value for %s\n", arg.c_str());
			return false;
		}

		if (arg == "--game") {
			if (!options.app0_dir.empty()) {
				::printf("--game can only be specified once\n");
				return false;
			}

			value           = Common::FixFilenameSlash(value);
			const auto path = Common::PathFromUtf8(value);

			if (Common::File::IsDirectoryExisting(path)) {
				options.app0_dir = path;
				options.elf      = "/app0/eboot.bin";
			} else if (Common::IsSupportedArchive(path) && Common::File::IsFileExisting(path)) {
				const auto root = Common::MakeArchivePath(path);
				if (!Common::File::IsFileExisting(root / "eboot.bin")) {
					::printf("Archive does not contain eboot.bin: %s\n", value.c_str());
					return false;
				}
				options.app0_dir = root;
				options.elf      = "/app0/eboot.bin";
			} else if (Common::File::IsFileExisting(path)) {
				options.app0_dir = path.parent_path();

				if (options.app0_dir.empty()) {
					options.app0_dir = ".";
				}

				options.elf = std::filesystem::path("/app0") / path.filename();
			} else {
				::printf("--game must point to an existing directory, ELF, or archive: %s\n",
				         value.c_str());
				return false;
			}
		} else if (arg == "--guest-exec") {
			options.elf = Common::PathFromUtf8(Common::FixFilenameSlash(value));
			options.config.guest_arguments.assign(argv + i + 1, argv + argc);
			break;
		} else if (arg == "--game-patch") {
			if (!options.game_patch.empty()) {
				::printf("--game-patch can only be specified once\n");
				return false;
			}
			value = Common::FixFilenameSlash(value);
			const auto path = Common::PathFromUtf8(value);

			if (!Common::File::IsFileExisting(path)) {
				::printf("--game-patch must point to an existing file: %s\n", value.c_str());
				return false;
			}
			options.game_patch = path;
		} else if (arg == "--screen-width") {
			if (!ParseUint32(value, options.config.screen_width) ||
			    options.config.screen_width == 0) {
				::printf("invalid screen width: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--screen-height") {
			if (!ParseUint32(value, options.config.screen_height) ||
			    options.config.screen_height == 0) {
				::printf("invalid screen height: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--user-name") {
			if (value.empty() || value.size() > Config::MAX_USER_NAME_LENGTH) {
				::printf("invalid user name: must contain 1-%zu bytes\n",
				         Config::MAX_USER_NAME_LENGTH);
				return false;
			}
			options.config.user_name = value;
		} else if (arg == "--user-id") {
			if (!ParseUserId(value, options.config.user_id)) {
				::printf("invalid user ID: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--mic") {
			options.config.audio_input_device = value;
		} else if (arg == "--controller-color") {
			Config::ControllerColor color {};
			if (!ParseControllerColor(value, color)) {
				::printf("invalid controller color (expected #RRGGBB): %s\n", value.c_str());
				return false;
			}
			options.config.controller_color = color;
		} else if (arg == "--controller-volume") {
			if (!ParseUint32(value, options.config.controller_speaker_volume) ||
			    options.config.controller_speaker_volume > 100) {
				::printf("invalid controller volume: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--controller-vibration") {
			if (!ParseUint32(value, options.config.controller_vibration_intensity) ||
			    options.config.controller_vibration_intensity > 100) {
				::printf("invalid controller vibration intensity: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--present-mode") {
			if (!ParseEnum(value, options.config.present_mode)) {
				::printf("invalid present mode: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--gpu") {
			if (!ParseInt32(value, options.config.gpu_index)) {
				::printf("invalid gpu index: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--vblank-frequency") {
			if (!ParseUint32(value, options.config.vblank_frequency)) {
				::printf("invalid vblank frequency: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--console-language") {
			if (!ParseConsoleLanguage(value, options.config.console_language)) {
				::printf("invalid console language: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--network-connected") {
			if (!ParseBool(value, options.config.network_connected)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--vulkan-validation") {
			if (!ParseBool(value, options.config.vulkan_validation_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--gpu-assisted-validation") {
			if (!ParseBool(value, options.config.gpu_assisted_validation_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--shader-validation") {
			if (!ParseBool(value, options.config.shader_validation_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--shader-optimization-type") {
			if (!ParseEnum(value, options.config.shader_optimization_type)) {
				::printf("invalid shader optimization type: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--shader-log-direction") {
			if (!ParseEnum(value, options.config.shader_log_direction)) {
				::printf("invalid shader log direction: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--shader-log-folder") {
			options.config.shader_log_folder = Common::PathFromUtf8(value);
		} else if (arg == "--command-buffer-dump") {
			if (!ParseBool(value, options.config.command_buffer_dump_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--command-buffer-dump-folder") {
			options.config.command_buffer_dump_folder = Common::PathFromUtf8(value);
		} else if (arg == "--graphics-debug-dump") {
			if (!ParseBool(value, options.config.graphics_debug_dump_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--printf-direction") {
			if (!ParseEnum(value, options.config.printf_direction)) {
				::printf("invalid printf direction: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--printf-output-file") {
			options.config.printf_output_file = Common::PathFromUtf8(value);
		} else if (arg == "--spirv-debug-printf") {
			if (!ParseBool(value, options.config.spirv_debug_printf_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--readback-linear-images") {
			if (!ParseBool(value, options.config.readback_linear_images)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--sync-raw-image-buffers") {
			if (!ParseBool(value, options.config.sync_raw_image_buffers)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--trophy-notifications") {
			if (!ParseBool(value, options.config.trophy_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--skip-notice-screen") {
			if (!ParseBool(value, options.config.skip_notice_screen)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--diagnostics-output") {
			if (value.empty()) {
				::printf("diagnostics output path must not be empty\n");
				return false;
			}
			options.config.diagnostics_output_file = Common::PathFromUtf8(value);
		} else if (arg == "--keymap") {
			const auto split = value.find('=');
			if (split == std::string::npos || split == 0 || split + 1 == value.size()) {
				::printf("invalid keymap: %s\n", value.c_str());
				return false;
			}
			options.config.keymap.push_back(value);
		} else {
			::printf("unknown option: %s\n", arg.c_str());
			return false;
		}
	}

	if (options.config.gpu_assisted_validation_enabled) {
		options.config.vulkan_validation_enabled = true;
	}

	return show_help || (!options.app0_dir.empty() && !options.elf.empty());
}

static int HandleRecompileShader(int argc, char* argv[], int opt_index) {
	if (opt_index + 2 >= argc) {
		::fprintf(stderr, "Error: --recompile-shader requires <stage> and <input.bin>\n");
		return 1;
	}

	std::string stage_str = argv[opt_index + 1];
	std::string input_path = argv[opt_index + 2];
	std::string output_path;
	bool do_disasm = false;
	bool do_validate = true;

	for (int i = opt_index + 3; i < argc; ++i) {
		std::string_view a = argv[i];
		if (a == "--out" && i + 1 < argc) {
			output_path = argv[++i];
		} else if (a == "--disasm") {
			do_disasm = true;
		} else if (a == "--validate") {
			do_validate = true;
		} else if (a == "--no-validate") {
			do_validate = false;
		}
	}

	using namespace Libs::Graphics;
	ShaderType stage_type = ShaderType::Compute;
	if (Common::EqualNoCase(stage_str, "cs") || Common::EqualNoCase(stage_str, "compute")) {
		stage_type = ShaderType::Compute;
	} else if (Common::EqualNoCase(stage_str, "vs") || Common::EqualNoCase(stage_str, "vertex")) {
		stage_type = ShaderType::Vertex;
	} else if (Common::EqualNoCase(stage_str, "ps") || Common::EqualNoCase(stage_str, "pixel")) {
		stage_type = ShaderType::Pixel;
	} else {
		::fprintf(stderr, "Error: unknown shader stage '%s'\n", stage_str.c_str());
		return 1;
	}

	const auto in_p = Common::PathFromUtf8(input_path);
	if (!Common::File::IsFileExisting(in_p)) {
		::fprintf(stderr, "Error: input file does not exist: %s\n", input_path.c_str());
		return 1;
	}

	Common::File in_file(in_p, Common::File::Mode::Read);
	if (in_file.IsInvalid() || in_file.Size() == 0) {
		::fprintf(stderr, "Error: failed to open input file or file is empty: %s\n", input_path.c_str());
		return 1;
	}

	std::vector<uint32_t> code(in_file.Size() / sizeof(uint32_t));
	in_file.Read(code.data(), in_file.Size());

	static const ShaderVertexInputInfo vertex_info{};
	static const ShaderPixelInputInfo pixel_info{};
	static const ShaderComputeInputInfo compute_info{};
	static const auto user_data = [] {
		std::array<uint32_t, 64> data{};
		data[3] = 3u << 28u;
		return data;
	}();

	ShaderRecompiler::CompileOptions options;
	options.stage = stage_type;
	options.user_data = user_data;
	options.dump_ir = false;
	options.dump_label = "recompile_shader_cli";
	switch (stage_type) {
		case ShaderType::Vertex: options.input_info.vertex = &vertex_info; break;
		case ShaderType::Pixel: options.input_info.pixel = &pixel_info; break;
		case ShaderType::Compute: options.input_info.compute = &compute_info; break;
		default: break;
	}

	static auto StandaloneReadMemory = [](void*, uint64_t address, std::span<uint32_t> values) -> bool {
		if (address < 0x1000 || values.empty()) {
			std::ranges::fill(values, 0u);
			return true;
		}
		return false;
	};

	auto translated = ShaderRecompiler::TranslateProgram(code, options);
	auto plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
	ShaderRecompiler::IR::ResourceSnapshot resources;
	ShaderRecompiler::IR::ResourceSpecialization specialization;
	const ShaderRecompiler::IR::SrtRuntime runtime{
		.user_data = options.user_data,
		.shader_base = reinterpret_cast<uint64_t>(code.data()),
		.read_memory = StandaloneReadMemory,
		.userdata = nullptr,
		.read_specialization_memory = StandaloneReadMemory,
	};
	ShaderRecompiler::IR::MaterializeResources(plan, runtime, resources, specialization);
	auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, specialization, 0);

	if (do_validate) {
		spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
		std::string val_messages;
		tools.SetMessageConsumer([&val_messages](spv_message_level_t, const char*,
		                                         const spv_position_t& pos, const char* msg) {
			val_messages += fmt::format("{}:{}: {}\n", pos.line, pos.column, msg);
		});
		if (!tools.Validate(compiled.spirv)) {
			::fprintf(stderr, "Error: SPIR-V validation failed:\n%s\n", val_messages.c_str());
			return 2;
		}
	}

	if (do_disasm) {
		spvtools::SpirvTools disasm(SPV_ENV_VULKAN_1_3);
		std::string text;
		disasm.Disassemble(compiled.spirv, &text,
		                   SPV_BINARY_TO_TEXT_OPTION_NO_HEADER |
		                   SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES |
		                   SPV_BINARY_TO_TEXT_OPTION_INDENT);
		::printf("--- SPIR-V Disassembly ---\n%s\n", text.c_str());
	}

	if (!output_path.empty()) {
		const auto out_p = Common::PathFromUtf8(output_path);
		Common::File::CreateDirectories(out_p.parent_path());
		Common::File out_file(out_p);
		if (out_file.IsInvalid()) {
			::fprintf(stderr, "Error: cannot create output file: %s\n", output_path.c_str());
			return 1;
		}
		out_file.Write(compiled.spirv.data(), compiled.spirv.size() * sizeof(uint32_t));
	}

	nlohmann::json doc;
	doc["status"] = "ok";
	doc["stage"] = stage_str;
	doc["input_size_bytes"] = code.size() * sizeof(uint32_t);
	doc["output_size_dwords"] = compiled.spirv.size();
	doc["output_size_bytes"] = compiled.spirv.size() * sizeof(uint32_t);

	::printf("%s\n", doc.dump().c_str());
	return 0;
}

static std::vector<uint32_t> ParseDwordsFromLog(const std::string& text) {
	std::vector<uint32_t> dwords;
	size_t pos = 0;
	while ((pos = text.find('|', pos)) != std::string::npos) {
		size_t next_pipe = text.find('|', pos + 1);
		if (next_pipe == std::string::npos) {
			break;
		}
		std::string segment = text.substr(pos + 1, next_pipe - pos - 1);
		size_t hex_pos = segment.find("0x");
		if (hex_pos != std::string::npos && hex_pos + 10 <= segment.size()) {
			uint32_t val = 0;
			auto res = std::from_chars(segment.data() + hex_pos + 2, segment.data() + hex_pos + 10, val, 16);
			if (res.ec == std::errc{}) {
				dwords.push_back(val);
			}
		}
		pos = next_pipe + 1;
	}
	return dwords;
}

static int HandleReplayPm4(int argc, char* argv[], int opt_index) {
	if (opt_index + 1 >= argc) {
		::fprintf(stderr, "Error: --replay-pm4 requires <stream.bin|stream.log>\n");
		return 1;
	}

	std::string input_path = argv[opt_index + 1];
	bool is_headless = true;
	for (int i = opt_index + 2; i < argc; ++i) {
		std::string_view a = argv[i];
		if (a == "--headless") {
			is_headless = true;
		}
	}

	const auto in_p = Common::PathFromUtf8(input_path);
	if (!Common::File::IsFileExisting(in_p)) {
		::fprintf(stderr, "Error: input stream file does not exist: %s\n", input_path.c_str());
		nlohmann::json err_doc;
		err_doc["status"] = "error";
		err_doc["error"] = "Input stream file does not exist";
		::printf("%s\n", err_doc.dump().c_str());
		return 1;
	}

	Common::File in_file(in_p, Common::File::Mode::Read);
	if (in_file.IsInvalid() || in_file.Size() == 0) {
		::fprintf(stderr, "Error: failed to open input stream or file is empty: %s\n", input_path.c_str());
		nlohmann::json err_doc;
		err_doc["status"] = "error";
		err_doc["error"] = "Failed to open input stream or file is empty";
		::printf("%s\n", err_doc.dump().c_str());
		return 1;
	}

	std::vector<uint32_t> dwords;
	if (Common::EqualNoCase(in_p.extension().string(), ".log")) {
		std::string text(in_file.Size(), '\0');
		in_file.Read(text.data(), text.size());
		dwords = ParseDwordsFromLog(text);
	} else {
		uint32_t magic = 0;
		if (in_file.Size() >= 16) {
			in_file.Read(&magic, sizeof(uint32_t));
			if (magic == 0x344D504Bu /* 'KPM4' */) {
				uint32_t version = 0, dword_count = 0, flags = 0;
				in_file.Read(&version, sizeof(uint32_t));
				in_file.Read(&dword_count, sizeof(uint32_t));
				in_file.Read(&flags, sizeof(uint32_t));
				dwords.resize(dword_count);
				in_file.Read(dwords.data(), dword_count * sizeof(uint32_t));
			} else {
				in_file.Seek(0);
				dwords.resize(in_file.Size() / sizeof(uint32_t));
				in_file.Read(dwords.data(), in_file.Size());
			}
		} else {
			dwords.resize(in_file.Size() / sizeof(uint32_t));
			in_file.Read(dwords.data(), in_file.Size());
		}
	}

	if (dwords.empty()) {
		nlohmann::json err_doc;
		err_doc["status"] = "error";
		err_doc["error"] = "No dwords extracted from stream file";
		::printf("%s\n", err_doc.dump().c_str());
		return 1;
	}

	Libs::Graphics::Pm4::HardwareContext hw_ctx;
	bool success = Libs::Graphics::Pm4::ReplayPm4PacketStream(dwords.data(), static_cast<uint32_t>(dwords.size()), &hw_ctx);

	nlohmann::json doc;
	doc["status"] = success ? "ok" : "error";
	doc["headless"] = is_headless;
	doc["dwords_read"] = dwords.size();
	doc["packets_parsed"] = hw_ctx.stats.packets_parsed;
	doc["type3_packets"] = hw_ctx.stats.type3_packets;
	doc["set_sh_reg_packets"] = hw_ctx.stats.set_sh_reg_packets;
	doc["set_context_reg_packets"] = hw_ctx.stats.set_context_reg_packets;
	doc["set_uconfig_reg_packets"] = hw_ctx.stats.set_uconfig_reg_packets;
	doc["draw_packets"] = hw_ctx.stats.draw_packets;
	doc["dispatch_packets"] = hw_ctx.stats.dispatch_packets;
	doc["nop_packets"] = hw_ctx.stats.nop_packets;
	if (!success) {
		doc["error"] = hw_ctx.stats.last_error;
	}

	::printf("%s\n", doc.dump().c_str());
	return success ? 0 : 1;
}

static int Main(int argc, char* argv[]) {
	VirtualMemory::Init();
	InitializeThreads();

	for (int i = 1; i < argc; ++i) {
		if (std::string_view(argv[i]) == "--recompile-shader") {
			return HandleRecompileShader(argc, argv, i);
		}
		if (std::string_view(argv[i]) == "--replay-pm4") {
			return HandleReplayPm4(argc, argv, i);
		}
	}

	RunOptions options;
	bool       show_help = false;

	if (argc < 2) {
		PrintUsage();
		return 0;
	}

	if (!ParseArgs(argc, argv, options, show_help)) {
		PrintUsage();
		return 1;
	}

	if (show_help) {
		PrintUsage();
		return 0;
	}

	Run(options);

	return 0;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> utf8_args;
    utf8_args.reserve(static_cast<size_t>(argc));

    for (int index = 0; index < argc; index++) {
        const std::wstring_view wide(argv[index]);
        const std::u16string utf16(wide.begin(), wide.end());

        utf8_args.push_back(Common::Utf16ToUtf8(utf16));
    }

    std::vector<char*> utf8_argv;
    utf8_argv.reserve(utf8_args.size());

    for (auto& argument: utf8_args) {
        utf8_argv.push_back(argument.data());
    }

    return Main(argc, utf8_argv.data());
}

#else

int main(int argc, char* argv[]) {
    return Main(argc, argv);
}

#endif
