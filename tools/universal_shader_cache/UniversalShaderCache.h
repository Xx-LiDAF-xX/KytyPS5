#pragma once
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <semaphore>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

class UniversalShaderCache final {
public:
    // Must return a new, independently owned pipeline. Thread-safe callback;
    // recipe data and all referenced Vulkan objects must outlive this cache.
    using CompileFunction = std::function<VkResult(
        std::uint64_t, VkDevice, VkPipelineCache, VkPipeline*)>;
    struct Statistics {
        std::uint64_t fallbackReturns;
        std::uint64_t queueDrops;
        std::uint64_t compiled;
        std::uint64_t failed;
        std::uint64_t capacityDrops;
    };

    // The device must have timelineSemaphore enabled. The fallback is borrowed,
    // precompiled and compatible with every draw routed through this instance.
    UniversalShaderCache(VkDevice device, VkPipeline fallbackInvisiblePipeline,
                         CompileFunction compile, std::uint32_t workerCount = 2,
                         std::size_t requestCapacity = 256,
                         std::size_t maximumPipelines = 16384);
    ~UniversalShaderCache();
    UniversalShaderCache(const UniversalShaderCache&) = delete;
    UniversalShaderCache& operator=(const UniversalShaderCache&) = delete;

    // No allocation, compilation, waiting mutex acquisition, GPU wait or thread
    // creation occurs here. Best-effort under contention, not hard realtime.
    [[nodiscard]] VkPipeline GetUniversalVulkanPipeline(std::uint64_t ps5ShaderHash);
    [[nodiscard]] Statistics GetStatistics() const noexcept;
    // nullopt means unknown/lock contention; VK_NOT_READY means in-flight.
    [[nodiscard]] std::optional<VkResult> TryGetCompilationResult(std::uint64_t hash) const;
    [[nodiscard]] VkSemaphore RetirementTimeline() const noexcept;

    // Single externally synchronized submission owner. After successful queue
    // submission, record the strictly increasing value signaled on this cache's
    // timeline at ALL_COMMANDS. Signal must follow all uses of cached pipelines.
    // One queue timeline only; other queues must join into this dependency chain.
    void RecordSuccessfulSubmission(std::uint64_t value);
    // Caller first stops all lookups/recording/submission, submits or discards
    // outstanding recorded command buffers, and ensures device lifetime.
    // This intentionally blocks only during shutdown. Retry allowed on timeout.
    [[nodiscard]] VkResult Shutdown(std::uint64_t timeoutNanoseconds = UINT64_MAX);

    const VkPipeline g_FallbackInvisiblePipeline;

private:
    enum class PipelineState { Compiling, Ready, Failed };
    struct Entry {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkResult result = VK_NOT_READY;
        PipelineState state = PipelineState::Compiling;
    };
    // Every slot transition has a single writer; state publishes the plain hash.
    enum class RequestState : std::uint8_t { Empty, Writing, Queued, Reading };
    struct Request {
        std::atomic<RequestState> state{RequestState::Empty};
        std::uint64_t hash = 0;
    };
    bool TryEnqueue(std::uint64_t hash) noexcept;
    bool TryDequeue(std::uint64_t& hash) noexcept;
    void Worker(std::stop_token stop, VkPipelineCache driverCache);
    void Compile(std::uint64_t hash, VkPipelineCache driverCache) noexcept;
    void StopWorkers();
    void DestroyObjects() noexcept;

    VkDevice device_;
    CompileFunction compiler_;
    VkSemaphore retirement_ = VK_NULL_HANDLE;
    std::size_t maximumPipelines_;
    std::size_t requestCapacity_;
    std::unique_ptr<Request[]> requests_;
    std::counting_semaphore<> workAvailable_{0};
    std::atomic<std::size_t> enqueueCursor_{0}, dequeueCursor_{0};
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::uint64_t, Entry> pipelines_;
    std::vector<VkPipelineCache> driverCaches_;
    std::vector<std::jthread> workers_;
    std::atomic<bool> accepting_{true};
    std::atomic<std::uint64_t> fallbackReturns_{0}, queueDrops_{0}, compiled_{0}, failed_{0}, capacityDrops_{0};
    std::uint64_t lastSubmittedValue_ = 0;
    bool destroyed_ = false;
};
