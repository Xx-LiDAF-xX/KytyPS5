#include "loader/symbolDatabase.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "SymbolDatabaseTests: failed: %s\n", message);
		std::abort();
	}
}

Loader::SymbolResolve MakeSymbol(const std::string& nid, const std::string& library,
	                             Loader::SymbolType type = Loader::SymbolType::Func) {
	Loader::SymbolResolve symbol {};
	symbol.name                 = nid;
	symbol.library              = library;
	symbol.library_version      = 1;
	symbol.module               = "TestModule";
	symbol.module_version_major = 1;
	symbol.module_version_minor = 0;
	symbol.type                 = type;
	return symbol;
}

void TestUniqueNidReturnsRecord() {
	Loader::SymbolDatabase database;
	database.Add(MakeSymbol("unique-nid", "LibraryA"), 0x1000, "UniqueFunction");

	const auto result = database.FindUniqueByNid("unique-nid", Loader::SymbolType::Func);
	Check(result.status == Loader::NidLookupStatus::Unique, "unique NID was not unique");
	Check(result.record != nullptr, "unique NID returned no record");
	Check(result.record->vaddr == 0x1000, "unique NID returned the wrong address");
}

void TestDuplicateNidIsAmbiguous() {
	Loader::SymbolDatabase database;
	database.Add(MakeSymbol("duplicate-nid", "LibraryA"), 0x1000);
	database.Add(MakeSymbol("duplicate-nid", "LibraryB"), 0x2000);

	const auto result = database.FindUniqueByNid("duplicate-nid", Loader::SymbolType::Func);
	Check(result.status == Loader::NidLookupStatus::Ambiguous,
	      "duplicate NID did not report ambiguity");
	Check(result.record == nullptr, "ambiguous NID retained an arbitrary record");
}

void TestTypeAndAddressFiltersDisambiguate() {
	Loader::SymbolDatabase database;
	database.Add(MakeSymbol("filtered-nid", "LibraryA"), 0x1000);
	database.Add(MakeSymbol("filtered-nid", "LibraryB"), 0x2000);
	database.Add(MakeSymbol("filtered-nid", "LibraryC", Loader::SymbolType::Object), 0x3000);

	const auto functions = database.FindUniqueByNid("filtered-nid", Loader::SymbolType::Func);
	Check(functions.status == Loader::NidLookupStatus::Ambiguous,
	      "type filter hid function ambiguity");

	const auto function_at_address =
	    database.FindUniqueByNid("filtered-nid", Loader::SymbolType::Func, 0x2000);
	Check(function_at_address.status == Loader::NidLookupStatus::Unique,
	      "address filter did not select a unique function");
	Check(function_at_address.record != nullptr && function_at_address.record->vaddr == 0x2000,
	      "address filter selected the wrong function");

	const auto object = database.FindUniqueByNid("filtered-nid", Loader::SymbolType::Object);
	Check(object.status == Loader::NidLookupStatus::Unique,
	      "type filter did not select a unique object");
	Check(object.record != nullptr && object.record->vaddr == 0x3000,
	      "type filter selected the wrong object");
}

void TestMissingNidIsNotFound() {
	Loader::SymbolDatabase database;
	const auto result = database.FindUniqueByNid("missing-nid", Loader::SymbolType::Func);
	Check(result.status == Loader::NidLookupStatus::NotFound, "missing NID had the wrong status");
	Check(result.record == nullptr, "missing NID returned a record");
}

void TestExactFindBehaviorIsUnchanged() {
	Loader::SymbolDatabase database;
	const auto first  = MakeSymbol("exact-nid", "LibraryA");
	const auto second = MakeSymbol("exact-nid", "LibraryB");
	database.Add(first, 0x1000);
	database.Add(second, 0x2000);

	const auto* first_record  = database.Find(first);
	const auto* second_record = database.Find(second);
	Check(first_record != nullptr && first_record->vaddr == 0x1000,
	      "exact lookup changed for the first library");
	Check(second_record != nullptr && second_record->vaddr == 0x2000,
	      "exact lookup changed for the second library");
}

} // namespace

int main() {
	TestUniqueNidReturnsRecord();
	TestDuplicateNidIsAmbiguous();
	TestTypeAndAddressFiltersDisambiguate();
	TestMissingNidIsNotFound();
	TestExactFindBehaviorIsUnchanged();
	std::puts("SymbolDatabaseTests: all cases passed");
	return 0;
}
