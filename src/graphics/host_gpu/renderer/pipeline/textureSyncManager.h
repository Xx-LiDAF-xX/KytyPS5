#pragma once

#include <vulkan/vulkan.hpp>
#include <shared_mutex>
#include <unordered_map>
#include <atomic>

namespace Libs::Graphics {

class TextureSyncManager {
public:
    void NotifyCPUWrite(vk::Image image, const vk::ImageSubresourceRange& range) {
        std::unique_lock lock(m_sync_mutex);
        m_dirty_images[image] = range;
        m_stale_readbacks.fetch_add(1, std::memory_order_relaxed);
    }

    void FlushToGPU(vk::CommandBuffer cmd, vk::Image image) {
        bool needs_flush = false;
        vk::ImageSubresourceRange range;
        
        {
            std::shared_lock lock(m_sync_mutex);
            auto it = m_dirty_images.find(image);
            if (it != m_dirty_images.end()) {
                needs_flush = true;
                range = it->second;
            }
        }
        
        if (needs_flush) {
            // Dispatch a pipeline barrier to flush CPU caches to Device Local memory
            vk::ImageMemoryBarrier barrier;
            barrier.srcAccessMask = vk::AccessFlagBits::eHostWrite;
            barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
            barrier.oldLayout = vk::ImageLayout::ePreinitialized;
            barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = range;
            
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eAllCommands,
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

} // namespace Libs::Graphics
