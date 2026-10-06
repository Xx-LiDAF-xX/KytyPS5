# UniversalShaderCache

Standalone C++20/Vulkan 1.3 asynchronous PSO database. This is an explicitly optional draw-skipping
mode and is not integrated into KytyPS5. Missing draws, shader side effects, depth/shadow contributions,
occlusion results and compute writes cannot be made accurate by returning an invisible pipeline.
Use an accurate blocking/prewarm path when those effects are required.

## Construction and compatibility

Construct at boot with a live `VkDevice`, a precompiled borrowed invisible graphics pipeline,
and a thread-safe `CompileFunction`. `g_FallbackInvisiblePipeline` is a real `VkPipeline` handle,
not a fabricated struct. The application owns its creation because only the renderer knows its
pipeline layout, rendering formats, sample count, render-pass compatibility, topology and dynamic
state requirements. It must remain valid until after the cache is shut down and GPU work completes.
A compatible rasterizer-discard fallback can suppress rasterization for a conventional vertex draw;
outputting alpha zero alone does not suppress depth/stencil writes or shader side effects.
Mesh draws, tessellation, compute, ray-tracing dispatches and arbitrary graphics layouts cannot use
one universal dummy graphics pipeline. Use separately compatible instances or a different strategy.

Each instance covers one immutable PSO compatibility domain. A raw PS5 shader hash does not identify
a general graphics PSO: multiple shaders, specialization, layout, formats, topology, depth/blend state,
multisampling and enabled dynamic states may all change it. The supplied hash must uniquely identify
the full recipe within this domain; callers must handle collisions and state variants. Immutable
recipe data must be accessible to the callback and remain alive through worker shutdown.

The callback receives `(hash, device, workerPipelineCache, outputPipeline)`, calls the engine's
actual shader/pipeline creation code, and returns its `VkResult`. It must return a new independently
owned pipeline, created with null allocation callbacks. No callback implementation can be inferred
from a shader hash alone. Successful ownership transfers to the database; partial output on failure
is destroyed. Returning the borrowed fallback is rejected. Exceptions become `VK_ERROR_UNKNOWN`.

Vulkan `timelineSemaphore` must be enabled on the logical device. Queue submission through
`vkQueueSubmit2` additionally requires `synchronization2` enabled. This library does not change
device features or inject calls into an engine's command recorder.

## Lookup and compilation

`GetUniversalVulkanPipeline(uint64_t ps5ShaderHash)` performs no allocation, pipeline creation,
thread creation, GPU wait or waiting mutex acquisition. It tries a shared read lock. A ready hit
returns the compiled handle; in-flight/failed hits return the fallback. Misses enter a bounded,
preallocated multi-producer/multi-consumer mailbox pool and wake a pre-created `std::jthread` worker.
On lock contention it may enqueue a redundant request; worker-side exclusive-map insertion deduplicates
both queued and in-flight hashes before compilation. On queue exhaustion it returns the fallback,
counts the drop and allows a later lookup to retry. Finite queue storage cannot guarantee immediate
dispatch for every possible burst while also guaranteeing no allocation or waiting.

The database is `std::unordered_map<uint64_t, Entry>`, where `Entry` owns its compiled `VkPipeline`,
status and error. `std::shared_mutex` protects it. Workers own separate Vulkan `VkPipelineCache`
objects, avoiding shared-driver-cache host synchronization. Driver calls only run on workers.
Database capacity is bounded. There is no eviction or replacement of a published pipeline; handles
remain stable until shutdown. Failure entries count against capacity and are terminal for this cache
instance. No persistent disk cache, automatic retries or device-loss recovery is claimed.

`TryGetCompilationResult` provides a non-waiting status query. `GetStatistics` reports fallbacks,
queue exhaustion, successful/failed compilation and capacity exhaustion. Its counters are independent
relaxed snapshots, not an atomic global snapshot. There is no promise of hard realtime, lock-free
standard-library atomics on every architecture, constant hash-table lookup complexity, instant OS
scheduling or guaranteed frame pacing. Those cannot follow from using `std::shared_mutex` or `jthread`.

## Publication and GPU synchronization

Pipeline construction finishes on the CPU before the worker publishes a handle under the exclusive
map lock. Subsequent shared-lock acquisitions publish it safely to command recording. No GPU
semaphore or fence is needed to complete CPU pipeline creation. Recording a new binding uses the
ready handle; previously recorded command buffers retain the fallback binding. Vulkan cannot swap
a recorded handle in place.

The cache creates and owns `RetirementTimeline()`. A single externally synchronized submission owner
must signal it at strictly increasing values at `VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT`, after all
submitted uses of these pipelines. A complete helper for a submission with one command buffer and
no external wait/signal semaphores is below. Real presentation must also preserve the renderer's
acquire/present semaphore dependencies and queue synchronization.

```cpp
VkResult SubmitTracked(VkQueue queue, VkCommandBuffer commandBuffer,
                       UniversalShaderCache& cache, uint64_t value) {
    VkCommandBufferSubmitInfo command{};
    command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command.commandBuffer = commandBuffer;
    VkSemaphoreSubmitInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = cache.RetirementTimeline();
    signal.value = value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    const VkResult result = vkQueueSubmit2(queue, 1, &submit, VK_NULL_HANDLE);
    if (result == VK_SUCCESS) cache.RecordSuccessfulSubmission(value);
    return result;
}
```

Prevalidate increasing `value` on the submission owner before submitting. No lookup, submission,
or device destruction may race `Shutdown`. Stop command recording, then submit or discard every
outstanding command buffer. Ensure all pipeline-using queues join the tracked dependency chain.
Simply taking the maximum of independently signaled values on different queues does not prove
completion of all pipeline uses. The cache never submits or externally synchronizes your queues.

`Shutdown(timeout)` stops and joins workers, waits for the last tracked GPU timeline value,
and destroys owned pipelines/caches/semaphore. A timeout retains resources and allows a retry.
The destructor waits indefinitely; shutdown is intentionally blocking. Driver compilation cannot
be forcibly canceled by a stop token. Device loss is reported and permits object cleanup; other
unexpected destructor wait errors terminate rather than destroy resources still in use. The
borrowed fallback is never destroyed by the database. Reset/discard old command buffers before
attempting to reuse them after shutdown because their pipeline references have become invalid.

## Build and verification

```sh
cmake -S tools/universal_shader_cache -B build/shader-cache
cmake --build build/shader-cache --config Release
ctest --test-dir build/shader-cache -C Release --output-on-failure
```

Requires Vulkan SDK headers/loader and a C++20 library implementing `jthread`, `shared_mutex` and
`counting_semaphore`. On this Windows host, GCC 15.2 compiles the sources with
`-std=c++20 -Wall -Wextra -Werror -pthread`. The CMake build produces the library and contract tests.
The test executable links mock Vulkan entry points rather than a graphics driver. It verifies
immediate fallback while workers are gated, 8 concurrent callers, single compilation per hash,
queue exhaustion/retry, ready publication, compilation failure, deferred retirement until timeline
completion, shutdown retry and database capacity. These do not validate graphics pipeline
compatibility, GPU rendering, actual synchronization validation, visual correctness or frame pacing.
Linux/macOS builds and GPU integration are unverified. For MinGW, use matching runtime DLLs on PATH.

## Primary references

- https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyPipeline.html
- https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDraw.html
- https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateGraphicsPipelines.html
- https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_timeline_semaphore.html
