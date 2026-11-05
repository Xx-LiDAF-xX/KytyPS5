#include "graphics/host_gpu/renderer/occlusionQueries.h"

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <algorithm>

namespace Libs::Graphics {

// Queries no dump has read yet; past them a draw counts one sample.
constexpr uint32_t QUERY_COUNT = 16384;

OcclusionQueries::OcclusionQueries(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_scheduler(scheduler) {
	if (!graphics.occlusion_queries_enabled) {
		return;
	}
	m_device = graphics.device;
	const vk::QueryPoolCreateInfo info {.queryType  = vk::QueryType::eOcclusion,
	                                    .queryCount = QUERY_COUNT};
	RequireVulkanSuccess(m_device.createQueryPool(&info, nullptr, &m_pool), "vkCreateQueryPool");
	m_device.resetQueryPool(m_pool, 0, QUERY_COUNT);
	m_results.resize(QUERY_COUNT);
	if (graphics.occlusion_query_precise_enabled) {
		m_control = vk::QueryControlFlagBits::ePrecise;
	}
}

OcclusionQueries::~OcclusionQueries() {
	if (m_pool) {
		m_device.destroyQueryPool(m_pool, nullptr);
	}
}

int32_t OcclusionQueries::BeginQuery(vk::CommandBuffer command) {
	if (m_begun - m_read == QUERY_COUNT) {
		m_uncounted++;
		return -1;
	}
	const auto query = static_cast<uint32_t>(m_begun++ % QUERY_COUNT);
	command.beginQuery(m_pool, query, m_control);
	return static_cast<int32_t>(query);
}

void OcclusionQueries::Dump(uint64_t address) {
	// Begins dump to the results, ends 8 bytes into them.
	if ((address & 8u) == 0) {
		m_open++;
	} else if (m_open != 0) {
		m_open--;
	}
	if (m_begun - m_read >= QUERY_COUNT - 512) {
		Publish(true);
	}
	m_dumps.push_back({address, m_scheduler.CurrentTick(), m_begun, m_uncounted});
}

void OcclusionQueries::PublishDumps(bool wait) {
	if (m_dumps.empty()) {
		return;
	}

	size_t ready_count = 0;
	if (wait) {
		m_scheduler.Wait(m_dumps.back().tick);
		ready_count = m_dumps.size();
	} else {
		for (size_t i = 0; i < m_dumps.size(); ++i) {
			if (m_scheduler.IsFree(m_dumps[i].tick)) {
				ready_count = i + 1;
			} else {
				break;
			}
		}
		if (ready_count == 0) {
			return;
		}
	}

	const auto last = m_dumps[ready_count - 1].queries;
	if (last != m_read) {
		// The unread queries wrap around the pool at most once.
		for (uint64_t read = m_read; read < last;) {
			const auto first = static_cast<uint32_t>(read % QUERY_COUNT);
			const auto count =
			    static_cast<uint32_t>(std::min<uint64_t>(last - read, QUERY_COUNT - first));
			RequireVulkanSuccess(
			    m_device.getQueryPoolResults(m_pool, first, count, count * sizeof(uint64_t),
				                             m_results.data() + first, sizeof(uint64_t),
				                             vk::QueryResultFlagBits::e64),
			    "vkGetQueryPoolResults");
			m_device.resetQueryPool(m_pool, first, count);
			read += count;
		}
	}
	for (size_t i = 0; i < ready_count; ++i) {
		const auto& dump = m_dumps[i];
		for (; m_read < dump.queries; m_read++) {
			m_samples += m_results[m_read % QUERY_COUNT];
		}
		WriteZPassCounters(reinterpret_cast<volatile uint64_t*>(dump.address),
		                   m_samples + dump.uncounted, (dump.address & 8u) == 0);
	}
	if (ready_count == m_dumps.size()) {
		m_dumps.clear();
	} else {
		m_dumps.erase(m_dumps.begin(), m_dumps.begin() + ready_count);
	}
}

} // namespace Libs::Graphics
