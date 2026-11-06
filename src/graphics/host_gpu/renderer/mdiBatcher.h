#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MDIBATCHER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MDIBATCHER_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {

struct MdiBatchKey {
	VkPipeline  pipeline           = nullptr;
	VkBuffer    index_buffer       = nullptr;
	uint64_t    index_offset       = 0;
	VkIndexType index_type         = {};
	uint32_t    topology           = 0;
	uint32_t    object_id          = 0;
	uint64_t    vertex_buffer_hash = 0;
	uint64_t    bindings_hash      = 0;

	[[nodiscard]] bool operator==(const MdiBatchKey& other) const noexcept = default;
};

class MdiBatcher {
public:
	static constexpr uint32_t MaxBatchCommands = 1024;

	MdiBatcher()  = default;
	~MdiBatcher() = default;

	KYTY_CLASS_NO_COPY(MdiBatcher);

	[[nodiscard]] bool IsEmpty() const noexcept { return m_count == 0; }
	[[nodiscard]] uint32_t Count() const noexcept { return m_count; }
	[[nodiscard]] bool IsIndexed() const noexcept { return m_is_indexed; }
	[[nodiscard]] bool IsActive() const noexcept { return m_active; }
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
