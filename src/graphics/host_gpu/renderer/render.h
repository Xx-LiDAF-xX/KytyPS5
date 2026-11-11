#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/dlss.h"
#include "graphics/host_gpu/renderer/mdiBatcher.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <span>
#include <vector>

namespace Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

struct GraphicContext;
struct DrawCallInfo;
struct DrawEmitInfo;
struct DrawIndexBufferSource;
class RenderContext;
class CommandScheduler;
struct RenderExecutorTestAccess;

enum class DrawOffsetSource : uint8_t {
	DrawState,
	IndirectArgs,
};

// Nonzero address: the GPU reads count Vulkan indirect draw records, stride bytes apart.
struct DrawIndirectArgs {
	uint64_t              address = 0;
	uint32_t              count   = 0;
	uint32_t              stride  = 0;
	ShaderIndirectOffsets offsets;
};

struct DrawIndexArgs {
	uint32_t         index_count         = 0;
	const void*      index_addr          = nullptr;
	uint32_t         instance_count      = 0;
	uint32_t         index_type_and_size = 0;
	int32_t          base_vertex         = 0;
	uint32_t         first_instance      = 0;
	DrawOffsetSource offset_source       = DrawOffsetSource::DrawState;
	// With indirect records, index_count is INDEX_BUFFER_SIZE.
	DrawIndirectArgs indirect;
};

struct DrawAutoArgs {
	uint32_t         vertex_count   = 0;
	uint32_t         instance_count = 0;
	uint32_t         first_vertex   = 0;
	uint32_t         first_instance = 0;
	DrawOffsetSource offset_source  = DrawOffsetSource::DrawState;
	DrawIndirectArgs indirect;
};

struct SubmitInfo {
	static constexpr uint32_t MaxSemaphores = 3;

	std::array<vk::Semaphore, MaxSemaphores>          wait_semaphores {};
	std::array<uint64_t, MaxSemaphores>               wait_ticks {};
	std::array<vk::PipelineStageFlags, MaxSemaphores> wait_stages {};
	std::array<vk::Semaphore, MaxSemaphores>          signal_semaphores {};
	std::array<uint64_t, MaxSemaphores>               signal_ticks {};
	uint32_t                                          num_wait_semaphores   = 0;
	uint32_t                                          num_signal_semaphores = 0;

	void AddWait(vk::Semaphore semaphore, uint64_t tick = 1,
	             vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eAllCommands) {
		EXIT_IF(semaphore == nullptr || num_wait_semaphores >= MaxSemaphores);
		wait_semaphores[num_wait_semaphores] = semaphore;
		wait_ticks[num_wait_semaphores]      = tick;
		wait_stages[num_wait_semaphores++]   = stage;
	}

	void AddSignal(vk::Semaphore semaphore, uint64_t tick = 1) {
		EXIT_IF(semaphore == nullptr || num_signal_semaphores >= MaxSemaphores);
		signal_semaphores[num_signal_semaphores] = semaphore;
		signal_ticks[num_signal_semaphores++]    = tick;
	}
};

class CommandBuffer {
public:
	static_assert(BoundGraphicsState::MaxVertexBuffers == ShaderVertexInputInfo::RES_MAX);

	~CommandBuffer() = default;

	KYTY_CLASS_NO_COPY(CommandBuffer);

	[[nodiscard]] bool IsInvalid() const noexcept { return m_buffer == nullptr; }
	[[nodiscard]] bool IsRendering() const noexcept { return m_rendering; }
	[[nodiscard]] bool IsRenderingWith(const RenderState& state) const noexcept {
		return m_rendering && m_render_state == state;
	}

	void BeginRendering(const RenderState& state) const;
	void EndRendering() const;
	void InvalidateGraphicsState() const noexcept { m_bound_graphics_state.Reset(); }

	[[nodiscard]] BoundGraphicsState& GraphicsState() const noexcept {
		return m_bound_graphics_state;
	}
	[[nodiscard]] MdiBatcher& GetMdiBatcher() const noexcept { return m_mdi_batcher; }
	[[nodiscard]] bool TakeMdiBatch(
	    const std::vector<vk::DrawIndexedIndirectCommand>*& out_indexed,
	    const std::vector<vk::DrawIndirectCommand>*&        out_direct,
	    bool&                                               out_is_indexed) const noexcept {
		if (!m_mdi_batcher.TakeBatch(m_mdi_indexed_scratch, m_mdi_direct_scratch, out_is_indexed)) {
			return false;
		}
		out_indexed = &m_mdi_indexed_scratch;
		out_direct  = &m_mdi_direct_scratch;
		return true;
	}

	[[nodiscard]] vk::CommandBuffer Handle() const noexcept { return m_buffer; }
	[[nodiscard]] GraphicContext&   GetGraphics() const noexcept { return m_graphics; }
	[[nodiscard]] RenderContext&    GetContext() const noexcept { return m_context; }
	[[nodiscard]] HW::Context&      GetRegisters() const noexcept { return *m_registers; }
	[[nodiscard]] HW::UserConfig&   GetUserConfig() const noexcept { return *m_user_config; }
	[[nodiscard]] HW::Shader&       GetShaders() const noexcept { return *m_shaders; }

private:
	explicit CommandBuffer(CommandScheduler& scheduler);
	void Bind(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders) noexcept {
		m_registers   = &registers;
		m_user_config = &user_config;
		m_shaders     = &shaders;
	}

	void Begin();
	void End() const;

	RenderContext&                                      m_context;
	GraphicContext&                                     m_graphics;
	vk::CommandBuffer                                   m_buffer = nullptr;
	mutable RenderState                                 m_render_state;
	mutable bool                                        m_rendering = false;
	mutable BoundGraphicsState                          m_bound_graphics_state {};
	mutable MdiBatcher                                  m_mdi_batcher;
	mutable std::vector<vk::DrawIndexedIndirectCommand> m_mdi_indexed_scratch;
	mutable std::vector<vk::DrawIndirectCommand>        m_mdi_direct_scratch;
	HW::Context*                                        m_registers   = nullptr;
	HW::UserConfig*                                     m_user_config = nullptr;
	HW::Shader*                                         m_shaders     = nullptr;

	friend class CommandScheduler;
};

namespace Pssr {
struct Request;
}

class RenderExecutor {
public:
	struct GraphicsCommitExtra {
		uint32_t                draw_dword_count = 0;
		std::array<uint32_t, 5> draw_dwords {};
		uint32_t                user_clip_planes = 0;
		bool                    compute_mdi_info = false;
		bool                    has_precomputed_mdi_hash = false;
		MdiHash128              precomputed_mdi_bindings_hash {};
		MdiHash128              out_mdi_bindings_hash {};
		bool                    out_can_begin_mdi = false;
	};

	explicit RenderExecutor(RenderContext& context): m_context(context) {}
	~RenderExecutor();
	KYTY_CLASS_NO_COPY(RenderExecutor);

	void DispatchDirect(CommandBuffer& buffer, uint32_t thread_group_x, uint32_t thread_group_y,
	                    uint32_t thread_group_z, uint32_t mode, bool async_compute);
	void DispatchIndirect(CommandBuffer& buffer, uint64_t args_addr, uint32_t mode,
	                      bool async_compute);
	void ProcessPssr(CommandBuffer& buffer, const Pssr::Request& request);

	void PrepareBindings(const ShaderStageRuntime& runtime, PreparedBindings& prepared);
	void FindBuffers(std::span<PreparedBindings* const> stages);
	void RebindBuffers(PreparedBindings& bindings);
	void RebindImages(PreparedBindings& bindings);
	void CommitBindings(CommandBuffer& buffer, vk::PipelineBindPoint pipeline_bind_point,
	                    const PipelineCache::Pipeline&     pipeline,
	                    std::span<PreparedBindings* const> bindings,
	                    GraphicsCommitExtra*               gfx_extra = nullptr);
	[[nodiscard]] bool CanFastAppendMdiBindings(const CommandBuffer&               buffer,
	                                            const PipelineCache::Pipeline&     pipeline,
	                                            std::span<PreparedBindings* const> bindings,
	                                            uint32_t                           draw_dword_count,
	                                            std::span<const uint32_t>          draw_dwords,
	                                            uint32_t                           user_clip_planes,
	                                            const MdiHash128&                  expected_hash,
	                                            MdiHash128*                        out_hash = nullptr,
	                                            bool*                              out_eligible = nullptr);
	[[nodiscard]] TextureBinding ResolveTexture(const ShaderRecompiler::IR::ImageResource&   resource,
	                                            const ShaderRecompiler::IR::DescriptorValue& value);

	void FlushMdi(CommandBuffer& buffer);
	void InvalidateRenderTargetCache() noexcept { m_rt_cache.valid = false; }

private:
	// False, with nothing drawn, when the draw's indirect records need CPU counts.
	bool DrawIndex(CommandBuffer& buffer, const DrawIndexArgs& args);
	bool DrawAuto(CommandBuffer& buffer, const DrawAutoArgs& args);

	struct GraphicsBindings {
		std::array<PreparedBindings, 3> vertex;
		PreparedBindings                pixel;
	};

	[[nodiscard]] uint32_t MaxMdiCommands() noexcept;

	void PrepareGraphicsBindings(std::span<PreparedBindings* const> stages,
	                             std::span<RenderColorInfo>         colors,
	                             bool                               rebind_buffers = true);
	void ResolveRenderColorTarget(CommandBuffer& buffer, RenderColorInfo& target,
	                              uint32_t render_target_slot, bool ignore_target_mask = false,
	                              bool exact_format = false);
	void ResolveRenderDepthTarget(CommandBuffer& buffer, RenderDepthInfo& target);
	[[nodiscard]] bool DepthStencilCopy(CommandBuffer& buffer);
	[[nodiscard]] bool CopyDepthToColor(CommandBuffer& buffer, const DrawCallInfo& draw,
	                                    const DrawEmitInfo& emit, const DrawRenderState& state);
	[[nodiscard]] bool PrepareDrawRenderState(CommandBuffer& buffer, const DrawCallInfo& draw,
	                                          DrawRenderState& state);
	void               ExecutePreparedDraw(CommandBuffer& buffer, const DrawCallInfo& draw,
	                                       const DrawRenderState& state, const DrawEmitInfo& emit,
	                                       const DrawIndexBufferSource& index_source);
	[[nodiscard]] RenderState AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
	                                               uint32_t color_count, RenderDepthInfo& depth,
	                                               vk::ImageAspectFlags&              feedback_aspects,
	                                               std::span<PreparedBindings* const> stages = {});
	[[nodiscard]] bool        ResolveColorTargets(CommandBuffer& buffer);
	[[nodiscard]] bool        ConsumeMetadataColorOperation(CommandBuffer& buffer);
	void                      BindImage(ImageId id, bool storage);
	void                      BindRenderTarget(ImageId id);
	void                      ResetBindings();
	[[nodiscard]] bool        TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
	                                                     const CommandBuffer&          buffer);
	[[nodiscard]] bool TryConsumeComputeFill(const ShaderComputeInputInfo& input,
	                                         CommandBuffer& command, uint32_t group_x,
	                                         uint32_t group_y, uint32_t group_z, uint32_t mode);
	[[nodiscard]] bool Upscale(vk::CommandBuffer command, const Pssr::Request& request,
	                           Dlss::Inputs& inputs);

	// DLSS history of a PSSR context; a null feature is one DLSS refused or failed.
	struct DlssContext {
		uint64_t       context;
		Dlss::Creation creation;
		Dlss::Feature* feature;
	};

	// GET_LOD_STATS counters since their last reset. With --lod-stats a bound T# with counting
	// enabled counts as sampled at its BASE_LEVEL, clamped when its MIN_LOD lies above that level.
	struct MipStatsCounter {
		uint8_t finest_level = 15; // no samples
		bool    clamped      = false;
		bool    unclamped    = false;
	};

	static constexpr size_t ResolvedTextureCacheSize = 512;

	struct ResolvedTextureCacheEntry {
		uint64_t                              epoch        = 0;
		uint64_t                              resource_key = 0;
		ShaderRecompiler::IR::DescriptorValue value {};
		ImageId                               image_id {};
		TextureCache::ImageDesc               desc {};
	};

	struct RenderTargetPassCache {
		bool                                                       valid              = false;
		uint64_t                                                   resolve_epoch      = 0;
		uint32_t                                                   mrt_mask           = 0;
		uint32_t                                                   render_target_mask = 0;
		HW::AaConfig                                               aa_config {};
		std::array<HW::RenderTarget, RENDER_COLOR_ATTACHMENTS_MAX> rt_regs {};
		HW::DepthRenderTarget                                      depth_rt {};
		HW::RenderControl                                          render_control {};
		HW::DepthControl                                           depth_control {};
		HW::StencilControl                                         stencil_control {};
		HW::StencilMask                                            stencil_mask {};
		float                                                      depth_clear_value   = 0.0f;
		uint8_t                                                    stencil_clear_value = 0;
		uint32_t                                                   color_count         = 0;
		std::array<RenderColorInfo, RENDER_COLOR_ATTACHMENTS_MAX>  color_info {};
		RenderDepthInfo                                            depth_info {};
	};

	RenderContext&                         m_context;
	bool                                   m_lod_stats = Config::LodStatsEnabled();
	GraphicsBindings                       m_graphics_bindings;
	// The current draw's targets; PrepareDrawRenderState resolves them again for each draw.
	DrawTargets                            m_targets;
	PreparedBindings                       m_compute_bindings;
	std::vector<ImageId>                   m_bound_images;
	std::vector<vk::DescriptorImageInfo>   m_descriptor_images;
	std::vector<vk::WriteDescriptorSet>    m_descriptor_writes;
	std::vector<uint64_t>                  m_descriptor_hash_words;
	std::vector<ResolvedTextureCacheEntry> m_resolved_texture_cache {ResolvedTextureCacheSize};
	RenderTargetPassCache                  m_rt_cache {};
	vk::DescriptorSetLayout                m_pssr_descriptor_layout = nullptr;
	vk::PipelineLayout                     m_pssr_pipeline_layout   = nullptr;
	vk::Pipeline                           m_pssr_pipeline          = nullptr;
	std::vector<DlssContext>               m_dlss_contexts;
	std::array<MipStatsCounter, 256>       m_mip_stats {};
	uint32_t                               m_max_mdi_commands = 0;

	friend class CommandProcessor;
	friend class ImageHeap;
	friend struct RenderExecutorTestAccess;
};

// Reinterpret raw bits in src as an ImageView of dst_format via a shader when the host lacks
// VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT. Leave it to the caller if supported natively.
// Implemented in renderCompute.cpp.
[[nodiscard]] bool CanTranscodeBlockToUncompressedOnCompute();

[[nodiscard]] bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                                            uint32_t group_y, uint32_t group_z, uint32_t mode,
                                            ShaderBufferResource& descriptor,
                                            uint32_t& packed_clear, uint64_t& size);

} // namespace Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_
