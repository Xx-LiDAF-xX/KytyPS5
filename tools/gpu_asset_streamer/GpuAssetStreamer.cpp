#include "GpuAssetStreamer.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
void Check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
void DrainForCleanup(VkDevice device) noexcept {
    const auto result = vkDeviceWaitIdle(device);
    // Never free resources with unconfirmed GPU completion after an unexpected error.
    if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) std::terminate();
}
VKAPI_ATTR VkBool32 VKAPI_CALL Debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* message, void* data) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        static_cast<StreamingDevice*>(data)->validationErrors.fetch_add(1);
        std::cerr << message->pMessage << '\n';
    }
    return VK_FALSE;
}
std::vector<std::uint32_t> LoadSpirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (!file || size <= 0 || size % 4 != 0 || size > 16 * 1024 * 1024) throw std::runtime_error("Invalid SPIR-V file");
    std::vector<std::uint32_t> words(static_cast<std::size_t>(size) / 4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(words.data()), size)) throw std::runtime_error("SPIR-V read failed");
    return words;
}
}

StreamingDevice::StreamingDevice(bool validation) {
    try {
        std::uint32_t count = 0;
        Check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr), "instance extensions");
        std::vector<VkExtensionProperties> available(count);
        Check(vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()), "instance extensions");
        std::vector<const char*> extensions;
        VkInstanceCreateFlags flags = 0;
        for (const auto& extension : available) {
            if (std::strcmp(extension.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
                extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
                flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            }
        }
        if (validation) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        }
        const char* layer = "VK_LAYER_KHRONOS_validation";
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "GPU asset streaming";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &app;
        instanceInfo.flags = flags;
        instanceInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        instanceInfo.ppEnabledExtensionNames = extensions.data();
        instanceInfo.enabledLayerCount = validation ? 1 : 0;
        instanceInfo.ppEnabledLayerNames = validation ? &layer : nullptr;
        const VkValidationFeatureEnableEXT enable = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validationFeatures{};
        validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
        validationFeatures.enabledValidationFeatureCount = 1;
        validationFeatures.pEnabledValidationFeatures = &enable;
        if (validation) instanceInfo.pNext = &validationFeatures;
        Check(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");
        if (validation) {
            VkDebugUtilsMessengerCreateInfoEXT info{};
            info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            info.pfnUserCallback = Debug;
            info.pUserData = this;
            const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (!create) throw std::runtime_error("Debug utils unavailable");
            Check(create(instance, &info, nullptr, &messenger_), "debug messenger");
        }
        Check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "physical devices");
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "physical devices");
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate, &props);
            if (props.apiVersion < VK_API_VERSION_1_3 || props.limits.maxComputeWorkGroupInvocations < 64 ||
                props.limits.maxComputeWorkGroupSize[0] < 64) continue;
            VkPhysicalDeviceVulkan13Features f13{};
            f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            VkPhysicalDeviceVulkan12Features f12{};
            f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            f12.pNext = &f13;
            VkPhysicalDeviceFeatures2 features{};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = &f12;
            vkGetPhysicalDeviceFeatures2(candidate, &features);
            if (!f12.timelineSemaphore || !f13.synchronization2) continue;
            std::uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, queues.data());
            for (std::uint32_t index = 0; index < families; ++index) {
                if (queues[index].queueCount && (queues[index].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                    physical = candidate; family = index; properties = props; break;
                }
            }
            if (physical) break;
        }
        if (!physical) throw std::runtime_error("Vulkan 1.3 compute/timeline/synchronization2 unavailable");
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr), "device extensions");
        available.resize(count);
        Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data()), "device extensions");
        extensions.clear();
        for (const auto& extension : available) {
            if (std::strcmp(extension.extensionName, "VK_KHR_portability_subset") == 0) extensions.push_back("VK_KHR_portability_subset");
        }
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features f13{};
        f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES; f13.synchronization2 = VK_TRUE;
        VkPhysicalDeviceVulkan12Features f12{};
        f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES; f12.timelineSemaphore = VK_TRUE; f12.pNext = &f13;
        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; deviceInfo.pNext = &f12;
        deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        deviceInfo.ppEnabledExtensionNames = extensions.data();
        Check(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);
    } catch (...) { Destroy(); throw; }
}
StreamingDevice::~StreamingDevice() { Destroy(); }
void StreamingDevice::Destroy() noexcept {
    if (device) { DrainForCleanup(device); vkDestroyDevice(device, nullptr); device = VK_NULL_HANDLE; }
    if (messenger_) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(instance, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance) { vkDestroyInstance(instance, nullptr); instance = VK_NULL_HANDLE; }
}

MappedStorageBuffer::MappedStorageBuffer(std::shared_ptr<StreamingDevice> context, VkDeviceSize bytes,
    VkMemoryPropertyFlags excludedMemoryFlags)
    : context_(std::move(context)), bytes_(bytes) {
    if (!context_ || bytes == 0 || bytes > context_->properties.limits.maxStorageBufferRange ||
        bytes > std::numeric_limits<std::size_t>::max()) throw std::invalid_argument("Storage buffer size");
    if (excludedMemoryFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) throw std::invalid_argument("Cannot exclude HOST_VISIBLE memory");
    try {
        VkBufferCreateInfo buffer{};
        buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; buffer.size = bytes;
        buffer.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(vkCreateBuffer(context_->device, &buffer, nullptr, &buffer_), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(context_->device, buffer_, &requirements);
        int best = -1;
        std::uint32_t type = 0;
        for (std::uint32_t index = 0; index < context_->memory.memoryTypeCount; ++index) {
            const auto flags = context_->memory.memoryTypes[index].propertyFlags;
            if (!(requirements.memoryTypeBits & (1u << index)) || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
                (flags & excludedMemoryFlags)) continue;
            const int score = ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? 4 : 0) +
                              ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? 2 : 0) +
                              ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 1 : 0);
            if (score > best) { best = score; type = index; flags_ = flags; }
        }
        if (best < 0) throw std::runtime_error("No compatible HOST_VISIBLE storage memory");
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = type;
        Check(vkAllocateMemory(context_->device, &allocation, nullptr, &allocation_), "vkAllocateMemory");
        Check(vkBindBufferMemory(context_->device, buffer_, allocation_, 0), "vkBindBufferMemory");
        Check(vkMapMemory(context_->device, allocation_, 0, VK_WHOLE_SIZE, 0, &mapped_), "vkMapMemory");
    } catch (...) { Destroy(); throw; }
}
MappedStorageBuffer::~MappedStorageBuffer() { Destroy(); }
void MappedStorageBuffer::Destroy() noexcept {
    if (mapped_) { vkUnmapMemory(context_->device, allocation_); mapped_ = nullptr; }
    if (buffer_) { vkDestroyBuffer(context_->device, buffer_, nullptr); buffer_ = VK_NULL_HANDLE; }
    if (allocation_) { vkFreeMemory(context_->device, allocation_, nullptr); allocation_ = VK_NULL_HANDLE; }
}
std::span<std::byte> MappedStorageBuffer::Bytes() const {
    return {static_cast<std::byte*>(mapped_), static_cast<std::size_t>(bytes_)};
}
void MappedStorageBuffer::Flush() {
    if (flags_ & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) return;
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; range.memory = allocation_;
    range.size = VK_WHOLE_SIZE; // Whole allocation satisfies nonCoherentAtomSize rules.
    Check(vkFlushMappedMemoryRanges(context_->device, 1, &range), "vkFlushMappedMemoryRanges");
}
void MappedStorageBuffer::Invalidate() {
    if (flags_ & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) return;
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; range.memory = allocation_; range.size = VK_WHOLE_SIZE;
    Check(vkInvalidateMappedMemoryRanges(context_->device, 1, &range), "vkInvalidateMappedMemoryRanges");
}

GpuAssetStreamer::GpuAssetStreamer(const std::filesystem::path& decodeSpirv,
    const std::filesystem::path& requestSpirv, bool validation, StreamingOptions options)
    : context_(std::make_shared<StreamingDevice>(validation)), options_(options) {
    if (options_.excludedMemoryFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) throw std::invalid_argument("Cannot exclude HOST_VISIBLE memory");
    try {
        const auto device = context_->device;
        for (unsigned mode = 0; mode < 2; ++mode) {
            VkDescriptorSetLayoutBinding bindings[2]{};
            const auto count = mode == 0 ? 2u : 1u;
            for (unsigned index = 0; index < count; ++index) {
                bindings[index].binding = index; bindings[index].descriptorCount = 1;
                bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo set{};
            set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; set.bindingCount = count; set.pBindings = bindings;
            auto& setHandle = mode == 0 ? decodeSet_ : requestSet_;
            auto& layout = mode == 0 ? decodeLayout_ : requestLayout_;
            Check(vkCreateDescriptorSetLayout(device, &set, nullptr, &setHandle), "descriptor layout");
            VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
            VkPipelineLayoutCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; info.setLayoutCount = 1; info.pSetLayouts = &setHandle;
            if (mode == 1) { info.pushConstantRangeCount = 1; info.pPushConstantRanges = &push; }
            Check(vkCreatePipelineLayout(device, &info, nullptr, &layout), "pipeline layout");
        }
        decodePipeline_ = CreatePipeline(decodeSpirv, decodeLayout_);
        requestPipeline_ = CreatePipeline(requestSpirv, requestLayout_);
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
        VkDescriptorPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pool.maxSets = 2; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
        Check(vkCreateDescriptorPool(device, &pool, nullptr, &descriptors_), "descriptor pool");
        VkCommandPoolCreateInfo commands{};
        commands.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; commands.queueFamilyIndex = context_->family;
        Check(vkCreateCommandPool(device, &commands, nullptr, &commands_), "command pool");
        VkCommandBufferAllocateInfo commandAllocation{};
        commandAllocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandAllocation.commandPool = commands_; commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandAllocation.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(device, &commandAllocation, &command_), "command allocation");
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO; type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO; semaphore.pNext = &type;
        Check(vkCreateSemaphore(device, &semaphore, nullptr, &timeline_), "timeline semaphore");
        worker_ = std::jthread([this](std::stop_token stop) { Worker(stop); });
    } catch (...) { Destroy(); throw; }
}
GpuAssetStreamer::~GpuAssetStreamer() {
    worker_.request_stop(); condition_.notify_all();
    if (worker_.joinable()) worker_.join();
    Destroy();
}
void GpuAssetStreamer::Destroy() noexcept {
    const auto device = context_->device;
    if (timeline_) vkDestroySemaphore(device, timeline_, nullptr);
    if (commands_) vkDestroyCommandPool(device, commands_, nullptr);
    if (descriptors_) vkDestroyDescriptorPool(device, descriptors_, nullptr);
    if (decodePipeline_) vkDestroyPipeline(device, decodePipeline_, nullptr);
    if (requestPipeline_) vkDestroyPipeline(device, requestPipeline_, nullptr);
    if (decodeLayout_) vkDestroyPipelineLayout(device, decodeLayout_, nullptr);
    if (requestLayout_) vkDestroyPipelineLayout(device, requestLayout_, nullptr);
    if (decodeSet_) vkDestroyDescriptorSetLayout(device, decodeSet_, nullptr);
    if (requestSet_) vkDestroyDescriptorSetLayout(device, requestSet_, nullptr);
}
VkPipeline GpuAssetStreamer::CreatePipeline(const std::filesystem::path& path, VkPipelineLayout layout) {
    const auto code = LoadSpirv(path);
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; info.codeSize = code.size() * 4; info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    Check(vkCreateShaderModule(context_->device, &info, nullptr, &module), "vkCreateShaderModule");
    VkComputePipelineCreateInfo compute{};
    compute.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO; compute.layout = layout;
    compute.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; compute.stage.module = module; compute.stage.pName = "main";
    VkPipeline pipeline = VK_NULL_HANDLE;
    const auto result = vkCreateComputePipelines(context_->device, VK_NULL_HANDLE, 1, &compute, nullptr, &pipeline);
    vkDestroyShaderModule(context_->device, module, nullptr);
    if (result != VK_SUCCESS && pipeline) vkDestroyPipeline(context_->device, pipeline, nullptr);
    Check(result, "vkCreateComputePipelines");
    return pipeline;
}

std::future<std::vector<StreamedAsset>> GpuAssetStreamer::StreamGpuDemandAsync(
    std::vector<std::filesystem::path> manifest, std::uint32_t stride) {
    if (manifest.empty() || stride == 0 || manifest.size() > UINT32_MAX) throw std::invalid_argument("Invalid asset demand");
    auto task = std::make_shared<std::packaged_task<std::vector<StreamedAsset>()>>(
        [this, files = std::move(manifest), stride] { return Stream(files, stride); });
    auto result = task->get_future();
    {
        std::lock_guard lock(mutex_);
        if (jobs_.size() >= 64) throw std::length_error("Streaming job queue full");
        jobs_.emplace_back([task] { (*task)(); });
    }
    condition_.notify_one();
    return result;
}
void GpuAssetStreamer::Worker(std::stop_token stop) {
    for (;;) {
        std::packaged_task<void()> task;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return stop.stop_requested() || !jobs_.empty(); });
            if (jobs_.empty()) return;
            task = std::move(jobs_.front()); jobs_.pop_front();
        }
        task(); // Exceptions propagate through the typed future.
    }
}

void GpuAssetStreamer::Dispatch(VkPipeline pipeline,
    std::span<const std::shared_ptr<MappedStorageBuffer>> buffers, std::uint32_t groupsX,
    std::uint32_t groupsY, const void* push, std::uint32_t pushBytes) {
    const auto device = context_->device;
    Check(vkResetDescriptorPool(device, descriptors_, 0), "reset descriptors");
    Check(vkResetCommandPool(device, commands_, 0), "reset commands");
    const bool decode = pipeline == decodePipeline_;
    const auto setLayout = decode ? decodeSet_ : requestSet_;
    const auto layout = decode ? decodeLayout_ : requestLayout_;
    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = descriptors_; allocation.descriptorSetCount = 1; allocation.pSetLayouts = &setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Check(vkAllocateDescriptorSets(device, &allocation, &set), "descriptor allocation");
    std::vector<VkDescriptorBufferInfo> infos(buffers.size());
    std::vector<VkWriteDescriptorSet> writes(buffers.size());
    for (std::size_t index = 0; index < buffers.size(); ++index) {
        infos[index] = {buffers[index]->Handle(), 0, buffers[index]->Size()};
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = set; writes[index].dstBinding = static_cast<std::uint32_t>(index);
        writes[index].descriptorCount = 1; writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[index].pBufferInfo = &infos[index];
    }
    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    const VkCommandBuffer command = command_;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
    VkMemoryBarrier2 before{};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    before.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; before.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
    before.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; before.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO; dependency.memoryBarrierCount = 1; dependency.pMemoryBarriers = &before;
    vkCmdPipelineBarrier2(command, &dependency);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    if (pushBytes) vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes, push);
    vkCmdDispatch(command, groupsX, groupsY, 1);
    VkMemoryBarrier2 after{};
    after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    after.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; after.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    after.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    dependency.pMemoryBarriers = &after; vkCmdPipelineBarrier2(command, &dependency);
    Check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
    if (value_ == UINT64_MAX) throw std::overflow_error("Timeline value overflow");
    ++value_;
    VkCommandBufferSubmitInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO; commandInfo.commandBuffer = command;
    VkSemaphoreSubmitInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO; signal.semaphore = timeline_; signal.value = value_;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2; submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &commandInfo;
    submit.signalSemaphoreInfoCount = 1; submit.pSignalSemaphoreInfos = &signal;
    {
        std::lock_guard lock(context_->queueMutex);
        const auto result = vkQueueSubmit2(context_->queue, 1, &submit, VK_NULL_HANDLE);
        if (result != VK_SUCCESS) DrainForCleanup(device);
        Check(result, "vkQueueSubmit2");
    }
    VkSemaphoreWaitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO; wait.semaphoreCount = 1;
    wait.pSemaphores = &timeline_; wait.pValues = &value_;
    const auto result = vkWaitSemaphores(device, &wait, UINT64_MAX);
    if (result != VK_SUCCESS) {
        // Error cleanup only: ensure buffers/pools are not destroyed in flight.
        std::lock_guard lock(context_->queueMutex); DrainForCleanup(device);
    }
    Check(result, "vkWaitSemaphores");
    for (const auto& buffer : buffers) buffer->Invalidate();
}

std::shared_ptr<MappedStorageBuffer> GpuAssetStreamer::Decode(const std::filesystem::path& path) {
    static_assert(std::endian::native == std::endian::little, "RLE1 files use little-endian words");
    std::ifstream file;
    file.rdbuf()->pubsetbuf(nullptr, 0);
    file.open(path, std::ios::binary | std::ios::ate);
    const auto fileBytes = file.tellg();
    if (!file || fileBytes < 28 || fileBytes % 4 != 0 ||
        fileBytes > context_->properties.limits.maxStorageBufferRange) throw std::runtime_error("Invalid RLE1 file size");
    auto input = std::make_shared<MappedStorageBuffer>(context_, static_cast<VkDeviceSize>(fileBytes), options_.excludedMemoryFlags);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(input->Bytes().data()), fileBytes)) throw std::runtime_error("Asset read failed");
    if (file.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Asset changed size during read");
    const auto* words = reinterpret_cast<const std::uint32_t*>(input->Bytes().data());
    const std::uint64_t outputWords = words[1], runs = words[2];
    if (words[0] != 0x31454c52u || words[3] != 0 || outputWords == 0 || runs == 0 ||
        (4 + runs * 3) * 4 != static_cast<std::uint64_t>(fileBytes) ||
        outputWords * 4 > context_->properties.limits.maxStorageBufferRange) throw std::runtime_error("Invalid RLE1 header");
    std::uint64_t expected = 0;
    for (std::uint64_t run = 0; run < runs; ++run) {
        const auto* record = words + 4 + run * 3;
        if (record[0] != expected || record[1] == 0 || record[1] > outputWords - expected) throw std::runtime_error("Invalid RLE1 coverage");
        expected += record[1];
    }
    if (expected != outputWords) throw std::runtime_error("Incomplete RLE1 coverage");
    input->Flush();
    auto output = std::make_shared<MappedStorageBuffer>(context_, outputWords * 4, options_.excludedMemoryFlags);
    const std::uint64_t groups = (outputWords + 63) / 64;
    auto maximumX = context_->properties.limits.maxComputeWorkGroupCount[0];
    if (options_.maximumDecodeGroupsX != 0) maximumX = std::min(maximumX, options_.maximumDecodeGroupsX);
    const auto x = std::min(groups, static_cast<std::uint64_t>(maximumX));
    const auto y = (groups + x - 1) / x;
    if (y > context_->properties.limits.maxComputeWorkGroupCount[1] || x * y * 64 > UINT32_MAX) throw std::length_error("GPU dispatch size");
    const std::shared_ptr<MappedStorageBuffer> buffers[]{input, output};
    Dispatch(decodePipeline_, buffers, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), nullptr, 0);
    return output;
}

std::vector<StreamedAsset> GpuAssetStreamer::Stream(std::span<const std::filesystem::path> manifest, std::uint32_t stride) {
    const auto count = static_cast<std::uint32_t>(manifest.size());
    const auto groups = (static_cast<std::uint64_t>(count) + 63) / 64;
    if (groups > context_->properties.limits.maxComputeWorkGroupCount[0]) throw std::length_error("GPU request count");
    auto requests = std::make_shared<MappedStorageBuffer>(context_, static_cast<VkDeviceSize>(count) * 4, options_.excludedMemoryFlags);
    const std::uint32_t policy[]{count, stride};
    const std::shared_ptr<MappedStorageBuffer> buffers[]{requests};
    Dispatch(requestPipeline_, buffers, static_cast<std::uint32_t>(groups), 1, policy, sizeof(policy));
    const auto* ids = reinterpret_cast<const std::uint32_t*>(requests->Bytes().data());
    std::vector<StreamedAsset> result;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto id = ids[index];
        if (id == UINT32_MAX) continue;
        if (id >= count) throw std::runtime_error("Invalid GPU asset request");
        result.push_back({id, Decode(manifest[id])});
    }
    return result;
}
