#include "common/environment.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "EnvironmentTests: failed: %s\n", message);
		std::exit(1);
	}
}

void TestFlagSemantics() {
	struct Case {
		const char* value;
		bool        default_off;
		bool        default_on;
		bool        nonempty;
	};
	constexpr std::array cases {
	    Case {nullptr, false, true, false}, Case {"", true, true, false},
	    Case {"0", false, false, false},    Case {"1", true, true, true},
	    Case {"00", true, true, true},      Case {"01", true, true, true},
	    Case {"false", true, true, true},   Case {"true", true, true, true},
	    Case {" 0", true, true, true},      Case {"0 ", true, true, true},
	    Case {"-1", true, true, true},      Case {"off", true, true, true},
	};
	for (const auto& test: cases) {
		Check(Common::ParseEnvFlag(test.value, false) == test.default_off, "default-off parsing");
		Check(Common::ParseEnvFlag(test.value, true) == test.default_on, "default-on parsing");
		Check(Common::ParseNonEmptyEnvFlag(test.value) == test.nonempty, "nonempty parsing");
	}
	for (unsigned first = 1; first < 256; ++first) {
		for (unsigned second = 0; second < 256; ++second) {
			const char value[]  = {static_cast<char>(first), static_cast<char>(second), '\0'};
			const bool expected = std::strcmp(value, "0") != 0;
			Check(Common::ParseEnvFlag(value, false) == expected, "byte-pair differential");
			Check(Common::ParseNonEmptyEnvFlag(value) == expected,
			      "nonempty byte-pair differential");
		}
	}
}

void SetValue(const char* value) {
#if defined(_WIN32)
	Check(_putenv_s("KYTY_TEST_ENVIRONMENT_VALUE", value != nullptr ? value : "") == 0,
	      "set environment");
#else
	Check((value != nullptr ? setenv("KYTY_TEST_ENVIRONMENT_VALUE", value, 1)
	                        : unsetenv("KYTY_TEST_ENVIRONMENT_VALUE")) == 0,
	      "set environment");
#endif
}

void TestEnvironmentReads() {
	constexpr const char* name = "KYTY_TEST_ENVIRONMENT_VALUE";
	SetValue(nullptr);
	Check(!Common::ReadEnvFlag(name, false) && Common::ReadEnvFlag(name, true), "missing flag");
	Check(!Common::ReadEnvEquals(name, "1") && !Common::ReadNonEmptyEnvFlag(name), "missing value");
	for (const auto* value: {"0", "1", "00", "false", "verify", " 1"}) {
		SetValue(value);
		Check(Common::ReadEnvFlag(name, false) == (std::strcmp(value, "0") != 0), "read flag");
		Check(Common::ReadNonEmptyEnvFlag(name) == (std::strcmp(value, "0") != 0),
		      "read nonempty flag");
		Check(Common::ReadEnvEquals(name, "1") == (std::strcmp(value, "1") == 0), "exact one");
		Check(Common::ReadEnvEquals(name, "verify") == (std::strcmp(value, "verify") == 0),
		      "exact mode");
	}
#if !defined(_WIN32)
	SetValue("");
	Check(Common::ReadEnvFlag(name, false) && !Common::ReadNonEmptyEnvFlag(name),
	      "empty environment value");
#endif
	SetValue("1");
	const bool startup = Common::ReadEnvFlag(name, false);
	SetValue("0");
	Check(startup && !Common::ReadEnvFlag(name, false), "caller controls caching");
	SetValue(nullptr);
}

} // namespace

int main() {
	TestFlagSemantics();
	TestEnvironmentReads();
	std::puts("EnvironmentTests: all cases passed");
}
