#pragma once
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

struct StreamingDevice final {
    explicit StreamingDevice(bool validation);
    ~StreamingDevice();
    StreamingDevice(const StreamingDevice&) = delete;
    StreamingDevice& operator=(const StreamingDevice&) = delete;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::atomic<unsigned> validationErrors{0};
    std::mutex queueMutex;
private:
    void Destroy() noexcept;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
};

class MappedStorageBuffer final {
public:
    MappedStorageBuffer(std::shared_ptr<StreamingDevice> context, VkDeviceSize bytes,
                        VkMemoryPropertyFlags excludedMemoryFlags = 0);
    ~MappedStorageBuffer();
    MappedStorageBuffer(const MappedStorageBuffer&) = delete;
    MappedStorageBuffer& operator=(const MappedStorageBuffer&) = delete;
    void Flush();
    void Invalidate();
    [[nodiscard]] std::span<std::byte> Bytes() const;
    [[nodiscard]] VkMemoryPropertyFlags Properties() const noexcept { return flags_; }
    [[nodiscard]] VkBuffer Handle() const noexcept { return buffer_; }
    [[nodiscard]] VkDeviceSize Size() const noexcept { return bytes_; }
private:
    void Destroy() noexcept;
    std::shared_ptr<StreamingDevice> context_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory allocation_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkDeviceSize bytes_;
    VkMemoryPropertyFlags flags_ = 0;
};

struct StreamedAsset {
    std::uint32_t id;
    std::shared_ptr<MappedStorageBuffer> data;
};

struct StreamingOptions {
    // Zero uses the device's native X limit. Smaller limits shape a 2D decode grid.
    std::uint32_t maximumDecodeGroupsX = 0;
    // Strict selection constraint; unsupported memory combinations fail explicitly.
    VkMemoryPropertyFlags excludedMemoryFlags = 0;
};

class GpuAssetStreamer final {
public:
    GpuAssetStreamer(const std::filesystem::path& decodeSpirv,
                     const std::filesystem::path& requestSpirv, bool validation = false,
                     StreamingOptions options = {});
    ~GpuAssetStreamer();
    GpuAssetStreamer(const GpuAssetStreamer&) = delete;
    GpuAssetStreamer& operator=(const GpuAssetStreamer&) = delete;
    // GPU produces demand IDs according to the concrete stride policy, then the
    // background service reads only those files and dispatches GPU decompression.
    [[nodiscard]] std::future<std::vector<StreamedAsset>> StreamGpuDemandAsync(
        std::vector<std::filesystem::path> manifest, std::uint32_t stride = 1);
    [[nodiscard]] std::shared_ptr<StreamingDevice> Device() const noexcept { return context_; }
private:
    void Worker(std::stop_token stop);
    std::vector<StreamedAsset> Stream(std::span<const std::filesystem::path> manifest, std::uint32_t stride);
    std::shared_ptr<MappedStorageBuffer> Decode(const std::filesystem::path& path);
    void Dispatch(VkPipeline pipeline, std::span<const std::shared_ptr<MappedStorageBuffer>> buffers,
                  std::uint32_t groupsX, std::uint32_t groupsY, const void* push, std::uint32_t pushBytes);
    VkPipeline CreatePipeline(const std::filesystem::path& path, VkPipelineLayout layout);
    void Destroy() noexcept;
    std::shared_ptr<StreamingDevice> context_;
    StreamingOptions options_;
    VkDescriptorSetLayout decodeSet_ = VK_NULL_HANDLE, requestSet_ = VK_NULL_HANDLE;
    VkPipelineLayout decodeLayout_ = VK_NULL_HANDLE, requestLayout_ = VK_NULL_HANDLE;
    VkPipeline decodePipeline_ = VK_NULL_HANDLE, requestPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptors_ = VK_NULL_HANDLE;
    VkCommandPool commands_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    std::uint64_t value_ = 0;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::packaged_task<void()>> jobs_;
    std::jthread worker_;
};
