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
	static constexpr uint32_t MaxVertexBuffers = 32;

	vk::Pipeline                                     pipeline     = nullptr;
	vk::Buffer                                       index_buffer = nullptr;
	vk::DeviceSize                                   index_offset = 0;
	vk::IndexType                                    index_type   = {};
	bool                                             has_index    = false;
	uint32_t                                         vertex_count = 0;
	std::array<vk::Buffer, MaxVertexBuffers>         vertex_buffers {};
	std::array<vk::DeviceSize, MaxVertexBuffers>     vertex_offsets {};
	std::array<vk::DeviceSize, MaxVertexBuffers>     vertex_sizes {};

	void Reset() noexcept { *this = BoundGraphicsState {}; }

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
			return false;
		}
		for (uint32_t i = 0; i < count; ++i) {
			vertex_buffers[i] = bufs[i];
			vertex_offsets[i] = offs[i];
			vertex_sizes[i]   = szs[i];
		}
		vertex_count = std::max(vertex_count, count);
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
