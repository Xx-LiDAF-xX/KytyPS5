#ifndef KYTY_COMMON_ENVIRONMENT_H_
#define KYTY_COMMON_ENVIRONMENT_H_

#include <cstdlib>
#include <cstring>

namespace Common {

// Only the exact string "0" disables a present value. Empty is a present value;
// callers whose flags require nonempty text use ParseNonEmptyEnvFlag instead.
[[nodiscard]] constexpr bool ParseEnvFlag(const char* value, bool default_value) noexcept {
	return value == nullptr ? default_value : !(value[0] == '0' && value[1] == '\0');
}

[[nodiscard]] constexpr bool ParseNonEmptyEnvFlag(const char* value) noexcept {
	return value != nullptr && value[0] != '\0' && ParseEnvFlag(value, false);
}

// Reads deliberately do not cache: the caller owns startup-only or live semantics.
[[nodiscard]] inline bool ReadEnvFlag(const char* name, bool default_value) {
	return ParseEnvFlag(std::getenv(name), default_value);
}

[[nodiscard]] inline bool ReadNonEmptyEnvFlag(const char* name) {
	return ParseNonEmptyEnvFlag(std::getenv(name));
}

[[nodiscard]] inline bool ReadEnvEquals(const char* name, const char* expected) {
	const auto* value = std::getenv(name);
	return value != nullptr && std::strcmp(value, expected) == 0;
}

} // namespace Common

#endif // KYTY_COMMON_ENVIRONMENT_H_
