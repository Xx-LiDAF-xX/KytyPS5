#include "UniversalShaderCache.h"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

// Mock Vulkan backend: exercises ownership, concurrency and retirement contracts.
// These tests do not validate a graphics pipeline or GPU rendering output.
namespace {
template<class T> T Handle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
std::atomic<std::uintptr_t> nextHandle{100};
std::atomic<std::uint64_t> timelineValue{0};
std::atomic<unsigned> destroyedPipelines{0};
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Predicate> void Eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Test timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
struct Gate {
    std::mutex mutex;
    std::condition_variable condition;
    bool released = false;
    void Wait() {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return released; });
    }
    void Release() {
        { std::lock_guard lock(mutex); released = true; }
        condition.notify_all();
    }
};
struct ReleaseOnExit {
    Gate& gate;
    ~ReleaseOnExit() { gate.Release(); }
};
}

extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo* info,
    const VkAllocationCallbacks*, VkSemaphore* semaphore) {
    const auto* type = static_cast<const VkSemaphoreTypeCreateInfo*>(info->pNext);
    if (!type || type->semaphoreType != VK_SEMAPHORE_TYPE_TIMELINE) return VK_ERROR_INITIALIZATION_FAILED;
    *semaphore = Handle<VkSemaphore>(nextHandle.fetch_add(1));
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice, const VkPipelineCacheCreateInfo*,
    const VkAllocationCallbacks*, VkPipelineCache* cache) {
    *cache = Handle<VkPipelineCache>(nextHandle.fetch_add(1));
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(VkDevice, VkPipelineCache, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks*) {
    destroyedPipelines.fetch_add(1);
}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(VkDevice, const VkSemaphoreWaitInfo* info, std::uint64_t) {
    return timelineValue.load() >= info->pValues[0] ? VK_SUCCESS : VK_TIMEOUT;
}
}

int main() {
    try {
        const auto device = Handle<VkDevice>(1);
        const auto fallback = Handle<VkPipeline>(2);
        std::atomic<unsigned> calls{0}, entered{0};
        Gate gate;
        UniversalShaderCache cache(device, fallback,
            [&](std::uint64_t hash, VkDevice, VkPipelineCache, VkPipeline* result) {
                calls.fetch_add(1);
                entered.fetch_add(1);
                gate.Wait();
                if (hash == 999) return VK_ERROR_INITIALIZATION_FAILED;
                *result = Handle<VkPipeline>(nextHandle.fetch_add(1));
                return VK_SUCCESS;
            }, 2, 8, 16);
        // Must release callbacks before cache destructor joins, including assertion failure.
        ReleaseOnExit release{gate};
        Require(cache.GetUniversalVulkanPipeline(42) == fallback, "Miss did not return fallback");
        Eventually([&] { return entered.load() == 1; });
        std::vector<std::jthread> callers;
        std::atomic<bool> badReturn{false};
        for (unsigned index = 0; index < 8; ++index) {
            callers.emplace_back([&] {
                for (unsigned repeat = 0; repeat < 2000; ++repeat) {
                    if (cache.GetUniversalVulkanPipeline(42) != fallback) badReturn.store(true);
                }
            });
        }
        callers.clear();
        Require(!badReturn.load(), "In-flight request returned an unpublished handle");
        Require(calls.load() == 1, "Same hash compiled more than once");
        (void)cache.GetUniversalVulkanPipeline(43);
        Eventually([&] { return entered.load() == 2; });
        for (std::uint64_t hash = 1000; hash < 1100; ++hash) (void)cache.GetUniversalVulkanPipeline(hash);
        Require(cache.GetStatistics().queueDrops > 0, "Bounded request queue did not report overflow");
        gate.Release();
        Eventually([&] { return cache.GetStatistics().compiled >= 2; });
        Eventually([&] { return cache.GetUniversalVulkanPipeline(42) != fallback; });
        // Queue overflow is retryable on later calls. Compilation failures are terminal.
        Eventually([&] {
            (void)cache.GetUniversalVulkanPipeline(999);
            const auto result = cache.TryGetCompilationResult(999);
            return result && *result == VK_ERROR_INITIALIZATION_FAILED;
        });
        Require(cache.GetUniversalVulkanPipeline(999) == fallback, "Failed pipeline escaped");
        cache.RecordSuccessfulSubmission(1);
        const auto before = destroyedPipelines.load();
        Require(cache.Shutdown(0) == VK_TIMEOUT, "Retirement did not wait for submission");
        Require(destroyedPipelines.load() == before, "Destroyed pipeline before timeline completion");
        timelineValue.store(1);
        Require(cache.Shutdown(0) == VK_SUCCESS, "Retirement retry failed");
        Require(destroyedPipelines.load() > before, "Completed pipelines were not destroyed");
        std::cout << "PASS: immediate fallback, concurrent deduplication, queue overflow, publication, "
                     "compile failure, timeline retirement and shutdown retry\n";
        // Independent fixed-capacity database test.
        UniversalShaderCache small(device, fallback,
            [&](std::uint64_t, VkDevice, VkPipelineCache, VkPipeline* result) {
                *result = Handle<VkPipeline>(nextHandle.fetch_add(1));
                return VK_SUCCESS;
            }, 1, 4, 1);
        Eventually([&] { return small.GetUniversalVulkanPipeline(1) != fallback; });
        Eventually([&] {
            (void)small.GetUniversalVulkanPipeline(2);
            return small.GetStatistics().capacityDrops > 0;
        });
        Require(small.Shutdown(0) == VK_SUCCESS, "Capacity test shutdown failed");
        std::cout << "PASS: bounded database capacity\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
