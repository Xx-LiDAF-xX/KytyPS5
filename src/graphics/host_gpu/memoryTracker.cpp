#include "graphics/host_gpu/memoryTracker.h"

#include "common/alignment.h"
#include "common/assert.h"

namespace Libs::Graphics {

static_assert(std::atomic<void*>::is_always_lock_free);

MemoryTracker::MemoryTracker(PageManager& page_manager): m_page_manager(page_manager) {
	m_regions = std::make_unique<std::atomic<RegionManager*>[]>(REGION_COUNT);
}

MemoryTracker::~MemoryTracker() = default;

#if KYTY_BUILD == KYTY_BUILD_DEBUG
void MemoryTracker::ValidateGpuDirtyPages(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
                                          const char* operation) const noexcept {
	if (!GuestRange {vaddr, size}.Valid() || (vaddr & (TRACKER_PAGE_SIZE - 1)) != 0 ||
	    (size & (TRACKER_PAGE_SIZE - 1)) != 0) {
		EXIT("MemoryTracker: invalid dirty-page validation range\n");
	}
	for (auto page = vaddr; page < vaddr + size; page += TRACKER_PAGE_SIZE) {
		if (!dirty.Intersects(page, TRACKER_PAGE_SIZE)) {
			EXIT("MemoryTracker: GPU-dirty tracker page has no dirty bytes, operation=%s "
			     "addr=0x%016" PRIx64 "\n",
			     operation, page);
		}
	}
}

void MemoryTracker::ValidateGpuDirtyOwnership(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
                                              const char* operation) {
	ValidateRange(vaddr, size);
	const auto begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	for (auto page = begin; page < end; page += TRACKER_PAGE_SIZE) {
		const bool has_dirty_bytes = dirty.Intersects(page, TRACKER_PAGE_SIZE);
		if (IsRegionGpuModified(page, TRACKER_PAGE_SIZE) != has_dirty_bytes) {
			EXIT("MemoryTracker: tracker and byte ownership disagree, operation=%s "
			     "addr=0x%016" PRIx64 "\n",
			     operation, page);
		}
	}
}
#endif

void MemoryTracker::ValidateRange(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("invalid memory tracker range\n");
	}
}

RegionManager* MemoryTracker::GetOrCreateRegion(uint64_t index) {
	if (auto* manager = m_regions[index].load(std::memory_order_acquire); manager != nullptr) {
		return manager;
	}
	std::lock_guard lock(m_region_mutex);
	if (auto* manager = m_regions[index].load(std::memory_order_acquire); manager != nullptr) {
		return manager;
	}
	auto  manager = std::make_unique<RegionManager>(m_page_manager, index * TRACKER_REGION_SIZE);
	auto* ptr     = manager.get();
	m_region_storage.push_back(std::move(manager));
	m_regions[index].store(ptr, std::memory_order_release);
	return ptr;
}

// Queries take no lock: dirty words are read atomically, and a query runs on the GPU thread, which
// alone writes GPU-dirty bits while other threads only set CPU-dirty ones, so it sees one instant.
bool MemoryTracker::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return Iterate<true>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		return manager->IsModified<DirtySource::Cpu>(offset, bytes);
	});
}

bool MemoryTracker::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		return manager->IsModified<DirtySource::Gpu>(offset, bytes);
	});
}

void MemoryTracker::MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size) {
	Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		manager->ChangeState<DirtySource::Cpu, true>(offset, bytes);
	});
}

void MemoryTracker::UnmarkRegionAsGpuModified(uint64_t vaddr, uint64_t size) {
	Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		manager->ChangeState<DirtySource::Gpu, false>(offset, bytes);
	});
}

std::pair<uint64_t, uint64_t> MemoryTracker::FindNonGpuModifiedRange(uint64_t page_start,
                                                                      uint64_t cluster_start,
                                                                      uint64_t cluster_end) {
	ValidateRange(page_start, TRACKER_PAGE_SIZE);
	const auto region_index = page_start / TRACKER_REGION_SIZE;
	const auto region_base  = region_index * TRACKER_REGION_SIZE;
	const auto region_end   = region_base + TRACKER_REGION_SIZE;

	cluster_start = std::max(cluster_start, region_base);
	cluster_end   = std::min(cluster_end, region_end);
	if (cluster_start >= cluster_end || page_start < cluster_start || page_start >= cluster_end) {
		return {page_start, page_start + TRACKER_PAGE_SIZE};
	}

	auto* manager = m_regions[region_index].load(std::memory_order_acquire);
	if (manager == nullptr) {
		return {cluster_start, cluster_end};
	}

	std::scoped_lock lock(manager->lock);
	const size_t page_idx           = (page_start - region_base) / TRACKER_PAGE_SIZE;
	const size_t cluster_start_page = (cluster_start - region_base) / TRACKER_PAGE_SIZE;
	const size_t cluster_end_page   = (cluster_end - region_base) / TRACKER_PAGE_SIZE;

	const auto [safe_start_page, safe_end_page] =
	    manager->FindCleanPageRange<DirtySource::Gpu>(page_idx, cluster_start_page, cluster_end_page);

	return {region_base + safe_start_page * TRACKER_PAGE_SIZE,
	        region_base + safe_end_page * TRACKER_PAGE_SIZE};
}

} // namespace Libs::Graphics
