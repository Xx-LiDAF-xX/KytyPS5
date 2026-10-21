#ifndef KYTY_COMMON_RENDERER_BATCH_H_
#define KYTY_COMMON_RENDERER_BATCH_H_

#include "common/environment.h"
#include "common/onceValue.h"
#include <cstdlib>
#include <cstring>

namespace Common {

// One opt-in switch for the combined renderer candidate. The default retains the
// established paths so the batch can be disabled without replacing the binary.
inline bool RendererBatchEnabled() {
	static std::atomic<int64_t> enabled_once {Common::OnceUnset};
	const auto enabled = static_cast<bool>(Common::Once(enabled_once, [] {
		return Common::ReadEnvEquals("KYTY_RENDERER_BATCH", "1");
	}));
	return enabled;
}

} // namespace Common

#endif
