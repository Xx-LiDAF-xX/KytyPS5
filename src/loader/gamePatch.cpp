#include "loader/gamePatch.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/elf.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace Loader::GamePatch {

namespace {

constexpr uint64_t kPageSize = 0x4000;

struct Write {
	uint64_t             source_address = 0;
	uint64_t             target_address = 0;
	std::vector<uint8_t> off;
	std::vector<uint8_t> on;
	bool                 ray_tracing_bypass = false;
};

struct Plan {
	std::string              title_id;
	std::string              version;
	std::string              process;
	std::vector<Write>       writes;
	std::vector<std::string> mod_names;
	std::vector<uint64_t>    cave_pages;
};

using Json = nlohmann::json;

std::unique_ptr<Plan> g_pending_plan;
std::unique_ptr<Plan> g_applied_plan;
std::vector<uint64_t> g_applied_cave_pages;

bool Fail(std::string* error, std::string message) {
	*error = std::move(message);
	return false;
}

bool ParseBytes(const std::string& text, std::vector<uint8_t>* bytes) {
	if (text.empty() || (text.size() % 2) != 0) {
		return false;
	}

	bytes->resize(text.size() / 2);
	for (size_t index = 0; index < bytes->size(); index++) {
		(*bytes)[index] = static_cast<uint8_t>(std::stoul(text.substr(index * 2, 2), nullptr, 16));
	}
	return true;
}

bool LoadPlan(const std::filesystem::path& path, Plan* plan, std::string* error) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return Fail(error, "could not read cheat file");
	}

	const auto root = Json::parse(file, nullptr, false);
	if (!root.is_object()) {
		return Fail(error, "expected a JSON object");
	}

	if (root.contains("id")) {
		plan->title_id = root["id"].get<std::string>();
	} else if (root.contains("title_id")) {
		plan->title_id = root["title_id"].get<std::string>();
	}

	if (root.contains("version")) {
		plan->version = root["version"].get<std::string>();
	} else if (root.contains("app_version")) {
		plan->version = root["app_version"].get<std::string>();
	}

	if (root.contains("process")) {
		plan->process = root["process"].get<std::string>();
	} else {
		plan->process = "eboot.bin";
	}

	if (!root.contains("mods") || !root["mods"].is_array()) {
		return Fail(error, "expected a mods array in patch JSON");
	}

	for (const auto& mod: root["mods"]) {
		if (!mod.value("enabled", true)) {
			continue;
		}
		const auto name = mod.value("name", "Patch Mod");
		plan->mod_names.push_back(name);

		if (mod.contains("memory") && mod["memory"].is_array()) {
			for (const auto& entry: mod["memory"]) {
				Write write;
				std::string offset_str = entry.value("offset", "");
				if (offset_str.empty()) offset_str = entry.value("address", "");
				if (offset_str.empty()) continue;
				write.source_address = std::stoull(offset_str, nullptr, 16);
				std::string off_str = entry.value("off", "");
				std::string on_str = entry.value("on", "");
				if (on_str.empty()) on_str = entry.value("value", "");
				if (off_str.empty() && !on_str.empty()) off_str = std::string(on_str.size(), '0');
				if (ParseBytes(off_str, &write.off) && ParseBytes(on_str, &write.on)) {
					plan->writes.push_back(std::move(write));
				}
			}
		} else if (mod.contains("address") || mod.contains("offset")) {
			std::string offset_str = mod.value("offset", "");
			if (offset_str.empty()) offset_str = mod.value("address", "");
			std::string on_str = mod.value("value", "");
			if (on_str.empty()) on_str = mod.value("on", "");
			std::string off_str = mod.value("off", "");
			if (off_str.empty() && !on_str.empty()) off_str = std::string(on_str.size(), '0');
			Write write;
			write.source_address = std::stoull(offset_str, nullptr, 16);
			if (ParseBytes(off_str, &write.off) && ParseBytes(on_str, &write.on)) {
				plan->writes.push_back(std::move(write));
			}
		}
	}
	return true;
}

bool ValidateGame(const Plan& plan, const Program* main_program, std::string* error) {
	if (main_program == nullptr || main_program->base_vaddr == 0) {
		return Fail(error, "main executable is not loaded");
	}
	std::string title_id;
	std::string version;
	if (!SystemContentParamSfoGetString("TITLE_ID", &title_id) ||
	    !SystemContentParamSfoGetString("APP_VER", &version) ||
	    !Common::EqualNoCase(main_program->file_name.filename().string(), plan.process) ||
	    !Common::EqualNoCase(title_id, plan.title_id)) {
		return Fail(error, "cheat file does not match the loaded game");
	}
	if (!plan.version.empty() && plan.version != "*" && version != plan.version &&
	    !version.starts_with(plan.version) && !plan.version.starts_with(version)) {
		return Fail(error, "cheat file version does not match the loaded game");
	}
	return true;
}

void Translate(const Program& program, uint64_t source_base, uint64_t source_address,
               uint64_t* target_address) {
	if (source_address >= source_base) {
		*target_address = program.base_vaddr + source_address - source_base;
		return;
	}
	*target_address = program.base_vaddr - (source_base - source_address);
}

bool IsInsideProgram(const Program& program, uint64_t address, size_t size) {
	return size != 0 && address >= program.base_vaddr &&
	       address + size <= program.base_vaddr + program.mapped_size;
}

bool IsZero(const std::vector<uint8_t>& bytes) {
	return std::all_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte == 0; });
}

bool TranslateAndValidate(Plan* plan, const Program& program, uint64_t source_base) {
	for (auto& write: plan->writes) {
		Translate(program, source_base, write.source_address, &write.target_address);
		if (!IsZero(write.off) &&
		    (!IsInsideProgram(program, write.target_address, write.off.size()) ||
		     std::memcmp(reinterpret_cast<const void*>(write.target_address), write.off.data(),
		                 write.off.size()) != 0)) {
			return false;
		}
	}
	return true;
}

bool Matches(Plan* plan, const Program& program, uint64_t* source_base) {
	const auto anchor = std::find_if(plan->writes.begin(), plan->writes.end(),
	                                 [](const Write& write) { return !IsZero(write.off); });
	if (anchor == plan->writes.end() || program.mapped_size < anchor->off.size()) {
		return false;
	}

	const auto* ehdr = program.elf->GetEhdr();
	const auto* phdr = program.elf->GetPhdr();
	for (Elf64_Half index = 0; index < ehdr->e_phnum; index++) {
		const auto& segment = phdr[index];
		if ((segment.p_type != PT_LOAD && segment.p_type != PT_OS_RELRO) ||
		    segment.p_filesz < anchor->off.size()) {
			continue;
		}

		const auto* begin = reinterpret_cast<const uint8_t*>(program.base_vaddr + segment.p_vaddr);
		const auto* end   = begin + segment.p_filesz;
		for (auto* current = begin; current < end;) {
			const auto* found = std::search(current, end, anchor->off.begin(), anchor->off.end());
			if (found == end) {
				break;
			}
			const auto rva = static_cast<uint64_t>(found - begin) + segment.p_vaddr;
			if (anchor->source_address >= rva) {
				*source_base = anchor->source_address - rva;
				if (TranslateAndValidate(plan, program, *source_base)) {
					return true;
				}
			}
			current = found + 1;
		}
	}
	return false;
}

void ReleaseCaves(std::vector<uint64_t>* cave_pages) {
	for (auto page = cave_pages->rbegin(); page != cave_pages->rend(); ++page) {
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(*page, kPageSize));
	}
	cave_pages->clear();
}

bool AllocateCave(Plan* plan, const Program& program, std::string* error) {
	std::vector<uint64_t> pages;
	for (const auto& write: plan->writes) {
		if (IsInsideProgram(program, write.target_address, write.off.size())) {
			continue;
		}
		if (!IsZero(write.off)) {
			return Fail(error, "non-zero write is outside the matched module");
		}

		const auto write_end = write.target_address + write.off.size();
		for (auto page = write.target_address & ~(kPageSize - 1); page < write_end;
		     page += kPageSize) {
			if (std::find(pages.begin(), pages.end(), page) == pages.end()) {
				pages.push_back(page);
			}
		}
	}

	for (const auto page: pages) {
		const auto allocated = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    page, kPageSize, Common::VirtualMemory::Mode::ExecuteReadWrite, "game_cheat_code_cave",
		    true);
		if (allocated != page) {
			if (allocated != 0) {
				Libs::LibKernel::Memory::FreeGuestMemory(allocated, kPageSize);
			}
			ReleaseCaves(&plan->cave_pages);
			return Fail(error, "could not allocate code-cave page");
		}
		plan->cave_pages.push_back(page);
	}
	return true;
}

bool ApplyWrites(Plan* plan, const Program& program, std::string* error) {
	if (!AllocateCave(plan, program, error)) {
		return false;
	}

	for (int external = 1; external >= 0; external--) {
		for (const auto& write: plan->writes) {
			if (write.ray_tracing_bypass && Config::RayTracingEnabled()) {
				continue;
			}
			if (static_cast<int>(
			        !IsInsideProgram(program, write.target_address, write.on.size())) != external) {
				continue;
			}
			std::memcpy(reinterpret_cast<void*>(write.target_address), write.on.data(),
			            write.on.size());
			if (!Common::VirtualMemory::FlushInstructionCache(write.target_address,
			                                                  write.on.size())) {
				return Fail(error, "could not finalize cheat write");
			}
		}
	}
	return true;
}

} // namespace

bool Apply(const std::filesystem::path& plan_path, Program* main_program,
           const std::vector<Program*>& programs) {
	Plan        plan;
	std::string error;
	if (!LoadPlan(plan_path, &plan, &error) || !ValidateGame(plan, main_program, &error)) {
		Log::WriteToConsoleAndLog(fmt::format("Game cheat error: {}\n", error));
		return false;
	}
	if (plan.writes.empty()) {
		return true;
	}

	g_pending_plan = std::make_unique<Plan>(std::move(plan));
	for (auto* program: programs) {
		if (!ApplyPending(program)) {
			return false;
		}
		if (g_pending_plan == nullptr) {
			return true;
		}
	}
	Log::WriteToConsoleAndLog("Game cheat: waiting for the matching module\n");
	return true;
}

bool ApplyAutoFixes(Program* main_program, const std::vector<Program*>& programs) {
	if (main_program == nullptr) return false;
	
	std::string title_id;
	if (!SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) {
		return false;
	}

	if (title_id == "PPSA21567" || title_id == "PPSA21564" || title_id == "PPSA01325") {
		Log::WriteToConsoleAndLog(fmt::format("Applying built-in Asobi Engine optimizations & stability fixes for {}...\n", title_id));
		
		Plan plan;
		plan.title_id = title_id;
		plan.process = main_program->file_name.filename().string();
		plan.mod_names = {Config::RayTracingEnabled()
		                      ? "Asobi stability fixes (guest RT renderer preserved)"
		                      : "Asobi stability fixes (guest RT/GI bypass enabled)"};

		auto add_write = [&plan](uint64_t addr, const char* off, const char* on,
		                         bool ray_tracing_bypass = false) {
			Write w;
			w.source_address = addr;
			w.ray_tracing_bypass = ray_tracing_bypass;
			ParseBytes(off, &w.off);
			ParseBytes(on, &w.on);
			plan.writes.push_back(std::move(w));
		};

		if (title_id == "PPSA21567" || title_id == "PPSA21564") {
			add_write(0x73eec3f, "4584f60f84f10800004531f64c8d3d8ec4a90141b430",
			          "4584f6e9f2080000904531f64c8d3d8ec4a90141b430", true);
			add_write(0x7108a40, "80bfe5050000000f857c1b0000", "e9841b00009090909090909090", true);
			add_write(0x7108a33, "0fb682a00700008887e4050000", "b80000000090908887e4050000", true);
			add_write(0x18f0552, "480f44f2", "480f46f2");
		} else if (title_id == "PPSA01325") {
			// Asobi Engine core memory stability fix for Astro's Playroom (PPSA01325).
			// Fixes a cmove bug that incorrectly assigns a NULL pointer and causes a memcpy access violation.
			// Expanded to 16 bytes to ensure a unique anchor for the Matches() function (source_base).
			add_write(0x1606e96, "ac0c000083782806480f44f2c5f82e04", "ac0c000083782806480f46f2c5f82e04");
			add_write(0x186a28a, "634885f6488d7610480f44f2488b1648", "634885f6488d7610480f46f2488b1648");
			add_write(0x19a79c4, "ba488d34114885d2480f44f27436488d", "ba488d34114885d2480f46f27436488d");
			add_write(0x19ad474, "488d74020c4885d2480f44f2418b540f", "488d74020c4885d2480f46f2418b540f");
			add_write(0x1ba35c7, "0848393a488d7210480f44f2eb044883", "0848393a488d7210480f46f2eb044883");
			add_write(0x1baed31, "c6000100004d85f6480f44f24801c645", "c6000100004d85f6480f46f24801c645");
			add_write(0x1baf057, "4885ff488b7c2410480f44f24801fe49", "4885ff488b7c2410480f46f24801fe49");
			add_write(0x1baf19a, "488b8c24a0000000480f44f24801ce49", "488b8c24a0000000480f46f24801ce49");
			add_write(0x1bb191d, "c6000100004885ff480f44f24c01f648", "c6000100004885ff480f46f24c01f648");
			add_write(0x1be9ed9, "d94989d44c0f44e6480f44f2ffc04983", "d94989d44c0f44e6480f46f2ffc04983");
			add_write(0x1bea269, "d94989d44c0f44e6480f44f2ffc04983", "d94989d44c0f44e6480f46f2ffc04983");
			Log::WriteToConsoleAndLog(fmt::format("Applying Astro's Playroom (PPSA01325) memory stability patch at 11 locations...\n"));
		}
		
		g_pending_plan = std::make_unique<Plan>(std::move(plan));
		for (auto* program: programs) {
			ApplyPending(program);
			if (g_pending_plan == nullptr) break;
		}
		return true;
	}

	return false;
}

bool ApplyPending(Program* program) {
	uint64_t source_base = 0;
	if (g_pending_plan == nullptr || program == nullptr || program->elf == nullptr ||
	    !Matches(g_pending_plan.get(), *program, &source_base)) {
		return true;
	}
	Log::WriteToConsoleAndLog(
	    fmt::format("Game cheat: matched {} with source base 0x{:x}\n",
	                program->file_name.filename().string(), source_base));

	std::string error;
	if (!ApplyWrites(g_pending_plan.get(), *program, &error)) {
		ReleaseCaves(&g_pending_plan->cave_pages);
		g_pending_plan.reset();
		Log::WriteToConsoleAndLog(fmt::format("Game cheat error: {}\n", error));
		return false;
	}

	for (const auto& name: g_pending_plan->mod_names) {
		Log::WriteToConsoleAndLog(fmt::format("Successfully applied cheat: {}\n", name));
	}
	g_applied_cave_pages = g_pending_plan->cave_pages;
	g_applied_plan = std::move(g_pending_plan);
	return true;
}

void ToggleRayTracingBypass(bool enable_raytracing) {
	if (g_applied_plan == nullptr) return;
	
	if (g_applied_plan->title_id == "PPSA21567" || g_applied_plan->title_id == "PPSA21564") {
		for (const auto& write: g_applied_plan->writes) {
			// Only built-in renderer bypasses participate, never stability fixes
			// or writes from a user-supplied cheat plan.
			if (!write.ray_tracing_bypass) continue;

			const auto& bytes = enable_raytracing ? write.off : write.on;
			std::memcpy(reinterpret_cast<void*>(write.target_address), bytes.data(), bytes.size());
			Common::VirtualMemory::FlushInstructionCache(write.target_address, bytes.size());
		}
	}
}

void Clear() {
	if (g_pending_plan != nullptr) {
		ReleaseCaves(&g_pending_plan->cave_pages);
	}
	g_pending_plan.reset();
	ReleaseCaves(&g_applied_cave_pages);
}

} // namespace Loader::GamePatch
