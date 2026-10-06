# Native guest memory and GPU asset streaming

Complete standalone C++20/Vulkan 1.3 implementation, two complete GLSL compute shaders,
shader compilation rules, and a real GPU integration test. No changes are made to KytyPS5's
memory manager, assets or renderer. No emulator speedup is claimed.

## Build and run

Install a C++20 compiler/library and Vulkan SDK with `glslc`. A Vulkan 1.3 device with
timeline semaphores, synchronization2 and compatible host-visible storage memory is required.

```sh
cmake -S tools/gpu_asset_streamer -B build/asset-streamer
cmake --build build/asset-streamer --config Release
ctest --test-dir build/asset-streamer -C Release --output-on-failure
```

CTest enables core and synchronization validation and requires the Khronos validation layer.
The executable can also run without validation:

```sh
build/asset-streamer/gpu_asset_streamer_demo build/asset-streamer/decompress.spv build/asset-streamer/requests.spv build/asset-streamer/fixtures
```

Adjust the executable path for multi-configuration generators and Windows `.exe` names.
The test writes `fixture-a.rle1`, `fixture-b.rle1`, and `fixture-invalid.rle1` in the explicitly
supplied fixture directory; use a dedicated test directory. MinGW needs its matching runtime
DLLs available on PATH. macOS also requires a Vulkan implementation such as MoltenVK; core
Vulkan is not an Apple system API. The code enables portability enumeration/subset when exposed,
but rejects devices that lack the required Vulkan version or features.

## Guest virtual memory

`GuestVirtualMemory` queries the native page size and rounds its mapping size without overflow.
Windows uses `VirtualAlloc(MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)`; Linux/macOS use
anonymous private `mmap(PROT_READ | PROT_WRITE)`. Destruction uses `VirtualFree` or `munmap`.
These initialize an OS-backed guest address region and per-page metadata, not custom hardware
page tables or a Vulkan allocation. Mapping all pages does not guarantee all physical RAM has
already been populated on overcommitting POSIX hosts.

`Read` and `Write` bounds-check the exact requested guest size and update each touched page's
`atomic<uint32_t>` access counter. The build asserts these counters are always lock-free on the
target. Relaxed counters measure operation counts, independently sampled, and wrap modulo 2^32.
They do not make the byte storage atomic: callers must synchronize overlapping reads/writes.
Raw-pointer accesses, guest native execution, hardware faults and GPU writes are not intercepted.
No claim of automatic hardware-level page utilization tracking is made.

## Vulkan mapped storage

`MappedStorageBuffer` creates a real buffer with `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`, queries
its memory requirements and chooses a compatible HOST_VISIBLE memory type. It prefers
DEVICE_LOCAL, then HOST_COHERENT, then HOST_CACHED capabilities, binds at an aligned offset 0,
and maps the entire allocation. It checks storage-range and host-address-space limits.
Noncoherent memory uses whole-allocation flush/invalidate operations, satisfying atom alignment
requirements. No `vkCmdCopyBuffer`, separate asset staging buffer or CPU decompressor is used.

`Decode` opens the asset with `std::ifstream`, requests unbuffered filebuf operation and reads
the raw file directly into mapped Vulkan input storage. The CPU validates header/run coverage;
it never expands the compressed data. The file must remain immutable during the read. The
standard library and OS may still buffer/copy file data internally; this implementation does
not promise kernel bypass, storage DMA into VRAM or physically zero-copy I/O.

Ordinary OS mappings cannot enable Resizable BAR or become device-local memory through alignment.
HOST_VISIBLE | DEVICE_LOCAL indicates the Vulkan memory path is available; it does not uniquely
identify BAR sizing, and can also describe unified memory. Without that combination the implementation
uses compatible host-visible storage and the GPU may access system RAM over PCIe. Even with BAR
mapping, CPU writes and reads can traverse PCIe. Hardware/DMA zero-copy on every host is impossible
to guarantee using only `ifstream` and core Vulkan. External-memory import or platform storage APIs
would require additional capabilities and distinct implementations.

## Concrete RLE1 asset format

All fields are little-endian uint32 words. The build supports little-endian hosts explicitly.

| Field | Meaning |
| --- | --- |
| Header word 0 | `0x31454C52`, bytes `RLE1` |
| Header word 1 | Nonzero decoded word count |
| Header word 2 | Nonzero run count |
| Header word 3 | Reserved, must be zero |
| Each 3-word record | Decoded word offset, nonzero word count, repeated uint32 value |

Records must cover the output contiguously in increasing order from offset zero, without holes,
overlap or overflow. File length must exactly equal `16 + runCount * 12` bytes. Both input and
output must fit `maxStorageBufferRange`. This is a defined word-run codec; it is not Oodle, zlib,
an arbitrary PS5 game format, or a claim that any compressed file can be decoded by this shader.

`decompress.comp` runs one invocation per output word, binary-searches the validated run offsets,
and writes the repeated value directly into the output storage buffer. Two-dimensional dispatch
extends beyond the X workgroup-count limit; host checks prevent index arithmetic overflow. The
output remains GPU storage and can be used without a bulk CPU readback copy. The test reads
the mapped output solely to validate exact decoded words.

## GPU-driven demand loop

`requests.comp` generates asset IDs on the GPU, using the concrete policy `id % stride == 0`.
Unrequested slots contain UINT32_MAX. `StreamGpuDemandAsync(manifest, stride)` returns a future
after enqueueing work into a bounded 64-job queue. The background `jthread` submits the request
shader, waits for its timeline value, invalidates noncoherent request memory, validates IDs, reads
only the requested files and dispatches their GPU decompression. Paths remain owned by the job.
This is a complete GPU-produced request/service loop with a deterministic selection policy,
not a renderer visibility/residency/LOD system. The application does not read files before GPU demand.

Each compute dispatch binds real descriptor sets and a core compute pipeline, emits HOST_WRITE to
COMPUTE_STORAGE_READ and COMPUTE_STORAGE_WRITE to HOST/STORAGE_READ barriers, and submits with
`vkQueueSubmit2`. A monotonically increasing timeline value is signaled at ALL_COMMANDS; the worker
waits before recycling descriptors, the persistent command buffer or input allocations. There is one
background worker and one in-flight dispatch; multi-slot pipelining is not implemented or claimed.
The application can continue working while the future is pending; calling `future.get()` waits.

## Memory selection and dispatch shape

The optional `StreamingOptions` argument supports `maximumDecodeGroupsX` (0 means the native
device limit) and `excludedMemoryFlags` (0 means no exclusions). A smaller X limit creates a
legal 2D decode grid without altering reported device limits. Strict exclusions are also available
on `MappedStorageBuffer`; excluding HOST_VISIBLE is rejected. Excluding HOST_COHERENT selects
real noncoherent memory when a compatible type exists, and fails explicitly otherwise.

The integration test exercises a 3-group X cap with exact output checks. It runs the noncoherent
test only when the device exposes a compatible type; unavailable hardware is reported as SKIP,
never simulated by changing the properties of a coherent allocation.

## Lifetime, errors and integration

The worker owns command/descriptor pool operations. Submission is protected by `StreamingDevice::queueMutex`;
external users of that queue must use the same mutex. Returned assets retain a shared device context.
Their buffers must remain alive until every external GPU consumer completes. Cross-queue/family users
must add their own semaphore dependencies and ownership transfers; the buffers use exclusive sharing.
Do not modify mapped data concurrently with GPU use. CPU invalidation is only valid after completion.

Failures propagate through futures. Invalid requests, malformed/truncated files, unsupported features,
allocation failures and oversized dispatches are rejected. A bad file does not kill the service worker.
Device loss is reported; full device-loss recovery is not implemented. Destructor calls drain accepted
jobs and join the worker before destroying pipeline resources. DeviceWaitIdle occurs only in device
teardown or exceptional submit/wait cleanup, not the normal streaming loop. Failed submission
also drains the device before input/output storage can be released. If cleanup cannot establish
completion and the device is not lost, the process terminates instead of freeing in-flight resources.
Constructor/partial-allocation
cleanup releases Vulkan objects. Do not race service destruction with submission of new jobs.

## Verification on October 7, 2026

Windows/MinGW-w64 GCC 15.2 Release build succeeds with `-Wall -Wextra -Werror`.
Real GPU integration on RTX 4060 Ti passes with core and synchronization validation enabled and
zero reported validation errors. It verifies native page alignment, counters across a page boundary,
guest range rejection, GPU selection that skips a nonexistent unrequested file, exact decompression
of 257 and 262145 output words, partial final workgroups, malformed RLE rejection and worker recovery.
Observed output memory flags are 7: DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT. Host page size is
4096 bytes. These flags do not establish the PCIe BAR size. The expanded run also verifies a
2D grid with X capped at 3, 8 concurrent users sharing page counters (8,000 reads and writes),
seven more malformed table variants, and retained output buffers after streamer shutdown.
The noncoherent GPU test is skipped because this device exposes no HOST_VISIBLE noncoherent
memory type. Linux/macOS builds, additional GPU vendors, device loss and game assets remain unverified.
No throughput, RAM/VRAM budget, zero-PCIe-transfer or game-FPS result is claimed.

Primary references:

- https://docs.vulkan.org/guide/latest/memory_allocation.html
- https://docs.vulkan.org/spec/latest/chapters/memory.html
- https://docs.vulkan.org/guide/latest/synchronization_examples.html
- https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc
- https://man7.org/linux/man-pages/man2/mmap.2.html
