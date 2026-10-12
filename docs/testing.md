# Testing KytyPS5

KytyPS5 classifies every CTest entry by the capability it needs. This keeps the
default CI suite deterministic and prevents GPU tests from being mistaken for
headless-safe coverage.

## Test labels

| Label | Contract | Where it runs |
| --- | --- | --- |
| `core` | No display server or Vulkan device is required. | Windows, Linux, and macOS CI. |
| `vulkan` | Creates a Vulkan device or exercises GPU-backed emulator state. | A developer machine with a supported Vulkan driver. |
| `platform` | Valid only on the platform where CMake registers it. | The matching platform CI job. |

The `core` inventory covers command-line validation, diagnostics, utility and
container code, file systems, memory management, loader patching, audio, input,
save data, dialogs, networking, shader metadata, and CPU-only graphics layout
checks. The CPU-only selectors in `shader_recompiler_compute_tests` are
`graphics_draw_offsets`, `image_transfer_layout`, and
`texture_cache_depth_readback`.

The `vulkan` inventory covers the complete shader recompiler run plus its
registered GPU selectors: shader alignment, rasterization, scheduling, stream
buffers, command lanes, PM4 state, tiling, image allocation and views, storage
and render targets, compute clears, buffer-cache lifecycle, overlaps, HTILE,
and range tracking. CI builds this executable but does not run these tests on
headless workers.

The current `platform` inventory contains `macos_sse4a`, which validates the
Darwin signal-context path for SSE4a instruction emulation.

## Local commands

Configure a Release build first. On Windows, run the commands from an x64
Visual Studio Developer Prompt:

```powershell
cmake -S . -B _Build/compat-foundation -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang-cl `
  -DCMAKE_CXX_COMPILER=clang-cl
cmake --build _Build/compat-foundation --target kyty_tests --parallel
ctest --test-dir _Build/compat-foundation -L core --output-on-failure
```

Linux uses the same target and label:

```bash
cmake -S . -B _Build/compat-foundation -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++
cmake --build _Build/compat-foundation --target kyty_tests --parallel
ctest --test-dir _Build/compat-foundation -L core --output-on-failure
```

Run the Vulkan suite only on a machine with a working Vulkan driver:

```text
ctest --test-dir _Build/compat-foundation -L vulkan --output-on-failure
```

Use `ctest --test-dir <build> --show-only=json-v1` when auditing labels. A new
test is incomplete until it has exactly one of the three labels above and its
executable is included in the `kyty_tests` aggregate target.

Windows validation currently uses Release builds. The existing shader IR
layout guard is incompatible with the larger MSVC debug standard-library
layout, so a clang-cl Debug configuration stops during compilation before the
test suite can run.
