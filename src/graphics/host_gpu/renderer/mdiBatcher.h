#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MDIBATCHER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MDIBATCHER_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {

struct MdiHash128 {
	uint64_t lo = 0;
	uint64_t hi = 0;

	[[nodiscard]] bool operator==(const MdiHash128& other) const noexcept = default;
};

inline void MixMdiHash128(MdiHash128& h, uint64_t k) noexcept {
	uint64_t a = k ^ (k >> 33u);
	a *= 0xff51afd7ed558ccdULL;
	a ^= a >> 33u;
	a *= 0xc4ceb9fe1a85ec53ULL;
	a ^= a >> 33u;
	h.lo ^= a;
	h.lo = ((h.lo << 27u) | (h.lo >> 37u)) * 5ULL + 0x52dce729ULL;

	uint64_t b = (k + 0x9e3779b97f4a7c15ULL) ^ (h.lo >> 29u);
	b = (b ^ (b >> 30u)) * 0xbf58476d1ce4e5b9ULL;
	b = (b ^ (b >> 27u)) * 0x94d049bb133111ebULL;
	b ^= b >> 31u;
	h.hi ^= b;
	h.hi = ((h.hi << 31u) | (h.hi >> 33u)) * 5ULL + 0x38495ab5ULL;
}

[[nodiscard]] inline MdiHash128 ComputeVertexBufferHash(uint32_t                        count,
                                                        std::span<const vk::Buffer>     bufs,
                                                        std::span<const vk::DeviceSize> offs,
                                                        std::span<const vk::DeviceSize> szs) noexcept {
	MdiHash128 hash {0x9e3779b97f4a7c15ULL, 0x6a09e667f3bcc908ULL};
	MixMdiHash128(hash, count);
	for (uint32_t i = 0; i < count; ++i) {
		MixMdiHash128(hash, reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(bufs[i])));
		MixMdiHash128(hash, offs[i]);
		MixMdiHash128(hash, szs[i]);
	}
	return hash;
}

struct MdiBatchKey {
	VkPipeline  pipeline           = nullptr;
	VkBuffer    index_buffer       = nullptr;
	uint64_t    index_offset       = 0;
	VkIndexType index_type         = {};
	uint32_t    topology           = 0;
	uint32_t    object_id          = 0;
	MdiHash128  vertex_buffer_hash = {};
	MdiHash128  bindings_hash      = {};

	[[nodiscard]] bool operator==(const MdiBatchKey& other) const noexcept = default;
};

struct BoundGraphicsState {
	static constexpr uint32_t MaxVertexBuffers      = 32;
	static constexpr uint32_t MaxPushConstantDwords = 32;

	vk::Pipeline                                     pipeline            = nullptr;
	vk::Buffer                                       index_buffer        = nullptr;
	vk::DeviceSize                                   index_offset        = 0;
	vk::IndexType                                    index_type          = {};
	bool                                             has_index           = false;
	uint32_t                                         vertex_count        = 0;
	uint32_t                                         active_vertex_count = 0;
	MdiHash128                                       vertex_buffer_hash =
	    ComputeVertexBufferHash(0, {}, {}, {});
	std::array<vk::Buffer, MaxVertexBuffers>         vertex_buffers {};
	std::array<vk::DeviceSize, MaxVertexBuffers>     vertex_offsets {};
	std::array<vk::DeviceSize, MaxVertexBuffers>     vertex_sizes {};

	VkPipelineLayout                                 set0_layout = nullptr;
	MdiHash128                                       set0_hash {};
	bool                                             has_set0 = false;

	VkPipelineLayout                                 push_layout      = nullptr;
	uint32_t                                         push_stages      = 0;
	uint32_t                                         push_dword_count = 0;
	std::array<uint32_t, MaxPushConstantDwords>      push_dwords {};

	void Reset() noexcept { *this = BoundGraphicsState {}; }

	void InvalidateDescriptorSet0() noexcept {
		set0_layout = nullptr;
		set0_hash   = {};
		has_set0    = false;
	}

	void InvalidatePushConstants() noexcept {
		push_layout      = nullptr;
		push_stages      = 0;
		push_dword_count = 0;
	}

	[[nodiscard]] bool ShouldBindPipeline(vk::Pipeline next) noexcept {
		if (pipeline == next) {
			return false;
		}
		pipeline = next;
		return true;
	}

	[[nodiscard]] bool ShouldBindIndexBuffer(vk::Buffer buf, vk::DeviceSize off,
	                                         vk::IndexType type) noexcept {
		if (has_index && index_buffer == buf && index_offset == off && index_type == type) {
			return false;
		}
		index_buffer = buf;
		index_offset = off;
		index_type   = type;
		has_index    = true;
		return true;
	}

	[[nodiscard]] bool MatchesActiveVertexBuffers(uint32_t                        count,
	                                              std::span<const vk::Buffer>     bufs,
	                                              std::span<const vk::DeviceSize> offs,
	                                              std::span<const vk::DeviceSize> szs) const noexcept {
		if (active_vertex_count != count) {
			return false;
		}
		for (uint32_t i = 0; i < count; ++i) {
			if (vertex_buffers[i] != bufs[i] || vertex_offsets[i] != offs[i] ||
			    vertex_sizes[i] != szs[i]) {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] bool ShouldBindVertexBuffers(uint32_t                        count,
	                                           std::span<const vk::Buffer>     bufs,
	                                           std::span<const vk::DeviceSize> offs,
	                                           std::span<const vk::DeviceSize> szs) noexcept {
		bool same = vertex_count >= count;
		for (uint32_t i = 0; same && i < count; ++i) {
			same = vertex_buffers[i] == bufs[i] && vertex_offsets[i] == offs[i] &&
			       vertex_sizes[i] == szs[i];
		}
		if (same) {
			if (active_vertex_count != count) {
				active_vertex_count = count;
				vertex_buffer_hash  = ComputeVertexBufferHash(count, bufs, offs, szs);
			}
			return false;
		}
		for (uint32_t i = 0; i < count; ++i) {
			vertex_buffers[i] = bufs[i];
			vertex_offsets[i] = offs[i];
			vertex_sizes[i]   = szs[i];
		}
		vertex_count        = std::max(vertex_count, count);
		active_vertex_count = count;
		vertex_buffer_hash  = ComputeVertexBufferHash(count, bufs, offs, szs);
		return true;
	}

	[[nodiscard]] bool ShouldBindDescriptorSet0(VkPipelineLayout  layout,
	                                            const MdiHash128& hash) noexcept {
		if (has_set0 && set0_layout == layout && set0_hash == hash) {
			return false;
		}
		set0_layout = layout;
		set0_hash   = hash;
		has_set0    = true;
		return true;
	}

	[[nodiscard]] bool ShouldPushConstants(VkPipelineLayout          layout,
	                                       uint32_t                  stages,
	                                       std::span<const uint32_t> dwords) noexcept {
		if (dwords.empty()) {
			return false;
		}
		if (dwords.size() > MaxPushConstantDwords) {
			InvalidatePushConstants();
			return true;
		}
		const auto count = static_cast<uint32_t>(dwords.size());
		if (push_layout == layout && push_stages == stages && push_dword_count >= count &&
		    std::equal(dwords.begin(), dwords.end(), push_dwords.begin())) {
			return false;
		}
		std::copy(dwords.begin(), dwords.end(), push_dwords.begin());
		push_layout      = layout;
		push_stages      = stages;
		push_dword_count = count;
		return true;
	}
};

struct StreamSliceCache {
	uint64_t                 tick = 0;
	std::vector<uint32_t>    words;
	vk::DescriptorBufferInfo info {};

	template <typename TickFn, typename UploadFn>
	[[nodiscard]] vk::DescriptorBufferInfo Resolve(vk::Buffer                stream_handle,
	                                               std::span<const uint32_t> payload,
	                                               TickFn&&                  current_tick,
	                                               UploadFn&&                upload) {
		const uint64_t now = current_tick();
		if (tick == now && info.buffer == stream_handle && std::ranges::equal(words, payload)) {
			return info;
		}
		info = upload(payload);
		tick = current_tick();
		words.assign(payload.begin(), payload.end());
		return info;
	}
};

template <typename TickFn, typename UploadFn>
inline void ResolveStageStreamUploads(vk::Buffer stream_handle, bool need_srt,
                                      std::span<const uint32_t> srt_payload,
                                      StreamSliceCache& srt_cache, vk::DescriptorBufferInfo& srt_out,
                                      bool need_data, std::span<const uint32_t> data_payload,
                                      StreamSliceCache&         data_cache,
                                      vk::DescriptorBufferInfo& data_out, TickFn&& current_tick,
                                      UploadFn&& upload) {
	if (need_srt) {
		srt_out = srt_cache.Resolve(stream_handle, srt_payload, current_tick, upload);
	}
	if (need_data) {
		data_out = data_cache.Resolve(stream_handle, data_payload, current_tick, upload);
	}
	// If uploading ShaderData wrapped StreamBuffer and retired the tick FlattenedSrt came from,
	// re-upload FlattenedSrt onto the active tick so both slices stay valid simultaneously.
	if (need_srt && need_data && srt_cache.tick != current_tick()) {
		srt_out = srt_cache.Resolve(stream_handle, srt_payload, current_tick, upload);
	}
}

// Selector 2 (kAfterWriteConfirm) is emitted after tiny (~3-draw) bundles in games like GYLT;
// keeping m_needs_flush false for selector 2 lets consecutive PM4 slices coalesce into MDI
// batches up to MaxDrawsInBatch(), while GuestGpu::ThreadRun still flushes immediately whenever
// the queue drains (m_submission_count == 0), blocks (!complete), or switches queues.
[[nodiscard]] constexpr bool ShouldFlushReleaseMemInterrupt(bool     has_listener,
                                                            uint32_t interrupt_selector) noexcept {
	return has_listener && (interrupt_selector == 1u || interrupt_selector == 4u);
}

class MdiBatcher {
public:
	static constexpr uint32_t MaxBatchCommands = 1024;

	MdiBatcher()  = default;
	~MdiBatcher() = default;

	KYTY_CLASS_NO_COPY(MdiBatcher);

	[[nodiscard]] bool               IsEmpty() const noexcept { return m_count == 0; }
	[[nodiscard]] uint32_t           Count() const noexcept { return m_count; }
	[[nodiscard]] bool               IsIndexed() const noexcept { return m_is_indexed; }
	[[nodiscard]] bool               IsActive() const noexcept { return m_active; }
	[[nodiscard]] const MdiBatchKey& CurrentKey() const noexcept { return m_current_key; }

	[[nodiscard]] bool CanBatch(const MdiBatchKey& key,
	                            uint32_t           max_commands = MaxBatchCommands) const noexcept {
		const uint32_t limit = max_commands < MaxBatchCommands ? max_commands : MaxBatchCommands;
		if (!m_active || m_count >= limit) {
			return false;
		}
		return m_current_key == key;
	}

	[[nodiscard]] bool MatchesScalars(VkPipeline  pipeline,
	                                  VkBuffer    index_buffer,
	                                  uint64_t    index_offset,
	                                  VkIndexType index_type,
	                                  uint32_t    topology,
	                                  uint32_t    object_id,
	                                  bool        indexed,
	                                  uint32_t    max_commands = MaxBatchCommands) const noexcept {
		const uint32_t limit = max_commands < MaxBatchCommands ? max_commands : MaxBatchCommands;
		if (!m_active || m_is_indexed != indexed || m_count >= limit) {
			return false;
		}
		return m_current_key.pipeline == pipeline && m_current_key.index_buffer == index_buffer &&
		       m_current_key.index_offset == index_offset &&
		       m_current_key.index_type == index_type && m_current_key.topology == topology &&
		       m_current_key.object_id == object_id;
	}

	void BeginBatch(const MdiBatchKey& key, bool indexed = true) {
		Reset();
		if (indexed && m_indexed_commands.capacity() < 256) {
			m_indexed_commands.reserve(256);
		} else if (!indexed && m_direct_commands.capacity() < 256) {
			m_direct_commands.reserve(256);
		}
		m_current_key = key;
		m_is_indexed  = indexed;
		m_active      = true;
	}

	void AppendIndexed(const vk::DrawIndexedIndirectCommand& cmd) {
		m_indexed_commands.push_back(cmd);
		m_count++;
	}

	void AppendDirect(const vk::DrawIndirectCommand& cmd) {
		m_direct_commands.push_back(cmd);
		m_count++;
	}

	[[nodiscard]] bool TakeBatch(std::vector<vk::DrawIndexedIndirectCommand>& out_indexed,
	                             std::vector<vk::DrawIndirectCommand>&        out_direct,
	                             bool&                                        out_is_indexed) noexcept {
		if (m_count == 0) {
			Reset();
			return false;
		}
		out_is_indexed = m_is_indexed;
		out_indexed.clear();
		out_direct.clear();
		if (m_is_indexed) {
			out_indexed.swap(m_indexed_commands);
		} else {
			out_direct.swap(m_direct_commands);
		}
		Reset();
		return true;
	}

	void Reset() noexcept {
		m_indexed_commands.clear();
		m_direct_commands.clear();
		m_count       = 0;
		m_is_indexed  = false;
		m_active      = false;
		m_current_key = {};
	}

	[[nodiscard]] std::span<const vk::DrawIndexedIndirectCommand> GetIndexedCommands() const noexcept {
		return m_indexed_commands;
	}

	[[nodiscard]] std::span<const vk::DrawIndirectCommand> GetDirectCommands() const noexcept {
		return m_direct_commands;
	}

private:
	MdiBatchKey                                 m_current_key {};
	std::vector<vk::DrawIndexedIndirectCommand> m_indexed_commands;
	std::vector<vk::DrawIndirectCommand>        m_direct_commands;
	uint32_t                                    m_count      = 0;
	bool                                        m_is_indexed = false;
	bool                                        m_active     = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MDIBATCHER_H_
