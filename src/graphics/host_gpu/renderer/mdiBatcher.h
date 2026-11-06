#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MDIBATCHER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MDIBATCHER_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {

struct MdiBatchKey {
	VkPipeline pipeline            = nullptr;
	uint64_t   index_address       = 0;
	uint32_t   index_type_and_size = 0;
	uint32_t   topology            = 0;
	uint64_t   render_targets_hash = 0;

	[[nodiscard]] bool operator==(const MdiBatchKey& other) const noexcept {
		return pipeline == other.pipeline &&
		       index_address == other.index_address &&
		       index_type_and_size == other.index_type_and_size &&
		       topology == other.topology &&
		       render_targets_hash == other.render_targets_hash;
	}
};

class MdiBatcher {
public:
	MdiBatcher()  = default;
	~MdiBatcher() = default;

	KYTY_CLASS_NO_COPY(MdiBatcher);

	[[nodiscard]] bool IsEmpty() const noexcept { return m_count == 0; }
	[[nodiscard]] uint32_t Count() const noexcept { return m_count; }
	[[nodiscard]] bool IsIndexed() const noexcept { return m_is_indexed; }
	[[nodiscard]] bool IsActive() const noexcept { return m_active; }
	[[nodiscard]] const MdiBatchKey& CurrentKey() const noexcept { return m_current_key; }

	[[nodiscard]] bool CanBatch(const MdiBatchKey& key) const noexcept {
		if (!m_active) {
			return false;
		}
		return m_current_key == key;
	}

	void BeginBatch(const MdiBatchKey& key, bool indexed = true) {
		Reset();
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

	void Reset() {
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
	MdiBatchKey                                  m_current_key {};
	std::vector<vk::DrawIndexedIndirectCommand> m_indexed_commands;
	std::vector<vk::DrawIndirectCommand>        m_direct_commands;
	uint32_t                                     m_count      = 0;
	bool                                         m_is_indexed = false;
	bool                                         m_active     = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MDIBATCHER_H_
