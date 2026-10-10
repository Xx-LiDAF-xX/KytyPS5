#pragma once

#include <vulkan/vulkan.hpp>
#include <shared_mutex>
#include <unordered_map>
#include <atomic>

namespace Kyty::Graphics::Pipeline {

class TextureSyncManager {
public:
    void NotifyCPUWrite(vk::Image image, const vk::ImageSubresourceRange& range) {
        std::unique_lock lock(m_sync_mutex);
        m_dirty_images[image] = range;
        m_stale_readbacks.fetch_add(1, std::memory_order_relaxed);
    }

    void FlushToGPU(vk::CommandBuffer cmd, vk::Image image) {
        std::shared_lock lock(m_sync_mutex);
        if (m_dirty_images.contains(image)) {
            // Dispatch a pipeline barrier to flush CPU caches to Device Local memory
            vk::ImageMemoryBarrier barrier;
            barrier.srcAccessMask = vk::AccessFlagBits::eHostWrite;
            barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
            barrier.oldLayout = vk::ImageLayout::ePreinitialized;
            barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = m_dirty_images.at(image);
            
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eFragmentShader,
                                {}, nullptr, nullptr, barrier);
        }
    }

    void ClearSyncState(vk::Image image) {
        std::unique_lock lock(m_sync_mutex);
        m_dirty_images.erase(image);
    }

private:
    std::shared_mutex m_sync_mutex;
    std::unordered_map<vk::Image, vk::ImageSubresourceRange> m_dirty_images;
    std::atomic<uint32_t> m_stale_readbacks{0};
};

} // namespace Kyty::Graphics::Pipeline
