#include "common/threads.h"
#include "loader/runtimeLinker.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "RuntimeLinkerTests: failed: %s\n", message);
		std::abort();
	}
}

Loader::SymbolResolve MakeSymbol(const std::string& nid, const std::string& library,
	                             const std::string& module) {
	Loader::SymbolResolve symbol {};
	symbol.name                 = nid;
	symbol.library              = library;
	symbol.library_version      = 1;
	symbol.module               = module;
	symbol.module_version_major = 1;
	symbol.module_version_minor = 0;
	symbol.type                 = Loader::SymbolType::Func;
	return symbol;
}

void PopulateImporter(Loader::Program* program) {
	program->file_name    = "synthetic-eboot.bin";
	program->dynamic_info = std::make_unique<Loader::DynamicInfo>();
	program->dynamic_info->import_libs.push_back({"library-id", 1, "TargetLibrary"});
	program->dynamic_info->import_modules.push_back({"module-id", 1, 0, "TargetModule"});
}

void TestExactMetadataWinsOverFallback() {
	Loader::RuntimeLinker linker;
	Loader::Program       importer;
	PopulateImporter(&importer);
	linker.Symbols()->Add(MakeSymbol("exact-nid", "TargetLibrary", "TargetModule"), 0x1111,
	                      "ExactFunction");
	linker.Symbols()->Add(MakeSymbol("exact-nid", "OtherLibrary", "OtherModule"), 0x2222,
	                      "FallbackFunction");

	Loader::SymbolRecord result {};
	bool                 bind_self = true;
	linker.Resolve("exact-nid#library-id#module-id", Loader::SymbolType::Func, &importer, &result,
	               &bind_self);

	Check(result.vaddr == 0x1111, "exact metadata did not win over NID fallback");
	Check(result.dbg_name == "ExactFunction", "exact metadata returned the wrong record");
	Check(!bind_self, "built-in exact symbol was marked as self-bound");
}

void TestUniqueFallbackResolvesMissingExactRegistration() {
	Loader::RuntimeLinker linker;
	Loader::Program       importer;
	PopulateImporter(&importer);
	linker.Symbols()->Add(MakeSymbol("fallback-nid", "OtherLibrary", "OtherModule"), 0x3333,
	                      "FallbackFunction");

	Loader::SymbolRecord result {};
	linker.Resolve("fallback-nid#library-id#module-id", Loader::SymbolType::Func, &importer,
	               &result, nullptr);

	Check(result.vaddr == 0x3333, "unique NID fallback did not resolve");
	Check(result.dbg_name == "FallbackFunction", "unique fallback returned the wrong record");
}

void TestAmbiguousFallbackRemainsUnresolved() {
	Loader::RuntimeLinker linker;
	Loader::Program       importer;
	PopulateImporter(&importer);
	linker.Symbols()->Add(MakeSymbol("ambiguous-nid", "LibraryA", "ModuleA"), 0x4444);
	linker.Symbols()->Add(MakeSymbol("ambiguous-nid", "LibraryB", "ModuleB"), 0x5555);

	Loader::SymbolRecord result {};
	linker.Resolve("ambiguous-nid#library-id#module-id", Loader::SymbolType::Func, &importer,
	               &result, nullptr);

	Check(result.vaddr == 0, "ambiguous fallback resolved by insertion order");
	Check(result.dbg_name.empty(), "ambiguous fallback retained debug metadata");
}

void TestLoadedProgramCandidatesParticipateInAmbiguity() {
	Loader::RuntimeLinker linker;
	Loader::Program       importer;
	PopulateImporter(&importer);
	linker.Symbols()->Add(MakeSymbol("cross-database-nid", "BuiltInLibrary", "BuiltInModule"),
	                      0x6666);

	auto loaded_program            = std::make_unique<Loader::Program>();
	loaded_program->export_symbols = std::make_unique<Loader::SymbolDatabase>();
	loaded_program->export_symbols->Add(
	    MakeSymbol("cross-database-nid", "LoadedLibrary", "LoadedModule"), 0x7777);
	linker.AdoptTestProgram(std::move(loaded_program));

	Loader::SymbolRecord result {};
	linker.Resolve("cross-database-nid#library-id#module-id", Loader::SymbolType::Func, &importer,
	               &result, nullptr);

	Check(result.vaddr == 0, "cross-database ambiguity resolved by database order");
}

void TestMissingLibraryOrModuleRemainsUnresolved() {
	Loader::RuntimeLinker linker;
	Loader::Program       importer;
	importer.dynamic_info = std::make_unique<Loader::DynamicInfo>();

	Loader::SymbolRecord result {};
	bool                 bind_self = true;
	linker.Resolve("missing-nid#library-id#module-id", Loader::SymbolType::Func, &importer,
	               &result, &bind_self);

	Check(result.vaddr == 0, "missing metadata unexpectedly resolved");
	Check(result.name == "missing-nid#library-id#module-id",
	      "missing metadata did not preserve the original symbol name");
	Check(result.dbg_name.empty(), "missing metadata retained debug metadata");
	Check(!bind_self, "missing metadata was marked as self-bound");
}

void TestMalformedSymbolNameRemainsUnresolved() {
	Loader::RuntimeLinker linker;
	Loader::Program       importer;
	PopulateImporter(&importer);

	for (const std::string name: {"plain-nid", "nid#only-library", "nid#too#many#parts"}) {
		Loader::SymbolRecord result {};
		linker.Resolve(name, Loader::SymbolType::Func, &importer, &result, nullptr);
		Check(result.vaddr == 0, "malformed symbol name unexpectedly resolved");
		Check(result.name == name, "malformed symbol name was not preserved");
		Check(result.dbg_name.empty(), "malformed symbol retained debug metadata");
	}
}

} // namespace

int main() {
	Common::InitializeThreads();
	TestExactMetadataWinsOverFallback();
	TestUniqueFallbackResolvesMissingExactRegistration();
	TestAmbiguousFallbackRemainsUnresolved();
	TestLoadedProgramCandidatesParticipateInAmbiguity();
	TestMissingLibraryOrModuleRemainsUnresolved();
	TestMalformedSymbolNameRemainsUnresolved();
	std::puts("RuntimeLinkerTests: all cases passed");
	return 0;
}
