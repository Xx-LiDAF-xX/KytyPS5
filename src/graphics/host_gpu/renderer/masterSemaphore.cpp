#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	if (result != vk::Result::eSuccess || m_semaphore == nullptr) {
		EXIT("vkCreateSemaphore (timeline) failed: %s (%d), null_handle=%d\n",
		     vk::to_string(result).c_str(), static_cast<int>(result), m_semaphore == nullptr);
	}
}

MasterSemaphore::~MasterSemaphore() {
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	if (result != vk::Result::eSuccess) {
		EXIT("vkGetSemaphoreCounterValue failed: %s (%d), known_gpu_tick=%llu next_tick=%llu\n",
		     vk::to_string(result).c_str(), static_cast<int>(result),
		     static_cast<unsigned long long>(KnownGpuTick()),
		     static_cast<unsigned long long>(CurrentTick()));
	}

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick) {
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	const auto result = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	if (result != vk::Result::eSuccess) {
		EXIT("vkWaitSemaphores failed: %s (%d), requested_tick=%llu known_gpu_tick=%llu next_tick=%llu timeout=UINT64_MAX\n",
		     vk::to_string(result).c_str(), static_cast<int>(result),
		     static_cast<unsigned long long>(tick),
		     static_cast<unsigned long long>(KnownGpuTick()),
		     static_cast<unsigned long long>(CurrentTick()));
	}
	Refresh();
}

} // namespace Libs::Graphics
