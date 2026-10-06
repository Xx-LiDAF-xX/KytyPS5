#include "UniversalShaderCache.h"
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
void CheckVulkan(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
    }
}
}

UniversalShaderCache::UniversalShaderCache(
    VkDevice device, VkPipeline fallbackInvisiblePipeline, CompileFunction compile,
    std::uint32_t workerCount, std::size_t requestCapacity, std::size_t maximumPipelines)
    : g_FallbackInvisiblePipeline(fallbackInvisiblePipeline), device_(device),
      compiler_(std::move(compile)), maximumPipelines_(maximumPipelines),
      requestCapacity_(requestCapacity) {
    if (device == VK_NULL_HANDLE || fallbackInvisiblePipeline == VK_NULL_HANDLE || !compiler_ ||
        workerCount == 0 || workerCount > 64 || requestCapacity == 0 || maximumPipelines == 0 ||
        requestCapacity > static_cast<std::size_t>(std::counting_semaphore<>::max()) - workerCount) {
        throw std::invalid_argument("Invalid UniversalShaderCache configuration");
    }
    requests_ = std::make_unique<Request[]>(requestCapacity_);
    pipelines_.reserve(maximumPipelines_);
    driverCaches_.reserve(workerCount);
    workers_.reserve(workerCount);
    try {
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semaphore.pNext = &type;
        CheckVulkan(vkCreateSemaphore(device_, &semaphore, nullptr, &retirement_), "vkCreateSemaphore");
        for (std::uint32_t index = 0; index < workerCount; ++index) {
            VkPipelineCacheCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
            VkPipelineCache cache = VK_NULL_HANDLE;
            CheckVulkan(vkCreatePipelineCache(device_, &info, nullptr, &cache), "vkCreatePipelineCache");
            driverCaches_.push_back(cache);
            workers_.emplace_back([this, cache](std::stop_token stop) { Worker(stop, cache); });
        }
    } catch (...) {
        StopWorkers();
        DestroyObjects();
        throw;
    }
}

UniversalShaderCache::~UniversalShaderCache() {
    const auto result = Shutdown();
    // Returning without completion would destroy pipelines still referenced by GPU work.
    if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) std::terminate();
}

VkPipeline UniversalShaderCache::GetUniversalVulkanPipeline(std::uint64_t ps5ShaderHash) {
    if (!accepting_.load(std::memory_order_acquire)) return g_FallbackInvisiblePipeline;
    bool known = false;
    {
        std::shared_lock lock(mutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            const auto found = pipelines_.find(ps5ShaderHash);
            if (found != pipelines_.end()) {
                if (found->second.state == PipelineState::Ready) return found->second.pipeline;
                known = true;
            }
        }
    }
    if (!known && !TryEnqueue(ps5ShaderHash)) queueDrops_.fetch_add(1, std::memory_order_relaxed);
    fallbackReturns_.fetch_add(1, std::memory_order_relaxed);
    return g_FallbackInvisiblePipeline;
}

bool UniversalShaderCache::TryEnqueue(std::uint64_t hash) noexcept {
    const auto start = enqueueCursor_.fetch_add(1, std::memory_order_relaxed) % requestCapacity_;
    for (std::size_t index = 0; index < requestCapacity_; ++index) {
        auto& request = requests_[(start + index) % requestCapacity_];
        auto expected = RequestState::Empty;
        if (!request.state.compare_exchange_strong(expected, RequestState::Writing,
                std::memory_order_acquire, std::memory_order_relaxed)) continue;
        request.hash = hash;
        request.state.store(RequestState::Queued, std::memory_order_release);
        workAvailable_.release();
        return true;
    }
    return false;
}

bool UniversalShaderCache::TryDequeue(std::uint64_t& hash) noexcept {
    const auto start = dequeueCursor_.fetch_add(1, std::memory_order_relaxed) % requestCapacity_;
    for (std::size_t index = 0; index < requestCapacity_; ++index) {
        auto& request = requests_[(start + index) % requestCapacity_];
        auto expected = RequestState::Queued;
        if (!request.state.compare_exchange_strong(expected, RequestState::Reading,
                std::memory_order_acquire, std::memory_order_relaxed)) continue;
        hash = request.hash;
        request.state.store(RequestState::Empty, std::memory_order_release);
        return true;
    }
    return false;
}

void UniversalShaderCache::Worker(std::stop_token stop, VkPipelineCache driverCache) {
    for (;;) {
        workAvailable_.acquire();
        if (stop.stop_requested()) return;
        std::uint64_t hash = 0;
        if (TryDequeue(hash)) Compile(hash, driverCache);
    }
}

void UniversalShaderCache::Compile(std::uint64_t hash, VkPipelineCache driverCache) noexcept {
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool inserted = false;
    try {
        {
            std::unique_lock lock(mutex_);
            if (pipelines_.contains(hash)) return; // Deduplicates queued and in-flight misses.
            if (pipelines_.size() >= maximumPipelines_) {
                capacityDrops_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            pipelines_.emplace(hash, Entry{});
            inserted = true;
        }
        VkResult result;
        try {
            result = compiler_(hash, device_, driverCache, &pipeline);
        } catch (...) {
            result = VK_ERROR_UNKNOWN;
        }
        if (result == VK_SUCCESS && pipeline == VK_NULL_HANDLE) result = VK_ERROR_UNKNOWN;
        // Aliasing the borrowed fallback violates the ownership contract; never destroy it.
        if (pipeline == g_FallbackInvisiblePipeline) {
            pipeline = VK_NULL_HANDLE;
            result = VK_ERROR_UNKNOWN;
        }
        if (result != VK_SUCCESS && pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
        {
            std::unique_lock lock(mutex_);
            auto& entry = pipelines_.at(hash);
            entry.pipeline = pipeline;
            entry.result = result;
            entry.state = result == VK_SUCCESS ? PipelineState::Ready : PipelineState::Failed;
            pipeline = VK_NULL_HANDLE; // Ownership transferred to the database.
        }
        if (result == VK_SUCCESS) compiled_.fetch_add(1, std::memory_order_relaxed);
        else failed_.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline, nullptr);
        failed_.fetch_add(1, std::memory_order_relaxed);
        // No throwing path normally exists after entry insertion except callback/publish;
        // callback exceptions are handled above. Recover if the standard mutex throws.
        if (inserted) {
            try {
                std::unique_lock lock(mutex_);
                auto& entry = pipelines_.at(hash);
                entry.result = VK_ERROR_UNKNOWN;
                entry.state = PipelineState::Failed;
            } catch (...) { std::terminate(); }
        }
    }
}

UniversalShaderCache::Statistics UniversalShaderCache::GetStatistics() const noexcept {
    return {fallbackReturns_.load(std::memory_order_relaxed), queueDrops_.load(std::memory_order_relaxed),
            compiled_.load(std::memory_order_relaxed), failed_.load(std::memory_order_relaxed),
            capacityDrops_.load(std::memory_order_relaxed)};
}
VkSemaphore UniversalShaderCache::RetirementTimeline() const noexcept { return retirement_; }
std::optional<VkResult> UniversalShaderCache::TryGetCompilationResult(std::uint64_t hash) const {
    std::shared_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return std::nullopt;
    const auto found = pipelines_.find(hash);
    if (found == pipelines_.end()) return std::nullopt;
    return found->second.result;
}

void UniversalShaderCache::RecordSuccessfulSubmission(std::uint64_t value) {
    if (!accepting_.load(std::memory_order_acquire) || value <= lastSubmittedValue_) {
        throw std::logic_error("Submission values must increase before shutdown");
    }
    lastSubmittedValue_ = value;
}

void UniversalShaderCache::StopWorkers() {
    accepting_.store(false, std::memory_order_release);
    for (auto& worker : workers_) worker.request_stop();
    workAvailable_.release(static_cast<std::ptrdiff_t>(workers_.size()));
    workers_.clear(); // std::jthread joins; compilation cannot be forcibly canceled.
}

VkResult UniversalShaderCache::Shutdown(std::uint64_t timeoutNanoseconds) {
    if (destroyed_) return VK_SUCCESS;
    StopWorkers();
    if (lastSubmittedValue_ != 0) {
        VkSemaphoreWaitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &retirement_;
        wait.pValues = &lastSubmittedValue_;
        const auto result = vkWaitSemaphores(device_, &wait, timeoutNanoseconds);
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) return result;
        DestroyObjects();
        return result;
    }
    DestroyObjects();
    return VK_SUCCESS;
}

void UniversalShaderCache::DestroyObjects() noexcept {
    for (const auto& [hash, entry] : pipelines_) {
        (void)hash;
        if (entry.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, entry.pipeline, nullptr);
    }
    pipelines_.clear();
    for (const auto cache : driverCaches_) vkDestroyPipelineCache(device_, cache, nullptr);
    driverCaches_.clear();
    if (retirement_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, retirement_, nullptr);
    retirement_ = VK_NULL_HANDLE;
    destroyed_ = true;
}
