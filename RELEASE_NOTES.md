# KytyPS5 improvements — 2026-10-07

- Fixed GPU frame-boundary synchronization and respected the configured frames-ahead setting.
- Fixed wave64 shader ballot, first-lane and scalar branch result handling.
- Restored correct texture clears when a single-mip target reuses a cached mip chain.
- Added Vulkan image allocation error codes and image details to crash diagnostics.
- Improved regression coverage for GPU synchronization, shaders, audio timing, tiling and pixel shader registers.
- Preserved touchpad game input, persistent RT settings and privacy-filtered, dated file logs.

Validation: 72 of 75 tests passed. Astro Bot completed a 40-second startup with RT enabled without a captured crash marker. Windows socket and GPU shared-memory test failures remain under investigation; gameplay FPS gains are not yet verified.

Thanks to nmzik for the original cached mip-chain clear fix (`0ec3655f`), restored in this update.
