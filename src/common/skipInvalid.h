#ifndef EMULATOR_SRC_COMMON_SKIP_INVALID_H_
#define EMULATOR_SRC_COMMON_SKIP_INVALID_H_

#include "common/environment.h"
#include <atomic>
#include <cstdlib>
#include <cstring>

// KYTY_SKIP_INVALID (default on; =0 restores the exits): guest data the emulator cannot use (a stale
// descriptor while a level loads, GPU results for memory unmapped meanwhile, resource tables that
// never become readable) is dropped, bound as null or skipped with a log line instead of ending the
// session. Each site logs its first occurrence only (LogOnce).
namespace Common {

[[nodiscard]] inline bool SkipInvalidEnabled() {
	static const bool enabled = Common::ReadEnvFlag("KYTY_SKIP_INVALID", true);
	return enabled;
}

// True for the first call with this flag (one static flag per site).
[[nodiscard]] inline bool LogOnce(std::atomic_bool& logged) {
	return !logged.exchange(true, std::memory_order_relaxed);
}

} // namespace Common

#endif // EMULATOR_SRC_COMMON_SKIP_INVALID_H_
