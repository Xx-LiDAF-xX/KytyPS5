#include "graphics/host_gpu/renderer/renderDraw.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

std::pair<int32_t, uint32_t> ResolveDrawOffsets(uint32_t index_offset,
	                                           const ShaderVertexInputInfo& vs_input_info) {
	auto     vertex_offset   = static_cast<int32_t>(index_offset);
	uint32_t instance_offset = 0;
	if (!vs_input_info.has_dynamic_vertex_fetch) {
		return {vertex_offset, instance_offset};
	}

	EXIT_IF(!vs_input_info.stage);
	const auto& program   = vs_input_info.stage.module->info;
	const auto& resources = *vs_input_info.stage.resources;
	if (index_offset == 0 &&
	    program.info.vertex_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.vertex_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			vertex_offset = static_cast<int32_t>(resources.user_data[index]);
		}
	}
	if (program.info.instance_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.instance_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			instance_offset = resources.user_data[index];
		}
	}

	return {vertex_offset, instance_offset};
}

static std::atomic<uint32_t> g_draw_state_log_count   = 0;
static std::atomic<uint32_t> g_draw_input_log_count   = 0;
static std::atomic<uint32_t> g_mrt_state_log_count    = 0;

static std::atomic<uint32_t> g_framebuffer_skip_log_count = 0;

static float ConvertPolygonOffsetConstantFactor(float guest_factor, const HW::PolyOffset& offset,
                                                vk::Format host_depth_format) {
	if (offset.db_is_float_fmt) {
		return guest_factor;
	}

	int host_depth_bits = 0;
	switch (host_depth_format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD16UnormS8Uint: host_depth_bits = 16; break;
		case vk::Format::eD24UnormS8Uint: host_depth_bits = 24; break;
		default:
			// A fixed-point guest bias cannot be represented exactly by a floating-point host
			// attachment without VK_EXT_depth_bias_control.
			return guest_factor;
	}
	return std::ldexp(guest_factor, host_depth_bits + offset.neg_num_db_bits);
}

static const char* RenderColorTypeName(const RenderColorInfo& color) {
	return color.image_id ? "RenderTexture" : "NoColorOutput";
}

static void LogFramebufferSkip(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               uint32_t index_count, uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	auto log_id = g_framebuffer_skip_log_count.fetch_add(1, std::memory_order_relaxed);
	if (log_id >= 128) {
		return;
	}

	LOGF(
	    "DrawFramebufferSkip[%u]: %s color=%s color_addr=0x%010" PRIx64 " color_size=0x%016" PRIx64
	    " color_image=%s depth_format=%s depth_image=%s depth_vaddr_num=%d target_mask=0x%08" PRIx32
	    " prim=%u index_count=%u flags=0x%08" PRIx32 "\n",
	    log_id, draw_name, RenderColorTypeName(color), color.desc.info.data.address,
	    color.desc.info.data.size, color.image_id ? "yes" : "no",
	    vk::to_string(depth.desc.view_info.format).c_str(), depth.image_id ? "yes" : "no",
	    static_cast<int>(!depth.desc.info.data.Empty()) +
	        static_cast<int>(depth.desc.info.HasStencil()),
	    ctx.GetRenderTargetMask(), static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags);
}

static void LogMrtState(const char* draw_name, const CommandBuffer& buffer,
                        const ShaderPixelInputInfo& ps_input_info) {
	const auto& ctx            = buffer.GetRegisters();
	const auto& sh_regs        = ctx.GetShaderRegisters();
	const auto  rt_mask        = ctx.GetRenderTargetMask();
	const auto  cb_shader_mask = sh_regs.m_cbShaderMask;
	const auto& bc0            = ctx.GetBlendControl(0);

	auto log_id = g_mrt_state_log_count.fetch_add(1);
	if (log_id >= 32) {
		return;
	}

	LOGF("MrtState[%u]: %s rt_mask=0x%08" PRIx32 " cb_shader_mask=0x%08" PRIx32
	     " blend0=%s src=%u dst=%u alpha_src=%u alpha_dst=%u sep_alpha=%s\n",
	     log_id, draw_name, rt_mask, cb_shader_mask, bc0.enable ? "true" : "false",
	     bc0.color_srcblend, bc0.color_destblend, bc0.alpha_srcblend, bc0.alpha_destblend,
	     bc0.separate_alpha_blend ? "true" : "false");

	for (uint32_t i = 0; i < 8; i++) {
		const auto& rt  = ctx.GetRenderTarget(i);
		const auto& bc  = ctx.GetBlendControl(i);
		const auto  ctm = (rt_mask >> (i * 4u)) & 0x0fu;
		const auto  csm = (cb_shader_mask >> (i * 4u)) & 0x0fu;

		if (rt.base.addr == 0 && ps_input_info.target_output_mode[i] == 0 && ctm == 0 && csm == 0 &&
		    !bc.enable) {
			continue;
		}

		LOGF("MrtState[%u]: slot=%u addr=0x%010" PRIx64
		     " target_mask=0x%x shader_mask=0x%x out_mode=%u"
		     " fmt=0x%08" PRIx32 " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32
		     " width=%u height=%u tile=%u"
		     " blend=%s src=%u dst=%u alpha_src=%u alpha_dst=%u\n",
		     log_id, i, rt.base.addr, ctm, csm, ps_input_info.target_output_mode[i],
		     static_cast<uint32_t>(rt.info.format), static_cast<uint32_t>(rt.info.channel_type),
		     static_cast<uint32_t>(rt.info.channel_order), rt.attrib2.width + 1,
		     rt.attrib2.height + 1, static_cast<uint32_t>(rt.attrib3.tile_mode),
		     bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.alpha_srcblend,
		     bc.alpha_destblend);
	}
}

static void LogDrawTargetState(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               const ShaderPixelInputInfo& ps_input_info, uint32_t index_count,
                               uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!color.image_id) {
		return;
	}

	auto log_id = g_draw_state_log_count.fetch_add(1);
	if (log_id >= 192) {
		return;
	}

	const auto& cc             = ctx.GetColorControl();
	const auto& bc             = ctx.GetBlendControl(color.target_slot);
	const auto& dc             = ctx.GetDepthControl();
	const auto& vp             = ctx.GetScreenViewport();
	const auto& vp0            = vp.viewports[0];
	const auto& ps_resources   = ps_input_info.stage.module->info.info;
	const auto  sampled_images = std::count_if(
	    ps_resources.images.begin(), ps_resources.images.end(), [](const auto& image) {
		    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
	    });

	const auto extent = color.Extent();
	const auto sc     = calc_final_scissor(vp, ctx.GetScanModeControl(), extent, 0);

	LOGF(
	    "DrawTargetState[%u]: frame=%d %s target=%s addr=0x%010" PRIx64
	    " extent=%ux%u prim=%u index_count=%u flags=0x%08" PRIx32 " color_mask=0x%08" PRIx32
	    " cc_mode=%u cc_op=0x%02x"
	    " blend=%s src=%u dst=%u comb=%u ps_tex=%d sampled=%d storage=%d ps_kill=%s target_mode0=%u"
	    " depth_test=%s depth_write=%s depth_func=%u depth_clear=%s viewport=(%.1f,%.1f %.1fx%.1f) "
	    "scissor=(%d,%d)-(%d,%d)\n",
	    log_id, buffer.GetContext().GetGpu().GetFrameNum(), draw_name, RenderColorTypeName(color),
	    color.desc.info.data.address, extent.width, extent.height,
	    static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags, ctx.GetRenderTargetMask(),
	    cc.mode, cc.op,
	    bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.color_comb_fcn,
	    static_cast<int>(ps_resources.images.size()), static_cast<int>(sampled_images),
	    static_cast<int>(ps_resources.images.size() - sampled_images),
	    ps_input_info.ps_pixel_kill_enable ? "true" : "false", ps_input_info.target_output_mode[0],
	    dc.z_enable ? "true" : "false", dc.z_write_enable ? "true" : "false", dc.zfunc,
	    depth.depth_clear_enable ? "true" : "false", vp0.xoffset - vp0.xscale,
	    vp0.yoffset - vp0.yscale, vp0.xscale * 2.0f, vp0.yscale * 2.0f, sc.left, sc.top, sc.right,
	    sc.bottom);

	LogMrtState(draw_name, buffer, ps_input_info);
}

static void LogDrawInputState(const CommandBuffer& buffer, const RenderColorInfo& color,
                              const ShaderVertexInputInfo& vs_input_info,
                              uint32_t index_type_and_size, uint32_t index_count,
                              const void* index_addr) {
	auto log_id = g_draw_input_log_count.fetch_add(1);
	if (log_id >= 64) {
		return;
	}

	LOGF("DrawInputState[%u]: frame=%d target=%s addr=0x%010" PRIx64
	     " index_type=%u index_count=%u index_addr=0x%016" PRIx64
	     " vs_resources=%d vs_buffers=%d\n",
	     log_id, buffer.GetContext().GetGpu().GetFrameNum(), RenderColorTypeName(color),
	     color.desc.info.data.address, index_type_and_size, index_count,
	     reinterpret_cast<uint64_t>(index_addr), vs_input_info.resources_num,
	     vs_input_info.buffers_num);

	for (int bi = 0; bi < vs_input_info.buffers_num; bi++) {
		const auto& b = vs_input_info.buffers[bi];
		LOGF("DrawInputState[%u]: vb[%d] addr=0x%010" PRIx64
		     " stride=%u records=%u fetch_index=%u\n",
		     log_id, bi, b.addr, b.stride, b.num_records, b.fetch_index);

		const auto* bytes = reinterpret_cast<const uint8_t*>(b.addr);
		if (bytes != nullptr && b.stride != 0) {
			const uint32_t records = std::min<uint32_t>(b.num_records, 4u);
			for (uint32_t rec = 0; rec < records; rec++) {
				const auto* rec_bytes = bytes + static_cast<uint64_t>(rec) * b.stride;
				const auto  dword_num = std::min<uint32_t>(b.stride / 4u, 12u);
				uint32_t    raw[12]   = {};
				float       flt[12]   = {};
				for (uint32_t i = 0; i < dword_num; i++) {
					std::memcpy(&raw[i], rec_bytes + i * 4u, sizeof(raw[i]));
					std::memcpy(&flt[i], rec_bytes + i * 4u, sizeof(flt[i]));
				}
				LOGF("DrawInputState[%u]: vb[%d].rec[%u] stride=%u dwords=%u raw=%08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " f=(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f)\n",
				     log_id, bi, rec, b.stride, dword_num, raw[0], raw[1], raw[2], raw[3], raw[4],
				     raw[5], raw[6], raw[7], raw[8], flt[0], flt[1], flt[2], flt[3], flt[4], flt[5],
				     flt[6], flt[7], flt[8]);

				for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
					const auto& r  = vs_input_info.resources[ai];
					const auto& rd = vs_input_info.resources_dst[ai];
					if (rd.buffer_index != bi) {
						continue;
					}
					const auto offset = static_cast<uint32_t>(r.Base48() - b.addr);
					if (offset + 4u <= b.stride &&
					    r.Format() == Prospero::BufferFormat::k8_8_8_8UNorm) {
						uint32_t packed = 0;
						std::memcpy(&packed, rec_bytes + offset, sizeof(packed));
						const auto r8 = (packed >> 0u) & 0xffu;
						const auto g8 = (packed >> 8u) & 0xffu;
						const auto b8 = (packed >> 16u) & 0xffu;
						const auto a8 = (packed >> 24u) & 0xffu;
						LOGF("DrawInputState[%u]: vb[%d].rec[%u].attr[%d] dst=v%d fmt=56 "
						     "rgba8=%02" PRIx32 "%02" PRIx32 "%02" PRIx32 "%02" PRIx32
						     " rgba=(%.3f,%.3f,%.3f,%.3f)\n",
						     log_id, bi, rec, ai, rd.register_start, r8, g8, b8, a8,
						     static_cast<double>(r8) / 255.0, static_cast<double>(g8) / 255.0,
						     static_cast<double>(b8) / 255.0, static_cast<double>(a8) / 255.0);
					}
				}
			}
		}

		for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
			const auto& r  = vs_input_info.resources[ai];
			const auto& rd = vs_input_info.resources_dst[ai];
			if (rd.buffer_index != bi) {
				continue;
			}
			LOGF("DrawInputState[%u]: attr[%d] offset=%u dst=v%d regs=%d fetch_index=%u "
			     "sharp=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
			     log_id, ai, static_cast<uint32_t>(r.Base48() - b.addr), rd.register_start,
			     rd.registers_num, rd.fetch_index, r.fields[0], r.fields[1], r.fields[2], r.fields[3]);
		}
	}
}

static void UpdateGraphicsDynamicState(const CommandBuffer& buffer, DynamicState& dynamic_state,
                                       const ShaderVertexInputInfo& vs_input_info,
                                       const RenderDepthInfo& depth, const RenderState& rendering) {
	KYTY_PROFILER_FUNCTION();

	const auto& ctx = buffer.GetRegisters();
	const auto&        vp  = ctx.GetScreenViewport();
	const vk::Extent2D framebuffer_extent {rendering.width, rendering.height};
	const auto& outputs = vs_input_info.stage.module->info.info.outputs;
	const bool  indexed_viewports =
	    std::any_of(outputs.begin(), outputs.end(), [](const auto& output) {
		    return output.kind == ShaderRecompiler::IR::StageOutputKind::ViewportIndex;
	    });
	constexpr uint32_t viewport_slots = std::size(HW::ScreenViewport {}.viewports);
	std::array<vk::Viewport, viewport_slots> viewports {};
	std::array<vk::Rect2D, viewport_slots>   scissors {};
	const uint32_t viewport_count = indexed_viewports ? viewport_slots : 1;
	for (uint32_t i = 0; i < viewport_count; i++) {
		const auto& guest    = vp.viewports[i];
		auto&       viewport = viewports[i];
		if (ctx.GetClipControl().clip_disable) {
			const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
			viewport.width  = static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u));
			viewport.height = static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u));
		} else {
			viewport.x      = guest.xoffset - guest.xscale;
			viewport.y      = guest.yoffset - guest.yscale;
			viewport.width  = guest.xscale * 2.0f;
			viewport.height = guest.yscale * 2.0f;
		}
		viewport.minDepth =
		    guest.zoffset - (ctx.GetClipControl().dx_clip_space ? 0.0f : guest.zscale);
		viewport.maxDepth = guest.zscale + guest.zoffset;

		const auto final_scissor =
		    calc_final_scissor(vp, ctx.GetScanModeControl(), framebuffer_extent, i);
		auto& scissor  = scissors[i];
		scissor.offset = {final_scissor.left, final_scissor.top};
		scissor.extent = {static_cast<uint32_t>(final_scissor.right - final_scissor.left),
		                  static_cast<uint32_t>(final_scissor.bottom - final_scissor.top)};
		if (viewport.width == 0.0f) {
			// Keep empty slots at their guest index; Vulkan requires a positive viewport width.
			viewport.width = 1.0f;
			scissor.extent = {0, 0};
		}
	}
	dynamic_state.SetViewports(std::span {viewports}.first(viewport_count));
	dynamic_state.SetScissors(std::span {scissors}.first(viewport_count));

	float line_width = ctx.GetLineWidth();
	if (line_width != 1.0f) {
		static bool logged = false;
		if (!logged) {
			LOGF("Render: temporary: clamping Vulkan line width %f to 1.0 because wideLines is "
			     "not enabled\n",
			     line_width);
			logged = true;
		}
		line_width = 1.0f;
	}
	dynamic_state.SetLineWidth(line_width);
	const auto&      blend = ctx.GetBlendColor();
	const std::array blend_constants {blend.red, blend.green, blend.blue, blend.alpha};
	dynamic_state.SetBlendConstants(blend_constants);
	dynamic_state.SetDepthTestEnabled(depth.depth_test_enable);
	dynamic_state.SetDepthWriteEnabled(depth.depth_write_enable);
	dynamic_state.SetDepthCompareOp(depth.depth_compare_op);
#if !defined(__APPLE__)
	dynamic_state.SetDepthBoundsTestEnabled(depth.depth_bounds_test_enable);
	if (depth.depth_bounds_test_enable) {
		dynamic_state.SetDepthBounds(ctx.GetDepthBoundsMin(), ctx.GetDepthBoundsMax());
	}
#endif

	const auto& mode              = ctx.GetModeControl();
	const auto& poly_offset       = ctx.GetPolyOffset();
	const bool  use_front         = mode.poly_offset_front_enable && !mode.cull_front;
	const bool  use_back          = mode.poly_offset_back_enable && !mode.cull_back;
	const bool  depth_bias_enable = use_front || use_back;
	dynamic_state.SetDepthBiasEnabled(depth_bias_enable);
	if (depth_bias_enable) {
		// Vulkan has one bias for both faces. Prefer a visible front face when both are enabled.
		const float guest_constant_factor =
		    use_front ? poly_offset.front_offset : poly_offset.back_offset;
		const float constant_factor = ConvertPolygonOffsetConstantFactor(
		    guest_constant_factor, poly_offset, depth.desc.view_info.format);
		const float slope_factor =
		    (use_front ? poly_offset.front_scale : poly_offset.back_scale) / 16.0f;
		dynamic_state.SetDepthBias(constant_factor, poly_offset.clamp, slope_factor);
	}

	// Rect lists are drawn without culling.
	vk::CullModeFlags cull_mode {};
	if (!Prospero::IsRectList(buffer.GetUserConfig().GetPrimType())) {
		if (mode.cull_front) {
			cull_mode |= vk::CullModeFlagBits::eFront;
		}
		if (mode.cull_back) {
			cull_mode |= vk::CullModeFlagBits::eBack;
		}
	}
	dynamic_state.SetCullMode(cull_mode);
	dynamic_state.SetFrontFace(mode.face ? vk::FrontFace::eClockwise
	                                     : vk::FrontFace::eCounterClockwise);

	dynamic_state.SetStencilTestEnabled(depth.stencil_test_enable);
	if (depth.stencil_test_enable) {
		const auto& front = depth.stencil_front;
		const auto& back  = depth.stencil_back;
		dynamic_state.SetStencilOps({front.failOp, front.passOp, front.depthFailOp, front.compareOp},
		                            {back.failOp, back.passOp, back.depthFailOp, back.compareOp});
		dynamic_state.SetStencilCompareMasks(front.compareMask, back.compareMask);
		dynamic_state.SetStencilWriteMasks(front.writeMask, back.writeMask);
		dynamic_state.SetStencilReferences(front.reference, back.reference);
	}
}

static bool DrawHasValidVertexShader(const HW::Shader& sh_ctx) {

	const auto& vs = sh_ctx.GetVs();
	return vs.es_regs.data_addr != 0;
}

struct DrawCallInfo {
	bool     indexed        = true;
	uint32_t index_count    = 0;
	uint32_t instance_count = 0;

	[[nodiscard]] const char* Name() const { return indexed ? "DrawIndex" : "DrawIndexAuto"; }
};

RenderState RenderExecutor::AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
                                                 uint32_t color_count, RenderDepthInfo& depth,
                                                 vk::ImageAspectFlags& feedback_aspects,
                                                 std::span<PreparedBindings* const> stages) {
	EXIT_IF(colors == nullptr || color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	feedback_aspects = {};
	auto&       cache = m_context.GetTextureCache();
	RenderState state {};
	state.width                 = std::numeric_limits<uint32_t>::max();
	state.height                = std::numeric_limits<uint32_t>::max();
	state.num_layers            = std::numeric_limits<uint32_t>::max();
	state.num_color_attachments = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		auto& target = colors[i];
		EXIT_IF(!target.image_id);
		const auto image_view = cache.FindRenderTarget(target.image_id, target.desc);
		auto&      image      = cache.GetImage(target.image_id);
		EXIT_IF(image.backing.samples != target.desc.info.samples || image_view == nullptr);
		const auto& view   = target.desc.view_info;
		const auto  layout = image.binding.is_bound ? vk::ImageLayout::eGeneral
		                                            : vk::ImageLayout::eColorAttachmentOptimal;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access =
		    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
		image.Transit(layout, image.binding.attachment_access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.Handle());
		const auto extent       = target.Extent();
		state.width             = std::min(state.width, extent.width);
		state.height            = std::min(state.height, extent.height);
		state.num_layers        = std::min(state.num_layers, view.layer_count);
		state.num_color_attachments = std::max(state.num_color_attachments, target.target_slot + 1);
		auto& attachment            = state.color_attachments[target.target_slot];
		attachment.image_view   = image_view;
		attachment.image_layout = layout;
	}
	if (depth.image_id) {
		const auto  image_view = cache.FindDepthTarget(depth.image_id, depth.desc);
		const auto& metadata   = depth.desc.info.metadata;
		const bool meta_clear =
		    metadata.kind == ImageMetadataKind::Htile &&
		    cache.IsMetaCleared(metadata.range.address, depth.desc.view_info.base_layer);
		depth.load_clear_aspects = {};
		if (depth.depth_clear_enable || meta_clear)
			depth.load_clear_aspects |= vk::ImageAspectFlagBits::eDepth;
		// Apply pending clears to compressed stencil as well as depth.
		if (depth.stencil_clear_enable || (meta_clear && metadata.stencil_compressed))
			depth.load_clear_aspects |= vk::ImageAspectFlagBits::eStencil;
		if (meta_clear &&
		    !cache.TouchMeta(metadata.range.address, depth.desc.view_info.base_layer, false)) {
			EXIT("failed to consume HTile clear state\n");
		}
		auto& image = cache.GetImage(depth.image_id);
		EXIT_IF(image_view == nullptr || image.backing.samples != depth.desc.info.samples);
		const auto draw_writes = depth.AttachmentWriteAspects();
		vk::ImageAspectFlags sampled_aspects;
		for (const auto* stage: stages) {
			for (const auto& binding: stage->images) {
				if (binding.image_id != depth.image_id ||
				    binding.desc.type != TextureCache::BindingType::Texture) continue;
				const auto native =
				    std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
				EXIT_IF(native == image.views.end());
				sampled_aspects |= native->info.aspect;
				feedback_aspects |= DepthFeedbackAspects(draw_writes, depth.desc.view_info,
				                                         native->info);
			}
		}
		if (feedback_aspects && !m_context.GetGraphics().attachment_feedback_loop_enabled) {
			static std::atomic_bool logged = false;
			if (!logged.exchange(true, std::memory_order_relaxed)) {
				std::printf("depth attachment feedback loop is not supported by the host\n");
			}
		}
		auto layout = depth_attachment_layout(depth);
		if (sampled_aspects & ~DepthReadableAspects(layout)) {
			layout = m_context.GetGraphics().attachment_feedback_loop_enabled
			             ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
			             : vk::ImageLayout::eGeneral;
		}
		// The attachment store writes even when guest depth/stencil tests do not.
		const auto access = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
		                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access = access;
		const auto& view                = depth.desc.view_info;
		image.Transit(layout, access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.Handle());
		state.width               = std::min(state.width, depth.desc.info.extent.width);
		state.height              = std::min(state.height, depth.desc.info.extent.height);
		state.num_layers          = std::min(state.num_layers, view.layer_count);
		const auto aspects        = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		auto&      attachment     = state.depth_stencil_attachment;
		attachment.image_view     = image_view;
		attachment.image_layout   = layout;
		attachment.clear_value[0] = std::bit_cast<uint32_t>(depth.depth_clear_value);
		attachment.clear_value[1] = depth.stencil_clear_value;
		attachment.has_depth      = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eDepth);
		attachment.depth_clear =
		    static_cast<bool>(depth.load_clear_aspects & vk::ImageAspectFlagBits::eDepth);
		attachment.has_stencil    = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eStencil);
		attachment.stencil_clear =
		    static_cast<bool>(depth.load_clear_aspects & vk::ImageAspectFlagBits::eStencil);
	}
	if (color_count == 0 && !depth.image_id) {
		const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
		state.width        = limits.maxFramebufferWidth;
		state.height       = limits.maxFramebufferHeight;
	}
	if (state.num_layers == std::numeric_limits<uint32_t>::max()) {
		state.num_layers = 1;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.width == std::numeric_limits<uint32_t>::max() ||
	        state.height == std::numeric_limits<uint32_t>::max());
	return state;
}

enum class CbColorMode : uint8_t {
	Disable            = 0,
	Normal             = 1,
	EliminateFastClear = 2,
	Resolve            = 3,
	FmaskDecompress    = 5,
	DccDecompress      = 6,
};

bool RenderExecutor::ConsumeMetadataColorOperation(CommandBuffer& buffer) {
	const auto& ctx  = buffer.GetRegisters();
	const auto  mode = ctx.GetColorControl().mode;
	if (mode == static_cast<uint8_t>(CbColorMode::EliminateFastClear)) {
		FlushMdi(buffer);
		InvalidateRenderTargetCache();
		// FCE can precede any ordinary draw. Discover the target to materialize its native
		// clear metadata before the expanded image is sampled.
		RenderColorInfo target {};
		ResolveRenderColorTarget(buffer, target, 0, true);
		return true;
	}
	// Metadata/decompression shaders must not export colors into the expanded image.
	return mode == static_cast<uint8_t>(CbColorMode::FmaskDecompress) ||
	       mode == static_cast<uint8_t>(CbColorMode::DccDecompress);
}

// vertex_offset is the first vertex of non-indexed draws.
struct DrawEmitInfo {
	int32_t          vertex_offset  = 0;
	uint32_t         first_instance = 0;
	DrawIndirectArgs indirect;
};

struct DrawIndexBufferSource {
	uint64_t    address            = 0;
	const void* host_data          = nullptr;
	uint64_t    size               = 0;
	uint32_t    guest_element_size = 0;
};

// The task shader's inputs for an indirect mesh draw: Params in gpu_mesh_indirect.task.
struct MeshTaskParams {
	vk::DeviceAddress records        = 0;
	uint64_t          index_address  = 0;
	uint32_t          stride         = 0;
	uint32_t          index_size     = 0;
	uint32_t          index_limit    = 0;
	uint32_t          primitive_size = 0;
	uint32_t          group_step     = 0;
	uint32_t          max_groups     = 0;
	uint32_t          max_instances  = 0;
	uint32_t          max_total      = 0;
};

struct PreparedIndexBuffer {
	vk::Buffer     buffer = nullptr;
	vk::DeviceSize offset = 0;
	vk::IndexType  type   = vk::IndexType::eUint16;
};

static uint64_t VertexBufferDescriptorSize(int binding, const ShaderVertexInputInfo& info) {
	const auto& buffer = info.buffers[binding];
	if (buffer.stride != 0 || buffer.num_records == 0) {
		return static_cast<uint64_t>(buffer.stride) * buffer.num_records;
	}

	uint64_t size = 0;
	for (int i = 0; i < info.resources_num; i++) {
		if (info.resources_dst[i].buffer_index != binding) {
			continue;
		}
		const auto& resource = info.resources[i];
		// RDNA2 OOB_SELECT=2 only checks NumRecords != 0. A constant attribute still
		// fetches its entire format; NumRecords is not a byte count in this mode.
		const uint64_t extent = resource.OutOfBounds() == 2
		                            ? resource.Base48() - buffer.addr +
		                                  ShaderRecompiler::Format::GetFormatInfo(resource.Format()).byte_size
		                            : buffer.num_records;
		size = std::max(size, extent);
	}
	return size;
}

struct VertexBufferRange {
	uint64_t                     base_address  = 0;
	uint64_t                     requested_end = 0;
	uint64_t                     acquired_end  = 0;
	std::pair<Buffer*, uint64_t> binding;

	[[nodiscard]] uint64_t RequestedSize() const { return requested_end - base_address; }
};

struct PreparedVertexBuffers {
	static constexpr uint32_t MaxBuffers = ShaderVertexInputInfo::RES_MAX;

	std::array<vk::Buffer, MaxBuffers>     buffers {};
	std::array<vk::DeviceSize, MaxBuffers> offsets {};
	std::array<vk::DeviceSize, MaxBuffers> sizes {};
	uint32_t                               count = 0;
};

static PreparedVertexBuffers AcquireVertexBuffers(CommandBuffer&               buffer,
                                                  const ShaderVertexInputInfo& vs_input_info) {
	EXIT_IF(vs_input_info.buffers_num < 0 ||
	        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX);

	// Collect the non-empty guest vertex ranges.
	std::array<uint64_t, ShaderVertexInputInfo::RES_MAX>          sizes {};
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> ranges {};
	uint32_t                                                      range_count = 0;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = VertexBufferDescriptorSize(i, vs_input_info);
		sizes[i]           = size;
		if (size == 0) {
			continue;
		}
		if (vertex.addr == 0 || size > UINT64_MAX - vertex.addr) {
			EXIT("invalid vertex buffer range: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     vertex.addr, size);
		}
		ranges[range_count++] = {vertex.addr, vertex.addr + size};
	}

	std::sort(ranges.begin(), ranges.begin() + range_count,
	          [](const VertexBufferRange& left, const VertexBufferRange& right) {
		          return left.base_address < right.base_address;
	          });

	// Merge overlapping or touching ranges before acquiring host buffers.
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> merged_ranges {};
	uint32_t                                                      merged_count = 0;
	for (uint32_t i = 0; i < range_count; i++) {
		const auto& range = ranges[i];
		if (merged_count != 0 &&
		    merged_ranges[merged_count - 1].requested_end >= range.base_address) {
			merged_ranges[merged_count - 1].requested_end =
			    std::max(merged_ranges[merged_count - 1].requested_end, range.requested_end);
			continue;
		}
		merged_ranges[merged_count++] = {range.base_address, range.requested_end};
	}

	auto& cache = buffer.GetContext().GetBufferCache();
	for (uint32_t i = 0; i < merged_count; i++) {
		auto& range = merged_ranges[i];
		// PPSA20298
		const auto size =
		    Libs::LibKernel::Memory::ClampRangeSize(range.base_address, range.RequestedSize());
		range.acquired_end = range.base_address + size;
		range.binding      = cache.ObtainBuffer(range.base_address, size, false);
	}

	// Rebuild slot bindings, offsetting non-empty slots into their acquired merged range.
	PreparedVertexBuffers prepared;
	prepared.count         = static_cast<uint32_t>(vs_input_info.buffers_num);
	vk::Buffer null_buffer = nullptr;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = sizes[i];
		if (size == 0) {
			if (null_buffer == nullptr) {
				null_buffer = cache.GetBuffer(NULL_BUFFER_ID).Handle();
			}
			prepared.buffers[i] = null_buffer;
			prepared.offsets[i] = 0;
			continue;
		}

		const auto range = std::find_if(merged_ranges.begin(), merged_ranges.begin() + merged_count,
		                                [&](const VertexBufferRange& value) {
			                                return vertex.addr >= value.base_address &&
			                                       vertex.addr < value.acquired_end;
		                                });
		if (range == merged_ranges.begin() + merged_count) {
			EXIT("vertex buffer address is outside the acquired range: addr=0x%016" PRIx64 "\n",
			     vertex.addr);
		}

		prepared.buffers[i] = range->binding.first->Handle();
		prepared.offsets[i] = range->binding.second + vertex.addr - range->base_address;
		prepared.sizes[i]   = std::min(size, range->acquired_end - vertex.addr);
	}

	return prepared;
}

static bool GetDrawTopology(const HW::UserConfig& ucfg, vk::PrimitiveTopology& topology) {

	topology = vk::PrimitiveTopology::ePointList;

	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kNone: return false;
		case Prospero::PrimitiveType::kPointList:
			topology = vk::PrimitiveTopology::ePointList;
			break;
		case Prospero::PrimitiveType::kLineList: topology = vk::PrimitiveTopology::eLineList; break;
		case Prospero::PrimitiveType::kLineStrip:
			topology = vk::PrimitiveTopology::eLineStrip;
			break;
		case Prospero::PrimitiveType::kTriList:
			topology = vk::PrimitiveTopology::eTriangleList;
			break;
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kPolygon:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		case Prospero::PrimitiveType::kTriStrip:
			topology = vk::PrimitiveTopology::eTriangleStrip;
			break;
		case Prospero::PrimitiveType::kPatch:
			if (!Config::TessellationEnabled()) return false;
			[[fallthrough]];
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kRectListLegacy:
			topology = vk::PrimitiveTopology::ePatchList;
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		default: {
			static std::atomic_bool logged = false;
			if (!logged.exchange(true, std::memory_order_relaxed)) {
				std::printf("Skipping draw with unknown primitive type: %u\n",
				            static_cast<uint32_t>(ucfg.GetPrimType()));
			}
			return false;
		}
	}

	return true;
}

static bool ResolvePrimitiveRestart(const CommandBuffer& buffer,
                                    const DrawIndexBufferSource& source) {
	const auto control = buffer.GetUserConfig().GetPrimitiveResetControl();
	EXIT_NOT_IMPLEMENTED((control & ~0x3u) != 0);
	if ((control & 0x1u) == 0) {
		return false;
	}
	switch (buffer.GetUserConfig().GetPrimType()) {
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip: break;
		default: return false;
	}

	const auto element_size = source.guest_element_size;
	const auto index_mask   = UINT32_MAX >> ((4 - element_size) * 8);
	const auto reset_index  = buffer.GetRegisters().GetPrimitiveResetIndex();
	if ((control & 0x2u) != 0 && (reset_index & ~index_mask) != 0) {
		return false;
	}
	const auto restart_index = reset_index & index_mask;
	if (restart_index == index_mask) {
		// Use native restart; the 8-bit path widens its marker to 0xffff.
		return true;
	}

	// A game can set a custom reset value without using it in the index buffer.
	// Keep restart off in that case; fail if we actually find the value.
	// Scan before preparing draw resources: readback can restart the command buffer.
	EXIT_NOT_IMPLEMENTED(source.address == 0);
	const auto* indices = reinterpret_cast<const uint8_t*>(source.address);
	for (uint64_t offset = 0; offset < source.size; offset += element_size) {
		uint32_t index = 0;
		std::memcpy(&index, indices + offset, element_size);
		EXIT_NOT_IMPLEMENTED(index == restart_index);
	}
	return false;
}

[[nodiscard]] static bool SameColorTargetRegs(const HW::RenderTarget& a,
                                              const HW::RenderTarget& b) noexcept {
	return a.base.addr == b.base.addr &&
	       a.view.base_array_slice_index == b.view.base_array_slice_index &&
	       a.view.last_array_slice_index == b.view.last_array_slice_index &&
	       a.view.current_mip_level == b.view.current_mip_level &&
	       a.info.fmask_compression_enable == b.info.fmask_compression_enable &&
	       a.info.fmask_data_compression_disable == b.info.fmask_data_compression_disable &&
	       a.info.fmask_one_frag_mode == b.info.fmask_one_frag_mode &&
	       a.info.cmask_fast_clear_enable == b.info.cmask_fast_clear_enable &&
	       a.info.dcc_compression_enable == b.info.dcc_compression_enable &&
	       a.info.blend_clamp == b.info.blend_clamp &&
	       a.info.blend_bypass == b.info.blend_bypass &&
	       a.info.round_mode == b.info.round_mode &&
	       a.info.format == b.info.format &&
	       a.info.channel_type == b.info.channel_type &&
	       a.info.channel_order == b.info.channel_order &&
	       a.attrib.force_dest_alpha_to_one == b.attrib.force_dest_alpha_to_one &&
	       a.attrib.num_samples == b.attrib.num_samples &&
	       a.attrib.num_fragments == b.attrib.num_fragments &&
	       a.attrib2.height == b.attrib2.height &&
	       a.attrib2.width == b.attrib2.width &&
	       a.attrib2.num_mip_levels == b.attrib2.num_mip_levels &&
	       a.attrib3.depth == b.attrib3.depth &&
	       a.attrib3.tile_mode == b.attrib3.tile_mode &&
	       a.attrib3.dimension == b.attrib3.dimension &&
	       a.attrib3.metadata_pipe_aligned == b.attrib3.metadata_pipe_aligned &&
	       a.attrib3.write_vrs_rate_hint_to_cmask == b.attrib3.write_vrs_rate_hint_to_cmask &&
	       a.dcc.max_uncompressed_block_size == b.dcc.max_uncompressed_block_size &&
	       a.dcc.max_compressed_block_size == b.dcc.max_compressed_block_size &&
	       a.dcc.color_transform == b.dcc.color_transform &&
	       a.dcc.dcc_clear_key_enable == b.dcc.dcc_clear_key_enable &&
	       a.dcc.overwrite_combiner_disable == b.dcc.overwrite_combiner_disable &&
	       a.dcc.independent_block_size == b.dcc.independent_block_size &&
	       a.dcc.data_write_on_dcc_clear_to_reg == b.dcc.data_write_on_dcc_clear_to_reg &&
	       a.cmask.addr == b.cmask.addr &&
	       a.fmask.addr == b.fmask.addr &&
	       a.clear_word0.word0 == b.clear_word0.word0 &&
	       a.clear_word1.word1 == b.clear_word1.word1 &&
	       a.dcc_addr.addr == b.dcc_addr.addr;
}

[[nodiscard]] static bool SameDepthTargetRegs(const HW::DepthRenderTarget& a,
                                              const HW::DepthRenderTarget& b) noexcept {
	return a.z_info.format == b.z_info.format &&
	       a.z_info.num_samples == b.z_info.num_samples &&
	       a.z_info.texture_compatibility == b.z_info.texture_compatibility &&
	       a.z_info.z_compare_base == b.z_info.z_compare_base &&
	       a.z_info.htile_acceleration == b.z_info.htile_acceleration &&
	       a.z_info.expclear_enabled == b.z_info.expclear_enabled &&
	       a.z_info.partially_resident == b.z_info.partially_resident &&
	       a.z_info.max_mip_level == b.z_info.max_mip_level &&
	       a.stencil_info.format == b.stencil_info.format &&
	       a.stencil_info.texture_compatibility == b.stencil_info.texture_compatibility &&
	       a.stencil_info.expclear_enabled == b.stencil_info.expclear_enabled &&
	       a.stencil_info.htile_stencil_disabled == b.stencil_info.htile_stencil_disabled &&
	       a.stencil_info.partially_resident == b.stencil_info.partially_resident &&
	       a.depth_view.slice_start == b.depth_view.slice_start &&
	       a.depth_view.slice_max == b.depth_view.slice_max &&
	       a.depth_view.current_mip_level == b.depth_view.current_mip_level &&
	       a.depth_view.depth_write_disable == b.depth_view.depth_write_disable &&
	       a.depth_view.stencil_write_disable == b.depth_view.stencil_write_disable &&
	       a.size.x_max == b.size.x_max &&
	       a.size.y_max == b.size.y_max &&
	       a.size.valid == b.size.valid &&
	       a.z_read_base_addr == b.z_read_base_addr &&
	       a.stencil_read_base_addr == b.stencil_read_base_addr &&
	       a.z_write_base_addr == b.z_write_base_addr &&
	       a.stencil_write_base_addr == b.stencil_write_base_addr &&
	       a.htile_data_base_addr == b.htile_data_base_addr &&
	       a.shading_rate_encoding == b.shading_rate_encoding;
}

[[nodiscard]] static bool SameAaConfig(const HW::AaConfig& a, const HW::AaConfig& b) noexcept {
	return a.msaa_num_samples == b.msaa_num_samples &&
	       a.aa_mask_centroid_dtmn == b.aa_mask_centroid_dtmn &&
	       a.max_sample_dist == b.max_sample_dist &&
	       a.msaa_exposed_samples == b.msaa_exposed_samples;
}

[[nodiscard]] static bool SameRenderControl(const HW::RenderControl& a,
                                            const HW::RenderControl& b) noexcept {
	return a.depth_clear_enable == b.depth_clear_enable &&
	       a.stencil_clear_enable == b.stencil_clear_enable &&
	       a.resummarize_enable == b.resummarize_enable &&
	       a.stencil_compress_disable == b.stencil_compress_disable &&
	       a.depth_compress_disable == b.depth_compress_disable &&
	       a.copy_depth_to_color == b.copy_depth_to_color &&
	       a.copy_stencil_to_color == b.copy_stencil_to_color &&
	       a.copy_centroid == b.copy_centroid &&
	       a.copy_sample == b.copy_sample;
}

[[nodiscard]] static bool SameDepthControl(const HW::DepthControl& a,
                                           const HW::DepthControl& b) noexcept {
	return a.stencil_enable == b.stencil_enable &&
	       a.z_enable == b.z_enable &&
	       a.z_write_enable == b.z_write_enable &&
	       a.depth_bounds_enable == b.depth_bounds_enable &&
	       a.zfunc == b.zfunc &&
	       a.backface_enable == b.backface_enable &&
	       a.stencilfunc == b.stencilfunc &&
	       a.stencilfunc_bf == b.stencilfunc_bf;
}

[[nodiscard]] static bool SameStencilControl(const HW::StencilControl& a,
                                             const HW::StencilControl& b) noexcept {
	return a.stencil_fail == b.stencil_fail &&
	       a.stencil_zpass == b.stencil_zpass &&
	       a.stencil_zfail == b.stencil_zfail &&
	       a.stencil_fail_bf == b.stencil_fail_bf &&
	       a.stencil_zpass_bf == b.stencil_zpass_bf &&
	       a.stencil_zfail_bf == b.stencil_zfail_bf;
}

[[nodiscard]] static bool SameStencilMask(const HW::StencilMask& a,
                                          const HW::StencilMask& b) noexcept {
	return a.stencil_testval == b.stencil_testval &&
	       a.stencil_mask == b.stencil_mask &&
	       a.stencil_writemask == b.stencil_writemask &&
	       a.stencil_opval == b.stencil_opval &&
	       a.stencil_testval_bf == b.stencil_testval_bf &&
	       a.stencil_mask_bf == b.stencil_mask_bf &&
	       a.stencil_writemask_bf == b.stencil_writemask_bf &&
	       a.stencil_opval_bf == b.stencil_opval_bf;
}

bool RenderExecutor::PrepareDrawRenderState(CommandBuffer& buffer, const DrawCallInfo& draw,
                                            DrawRenderState& state) {
	m_context.GetImageHeap().BeginDraw();
	m_context.GetPipelineCache().GetGraphicsPrograms(buffer, state);
	// Vertex programs beyond the host mesh limits or with unsupported call targets are not
	// compiled.
	if (state.vertex_info[0].stage.module == nullptr) {
		return false;
	}

	auto&       texture_cache = m_context.GetTextureCache();
	const auto& hw            = buffer.GetRegisters();
	if (m_rt_cache.valid && buffer.IsRendering() && !graphics_debug_dump_enabled() &&
	    m_rt_cache.resolve_epoch == texture_cache.ResolveEpoch() &&
	    m_rt_cache.mrt_mask == state.mrt_mask &&
	    m_rt_cache.render_target_mask == hw.GetRenderTargetMask() &&
	    SameAaConfig(m_rt_cache.aa_config, hw.GetAaConfig()) &&
	    SameDepthTargetRegs(m_rt_cache.depth_rt, hw.GetDepthRenderTarget()) &&
	    SameRenderControl(m_rt_cache.render_control, hw.GetRenderControl()) &&
	    SameDepthControl(m_rt_cache.depth_control, hw.GetDepthControl()) &&
	    SameStencilControl(m_rt_cache.stencil_control, hw.GetStencilControl()) &&
	    SameStencilMask(m_rt_cache.stencil_mask, hw.GetStencilMask()) &&
	    std::bit_cast<uint32_t>(m_rt_cache.depth_clear_value) ==
	        std::bit_cast<uint32_t>(hw.GetDepthClearValue()) &&
	    m_rt_cache.stencil_clear_value == hw.GetStencilClearValue()) {
		bool rt_match = true;
		for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; ++slot) {
			if ((state.mrt_mask & (1u << slot)) != 0 &&
			    !SameColorTargetRegs(m_rt_cache.rt_regs[slot], hw.GetRenderTarget(slot))) {
				rt_match = false;
				break;
			}
		}
		if (rt_match) {
			bool images_valid = false;
			{
				std::scoped_lock lock {texture_cache.m_lock};
				if (m_rt_cache.resolve_epoch == texture_cache.ResolveEpoch()) {
					images_valid = true;
					for (uint32_t i = 0; i < m_rt_cache.color_count; ++i) {
						const auto* img =
						    texture_cache.m_slot_images.try_get(m_rt_cache.color_info[i].image_id);
						if (img == nullptr || !img->registered || img->depth_id ||
						    img->binding.needs_rebind) {
							images_valid = false;
							break;
						}
					}
					if (images_valid && m_rt_cache.depth_info.image_id) {
						const auto* img =
						    texture_cache.m_slot_images.try_get(m_rt_cache.depth_info.image_id);
						if (img == nullptr || !img->registered || img->depth_id ||
						    img->binding.needs_rebind) {
							images_valid = false;
						}
					}
				}
			}
			if (images_valid) {
				state.color_count = m_rt_cache.color_count;
				for (uint32_t i = 0; i < state.color_count; ++i) {
					state.color_info[i] = m_rt_cache.color_info[i];
					BindRenderTarget(state.color_info[i].image_id);
				}
				state.depth_info = m_rt_cache.depth_info;
				if (state.depth_info.image_id) {
					BindRenderTarget(state.depth_info.image_id);
				}
				if (state.color_count == 0 && !state.depth_info.image_id && !state.ps_active) {
					LogFramebufferSkip(draw.Name(), state.color_info[0], state.depth_info, buffer,
					                   draw.index_count, 0);
					return false;
				}
				return true;
			}
		}
	}

	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		if ((state.mrt_mask & (1u << slot)) != 0) {
			ResolveRenderColorTarget(buffer, state.color_info[state.color_count], slot);
			if (state.color_info[state.color_count].image_id) {
				state.color_count++;
			}
		}
	}
	ResolveRenderDepthTarget(buffer, state.depth_info);
	// TODO(PPSA09243): Vulkan passes take one sample count. Where PS5 attachments mix counts,
	// keep those matching the raster count instead of resolving multisampled shadows.
	const auto colors       = std::span {state.color_info, state.color_count};
	const auto pass_samples = state.depth_info.image_id ? state.depth_info.desc.info.samples
	                                                    : state.color_info[0].desc.info.samples;
	if (std::ranges::any_of(colors, [&](const RenderColorInfo& color) {
		    return color.desc.info.samples != pass_samples;
	    })) {
		const auto raster =
		    render_sample_count(buffer.GetRegisters().GetAaConfig().msaa_num_samples);
		const auto dropped = std::ranges::remove_if(colors, [&](const RenderColorInfo& color) {
			return color.desc.info.samples != raster;
		});
		state.color_count -= static_cast<uint32_t>(dropped.size());
		if (state.depth_info.image_id && state.depth_info.desc.info.samples != raster) {
			state.depth_info = {};
		}
		static std::atomic_bool logged = false;
		if (!logged.exchange(true, std::memory_order_relaxed)) {
			Log::WriteToConsoleAndLog(
			    "Warning: dropping attachments whose sample count differs from the raster.\n");
		}
	}

	if (state.color_count == 0 && !state.depth_info.image_id && !state.ps_active) {
		LogFramebufferSkip(draw.Name(), state.color_info[0], state.depth_info, buffer,
		                   draw.index_count, 0);
		return false;
	}

	if (!graphics_debug_dump_enabled() && (state.color_count != 0 || state.depth_info.image_id)) {
		m_rt_cache.valid               = true;
		m_rt_cache.resolve_epoch       = texture_cache.ResolveEpoch();
		m_rt_cache.mrt_mask            = state.mrt_mask;
		m_rt_cache.render_target_mask  = hw.GetRenderTargetMask();
		m_rt_cache.aa_config           = hw.GetAaConfig();
		for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; ++slot) {
			if ((state.mrt_mask & (1u << slot)) != 0) {
				m_rt_cache.rt_regs[slot] = hw.GetRenderTarget(slot);
			}
		}
		m_rt_cache.depth_rt            = hw.GetDepthRenderTarget();
		m_rt_cache.render_control      = hw.GetRenderControl();
		m_rt_cache.depth_control       = hw.GetDepthControl();
		m_rt_cache.stencil_control     = hw.GetStencilControl();
		m_rt_cache.stencil_mask        = hw.GetStencilMask();
		m_rt_cache.depth_clear_value   = hw.GetDepthClearValue();
		m_rt_cache.stencil_clear_value = hw.GetStencilClearValue();
		m_rt_cache.color_count         = state.color_count;
		for (uint32_t i = 0; i < state.color_count; ++i) {
			m_rt_cache.color_info[i] = state.color_info[i];
		}
		m_rt_cache.depth_info = state.depth_info;
	}

	return true;
}

static PreparedIndexBuffer PrepareIndexBuffer(CommandBuffer&               buffer,
                                              const DrawIndexBufferSource& source) {
	PreparedIndexBuffer prepared;
	if (source.size == 0) {
		return prepared;
	}
	prepared.type =
	    source.guest_element_size == 4 ? vk::IndexType::eUint32 : vk::IndexType::eUint16;
	if (source.host_data != nullptr) {
		auto& stream = buffer.GetContext().GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
		prepared.offset = stream.Copy(source.host_data, source.size, 16);
		prepared.buffer = stream.Handle();
	} else {
		auto [buffer_ptr, offset] =
		    buffer.GetContext().GetBufferCache().ObtainBuffer(source.address, source.size, false);
		prepared.buffer = buffer_ptr->Handle();
		prepared.offset = offset;
	}
	return prepared;
}

static void CommitVertexBuffers(vk::CommandBuffer            vk_buffer,
                                const PreparedVertexBuffers& prepared) {
	if (prepared.count != 0) {
		// Guest descriptor bounds must survive allocation merging in the cache.
		vk_buffer.bindVertexBuffers2(0, prepared.count, prepared.buffers.data(),
		                             prepared.offsets.data(), prepared.sizes.data(), nullptr);
	}
}

static void CommitIndexBuffer(vk::CommandBuffer vk_buffer, const PreparedIndexBuffer& prepared) {
	if (prepared.buffer == nullptr) {
		return;
	}
	vk_buffer.bindIndexBuffer(prepared.buffer, prepared.offset, prepared.type);
}

static void LogDrawStateIfNeeded(const CommandBuffer& buffer, const DrawCallInfo& draw,
	                             const DrawRenderState& state, uint32_t index_type_and_size,
                                 const void* index_addr) {
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	if (!draw.indexed && !Prospero::IsRectList(buffer.GetUserConfig().GetPrimType())) {
		return;
	}

	if (state.ps_active) {
		LogDrawTargetState(draw.Name(), state.color_info[0], state.depth_info, buffer,
		                   state.ps_input_info, draw.index_count, 0);
	}
	LogDrawInputState(buffer, state.color_info[0], state.vertex_info[0], index_type_and_size,
	                  draw.index_count, index_addr);
}

uint32_t RenderExecutor::MaxMdiCommands() noexcept {
	if (m_max_mdi_commands != 0) {
		return m_max_mdi_commands;
	}
	const auto&    graphics = m_context.GetGraphics();
	const uint32_t hw_limit = graphics.physical_device_properties.limits.maxDrawIndirectCount;
	if (hw_limit <= 1u) {
		m_max_mdi_commands = 1u;
		return m_max_mdi_commands;
	}
	uint64_t    device_local_bytes = 0;
	const auto& mem                = graphics.physical_device_memory_properties;
	for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
		if (mem.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
			device_local_bytes = std::max<uint64_t>(device_local_bytes, mem.memoryHeaps[i].size);
		}
	}
	constexpr uint64_t FourGiB   = 4ull * 1024 * 1024 * 1024;
	constexpr uint64_t EightGiB  = 8ull * 1024 * 1024 * 1024;
	const uint32_t     vram_cap  = device_local_bytes >= EightGiB ? 1024u
	                             : device_local_bytes >= FourGiB  ? 512u
	                                                              : 256u;
	m_max_mdi_commands = std::min({hw_limit, vram_cap, MdiBatcher::MaxBatchCommands});
	return m_max_mdi_commands;
}

void RenderExecutor::FlushMdi(CommandBuffer& buffer) {
	const std::vector<vk::DrawIndexedIndirectCommand>* indexed_cmds = nullptr;
	const std::vector<vk::DrawIndirectCommand>*        direct_cmds  = nullptr;
	bool                                               is_indexed   = false;
	if (!buffer.TakeMdiBatch(indexed_cmds, direct_cmds, is_indexed)) {
		return;
	}
	EXIT_IF(buffer.IsInvalid());
	auto vk_buffer = buffer.Handle();

	if (is_indexed) {
		const auto& scratch = *indexed_cmds;
		const auto  count   = static_cast<uint32_t>(scratch.size());
		if (count == 1) {
			const auto& c = scratch[0];
			vk_buffer.drawIndexed(c.indexCount, c.instanceCount, c.firstIndex, c.vertexOffset,
			                      c.firstInstance);
			return;
		}
		const uint64_t bytes  = static_cast<uint64_t>(count) * sizeof(vk::DrawIndexedIndirectCommand);
		auto&          stream = m_context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
		const auto [mapped, offset] = stream.Map(bytes, 4, /*allow_wait=*/false);
		if (mapped != nullptr) {
			std::memcpy(mapped, scratch.data(), static_cast<size_t>(bytes));
			stream.Commit();
			vk_buffer.drawIndexedIndirect(stream.Handle(), offset, count,
			                              sizeof(vk::DrawIndexedIndirectCommand));
			Common::DebugCounters::Add(Common::DebugCounters::Counter::IndirectDraws, count);
		} else {
			for (const auto& c: scratch) {
				vk_buffer.drawIndexed(c.indexCount, c.instanceCount, c.firstIndex, c.vertexOffset,
				                      c.firstInstance);
			}
		}
		return;
	}

	const auto& scratch = *direct_cmds;
	const auto  count   = static_cast<uint32_t>(scratch.size());
	if (count == 1) {
		const auto& c = scratch[0];
		vk_buffer.draw(c.vertexCount, c.instanceCount, c.firstVertex, c.firstInstance);
		return;
	}
	const uint64_t bytes        = static_cast<uint64_t>(count) * sizeof(vk::DrawIndirectCommand);
	auto&          stream       = m_context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
	const auto [mapped, offset] = stream.Map(bytes, 4, /*allow_wait=*/false);
	if (mapped != nullptr) {
		std::memcpy(mapped, scratch.data(), static_cast<size_t>(bytes));
		stream.Commit();
		vk_buffer.drawIndirect(stream.Handle(), offset, count, sizeof(vk::DrawIndirectCommand));
		Common::DebugCounters::Add(Common::DebugCounters::Counter::IndirectDraws, count);
	} else {
		for (const auto& c: scratch) {
			vk_buffer.draw(c.vertexCount, c.instanceCount, c.firstVertex, c.firstInstance);
		}
	}
}

static void EmitDrawPrimitives(const HW::UserConfig& ucfg, vk::CommandBuffer vk_buffer,
                               const DrawCallInfo& draw, const DrawEmitInfo& emit,
                               uint32_t first_index = 0) {
	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kPointList:
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip:
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kPolygon:
		case Prospero::PrimitiveType::kRectListLegacy:
		case Prospero::PrimitiveType::kPatch:
			if (draw.indexed) {
				vk_buffer.drawIndexed(draw.index_count, draw.instance_count, first_index,
				                      emit.vertex_offset, emit.first_instance);
			} else {
				vk_buffer.draw(draw.index_count, draw.instance_count,
				               static_cast<uint32_t>(emit.vertex_offset), emit.first_instance);
			}
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			EXIT_NOT_IMPLEMENTED((draw.index_count & 0x3u) != 0);
			for (uint32_t i = 0; i < draw.index_count; i += 4) {
				if (draw.indexed) {
					vk_buffer.drawIndexed(4, draw.instance_count, i, emit.vertex_offset,
					                      emit.first_instance);
				} else {
					vk_buffer.draw(4, draw.instance_count,
					               i + static_cast<uint32_t>(emit.vertex_offset),
					               emit.first_instance);
				}
			}
			break;
		default: EXIT("unknown primitive type: %u\n", static_cast<uint32_t>(ucfg.GetPrimType()));
	}
}

bool RenderExecutor::CopyDepthToColor(CommandBuffer& buffer, const DrawCallInfo& draw,
                                       const DrawEmitInfo& emit, DrawRenderState& state) {
	if (!buffer.GetRegisters().GetRenderControl().copy_depth_to_color) {
		return false;
	}
	FlushMdi(buffer);
	InvalidateRenderTargetCache();
	auto& ucfg = buffer.GetUserConfig();
	const auto& hw = buffer.GetRegisters();
	auto& depth = state.depth_info;
	auto& color = state.color_info[0];
	EXIT_NOT_IMPLEMENTED(
	    !m_context.GetGraphics().maintenance8_enabled ||
	    !Prospero::IsRectList(ucfg.GetPrimType()) || draw.indexed || draw.index_count != 3 ||
	    draw.instance_count != 1 || emit.vertex_offset != 0 || emit.first_instance != 0 ||
	    state.color_count != 1 || color.target_slot != 0 || !depth.image_id ||
	    color.desc.info.samples != 1 || depth.desc.info.extent.width != color.Extent().width ||
	    depth.desc.info.extent.height != color.Extent().height ||
	    depth.desc.view_info.layer_count != 1 || color.desc.view_info.layer_count != 1 ||
	    hw.GetBlendControl(0).enable || state.ps_input_info.ps_pixel_kill_enable ||
	    state.ps_input_info.ps_depth_export_enable ||
	    state.ps_input_info.ps_sample_mask_export_enable);
	vk::ImageAspectFlags feedback;
	const auto rendering = AcquireRenderTargets(buffer, &color, 1, depth, feedback);
	if (depth.load_clear_aspects) {
		buffer.BeginRendering(rendering);
	}
	auto& cache = m_context.GetTextureCache();
	auto& source = cache.GetImage(depth.image_id);
	auto& destination = cache.GetImage(color.image_id);
	EXIT_NOT_IMPLEMENTED((source.backing.format != vk::Format::eD32Sfloat &&
	                      source.backing.format != vk::Format::eD32SfloatS8Uint) ||
	                     destination.backing.format != vk::Format::eR32Sfloat);
	const auto rect = calc_final_scissor(hw.GetScreenViewport(), hw.GetScanModeControl(),
	                                    {rendering.width, rendering.height}, 0);
	if (rect.right > rect.left && rect.bottom > rect.top) {
		const vk::ImageCopy region {
		    {vk::ImageAspectFlagBits::eDepth, depth.desc.view_info.base_level,
		     depth.desc.view_info.base_layer, 1}, {rect.left, rect.top, 0},
		    {vk::ImageAspectFlagBits::eColor, color.desc.view_info.base_level,
		     color.desc.view_info.base_layer, 1}, {rect.left, rect.top, 0},
		    {static_cast<uint32_t>(rect.right - rect.left),
		     static_cast<uint32_t>(rect.bottom - rect.top), 1}};
		buffer.EndRendering();
		const auto command = buffer.Handle();
		source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
		               {}, command);
		destination.Transit(vk::ImageLayout::eTransferDstOptimal,
		                    vk::AccessFlagBits2::eTransferWrite, {}, command);
		command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal,
		                  destination.backing.image, vk::ImageLayout::eTransferDstOptimal,
		                  1, &region);
	}
	return true;
}

void RenderExecutor::ExecutePreparedDraw(CommandBuffer& buffer, const DrawCallInfo& draw,
                                         DrawRenderState& state, const DrawEmitInfo& emit,
                                         const DrawIndexBufferSource& index_source) {
	auto&      ucfg                     = buffer.GetUserConfig();
	const bool primitive_restart_enable = state.primitive_restart_enable;
	if (CopyDepthToColor(buffer, draw, emit, state)) {
		return;
	}
	const auto  vertex_stages = state.VertexStages();
	const bool  mesh_active   = state.vertex_info[0].stage.module->info.stage == ShaderType::Mesh;
	const bool  mesh_task     = state.vertex_info[0].stage.module->mesh_task;
	const auto& mesh          = state.vertex_info[0].mesh;
	const auto& limits        = m_context.GetGraphics().mesh_shader_properties;
	uint32_t    mesh_groups   = 0;
	if (mesh_active) {
		EXIT_NOT_IMPLEMENTED(mesh.fast_launch && (draw.indexed || primitive_restart_enable));
		static std::atomic_bool restart_warned = false;
		if (primitive_restart_enable && !restart_warned.exchange(true, std::memory_order_relaxed)) {
			std::printf("Warning: primitive restart is not implemented for mesh shaders; "
			            "continuing draw (primitive=%u indexed=%u)\n",
			            static_cast<uint32_t>(ucfg.GetPrimType()), draw.indexed);
		}
		if (mesh.primitives_per_group == 0) {
			EXIT("unsupported mesh draw: primitive=%u indexed=%u restart=%u\n",
			     static_cast<uint32_t>(ucfg.GetPrimType()), draw.indexed, primitive_restart_enable);
		}
	}
	if (mesh_task) {
		EXIT_NOT_IMPLEMENTED(emit.indirect.count > std::min(limits.maxTaskWorkGroupCount[0],
		                                                    limits.maxTaskWorkGroupTotalCount));
	} else if (mesh_active) {
		const auto primitives = mesh.InputPrimitiveCount(draw.index_count);
		if (primitives == 0 || draw.instance_count == 0) {
			return;
		}
		mesh_groups = (primitives - 1u) / mesh.primitives_per_group + 1u;
		if (mesh_groups > limits.maxMeshWorkGroupCount[0] ||
		    draw.instance_count > limits.maxMeshWorkGroupCount[1] ||
		    static_cast<uint64_t>(mesh_groups) * draw.instance_count >
		        limits.maxMeshWorkGroupTotalCount) {
			EXIT("mesh draw exceeds host workgroup limits: %ux%u\n", mesh_groups,
			     draw.instance_count);
		}
	}

	if (mesh_active && draw.indexed) {
		// Register the original guest indices for shader reads; PrepareGraphicsBindings
		// synchronizes registered BDA ranges before any draw commands are committed.
		(void)m_context.GetBufferCache().FindBuffer(
		    index_source.address, static_cast<uint64_t>(draw.index_count) *
		                              index_source.guest_element_size);
	}
	auto&                            bindings = m_graphics_bindings;
	std::array<PreparedBindings*, 4> descriptor_stages {};
	uint32_t                         stage_count = 0;
	for (uint32_t i = 0; i < vertex_stages.size(); i++) {
		PrepareBindings(state.vertex_info[i].stage, bindings.vertex[i]);
		descriptor_stages[stage_count++] = &bindings.vertex[i];
	}
	if (state.ps_active) {
		PrepareBindings(state.ps_input_info.stage, bindings.pixel);
		descriptor_stages[stage_count++] = &bindings.pixel;
	}
	const auto stages = std::span {descriptor_stages.data(), stage_count};
	PrepareGraphicsBindings(stages, std::span {state.color_info, state.color_count},
	                        /*rebind_buffers=*/false);
	PreparedVertexBuffers vertex_bindings;
	PreparedIndexBuffer   index_binding;
	if (!mesh_active) {
		vertex_bindings = AcquireVertexBuffers(buffer, state.vertex_info[0]);
		if (index_source.host_data == nullptr) {
			index_binding = PrepareIndexBuffer(buffer, index_source);
		}
	}
	std::pair<Buffer*, uint64_t> args_buffer {};
	if (emit.indirect.address != 0) {
		const auto size = static_cast<uint64_t>(emit.indirect.count - 1) * emit.indirect.stride +
		                  (draw.indexed ? sizeof(vk::DrawIndexedIndirectCommand)
		                                : sizeof(vk::DrawIndirectCommand));
		args_buffer = m_context.GetBufferCache().ObtainBuffer(emit.indirect.address, size, false);
		EXIT_IF((args_buffer.second & 3u) != 0);
	}
	auto&             pipeline    = m_context.GetPipelineCache().GetGraphicsPipeline(buffer, state);
	vk::DeviceAddress task_params = 0;
	for (;;) {
		const uint64_t prep_tick = m_context.GetCommandScheduler().CurrentTick();
		if (!mesh_active && index_source.host_data != nullptr) {
			index_binding = PrepareIndexBuffer(buffer, index_source);
		}
		if (pipeline.uses_image_heap) {
			m_context.GetImageHeap().UploadDirtySlots(buffer);
		}
		for (auto* stage: stages) {
			RebindBuffers(*stage);
		}
		if (mesh_task) {
			const MeshTaskParams params {
			    .records        = args_buffer.first->BufferDeviceAddress() + args_buffer.second,
			    .index_address  = index_source.address,
			    .stride         = emit.indirect.stride,
			    .index_size     = index_source.guest_element_size,
			    .index_limit    = draw.index_count,
			    .primitive_size = mesh.InputPrimitiveSize(),
			    .group_step     = mesh.primitives_per_group * mesh.InputPrimitiveStep(),
			    .max_groups     = limits.maxMeshWorkGroupCount[0],
			    .max_instances  = limits.maxMeshWorkGroupCount[1],
			    .max_total      = limits.maxMeshWorkGroupTotalCount,
			};
			auto& stream = m_context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
			task_params  = stream.BufferDeviceAddress() + stream.Copy(&params, sizeof(params), 8);
		}
		if (m_context.GetCommandScheduler().CurrentTick() == prep_tick) {
			break;
		}
	}
	vk::ImageAspectFlags feedback_aspects;
	const auto rendering =
	    AcquireRenderTargets(buffer, state.color_info, state.color_count, state.depth_info,
	                         feedback_aspects, stages);

	// Normalize submesh index byte offsets into firstIndex so submeshes in the same VkBuffer
	// share a single index-buffer binding at offset 0 and coalesce into Multi-Draw Indirect.
	uint32_t first_index = 0;
	const auto prim_type = ucfg.GetPrimType();
	if (!mesh_active && draw.indexed && args_buffer.first == nullptr &&
	    prim_type != Prospero::PrimitiveType::kQuadListLegacy &&
	    index_binding.buffer != nullptr) {
		const uint32_t elem_size = index_binding.type == vk::IndexType::eUint32 ? 4u : 2u;
		if ((index_binding.offset % elem_size) == 0) {
			const uint64_t elem_offset = index_binding.offset / elem_size;
			if (elem_offset <= UINT32_MAX - draw.index_count) {
				first_index          = static_cast<uint32_t>(elem_offset);
				index_binding.offset = 0;
			}
		}
	}

	vk::PipelineStageFlags shader_write_stages = {};
	for (const auto& stage: vertex_stages) {
		if (HasShaderBufferWrites(stage.stage)) {
			shader_write_stages |= ShaderPipelineStages(NativeShaderStage(stage.logical_stage));
		}
	}
	if (state.ps_active && HasShaderBufferWrites(state.ps_input_info.stage)) {
		shader_write_stages |= vk::PipelineStageFlagBits::eFragmentShader;
	}

	auto& dynamic_state = m_context.GetCommandScheduler().GetDynamicState();
	UpdateGraphicsDynamicState(buffer, dynamic_state, vertex_stages.back(), state.depth_info,
	                           rendering);
	dynamic_state.SetAttachmentFeedbackLoopAspects(feedback_aspects);
	const bool dynamic_state_dirty = dynamic_state.HasPendingCommit(m_context.GetGraphics());

	auto&          occlusion_queries = m_context.GetOcclusionQueries();
	const uint32_t max_mdi_commands  = MaxMdiCommands();
	const bool     mdi_eligible_shape =
	    max_mdi_commands > 1u && !mesh_active && !mesh_task && args_buffer.first == nullptr &&
	    prim_type != Prospero::PrimitiveType::kQuadListLegacy && !shader_write_stages &&
	    !occlusion_queries.IsOpen() && !graphics_debug_dump_enabled();

	using ShaderRecompiler::IR::PushData;
	const uint32_t draw_dword_count =
	    PushData::GraphicsDrawDwords(mesh_active, state.ps_input_info.object_primitive_id);
	std::array<uint32_t, PushData::MeshDrawDwordCount> draw_data {};
	if (draw_dword_count != 0) {
		if (mesh_task) {
			draw_data[0] = static_cast<uint32_t>(task_params);
			draw_data[1] = static_cast<uint32_t>(task_params >> 32u);
		} else if (mesh_active) {
			draw_data = {draw.index_count,
			             static_cast<uint32_t>(emit.vertex_offset),
			             index_source.guest_element_size,
			             static_cast<uint32_t>(index_source.address),
			             static_cast<uint32_t>(index_source.address >> 32u)};
		} else {
			draw_data[0] = ucfg.GetObjectId();
		}
	}
	const uint32_t user_clip_planes = vertex_stages.back().user_clip_planes;
	const uint32_t object_id        =
	    state.ps_input_info.object_primitive_id ? ucfg.GetObjectId() : 0u;

	MdiHash128 precomputed_hash {};
	bool       precomputed_eligible = false;
	auto&      gfx_state            = buffer.GraphicsState();
	auto&      mdi_batcher          = buffer.GetMdiBatcher();
	if (mdi_eligible_shape && !dynamic_state_dirty && buffer.IsRenderingWith(rendering) &&
	    (!pipeline.uses_image_heap || !m_context.GetImageHeap().NeedsCommit()) &&
	    mdi_batcher.MatchesScalars(
	        static_cast<VkPipeline>(pipeline.pipeline),
	        draw.indexed ? static_cast<VkBuffer>(index_binding.buffer) : nullptr,
	        draw.indexed ? index_binding.offset : 0u,
	        draw.indexed ? static_cast<VkIndexType>(index_binding.type) : VK_INDEX_TYPE_UINT16,
	        static_cast<uint32_t>(prim_type), object_id, draw.indexed, max_mdi_commands)) {
		const MdiHash128 vb_hash =
		    gfx_state.MatchesActiveVertexBuffers(vertex_bindings.count, vertex_bindings.buffers,
		                                         vertex_bindings.offsets, vertex_bindings.sizes)
		        ? gfx_state.vertex_buffer_hash
		        : ComputeVertexBufferHash(vertex_bindings.count, vertex_bindings.buffers,
		                                  vertex_bindings.offsets, vertex_bindings.sizes);
		if (vb_hash == mdi_batcher.CurrentKey().vertex_buffer_hash &&
		    CanFastAppendMdiBindings(buffer, pipeline, stages, draw_dword_count,
		                             std::span<const uint32_t> {draw_data.data(), draw_dword_count},
		                             user_clip_planes, mdi_batcher.CurrentKey().bindings_hash,
		                             &precomputed_hash, &precomputed_eligible)) {
			if (pipeline.uses_image_heap) {
				m_context.GetImageHeap().ClearActive();
			}
			if (draw.indexed) {
				mdi_batcher.AppendIndexed({
				    .indexCount    = draw.index_count,
				    .instanceCount = draw.instance_count,
				    .firstIndex    = first_index,
				    .vertexOffset  = emit.vertex_offset,
				    .firstInstance = emit.first_instance,
				});
			} else {
				mdi_batcher.AppendDirect({
				    .vertexCount   = draw.index_count,
				    .instanceCount = draw.instance_count,
				    .firstVertex   = static_cast<uint32_t>(emit.vertex_offset),
				    .firstInstance = emit.first_instance,
				});
			}
			Common::DebugCounters::Add(Common::DebugCounters::Counter::Draws);
			Common::DebugCounters::Add(Common::DebugCounters::Counter::DrawInstances,
			                           draw.instance_count);
			return;
		}
	}

	// Flush any pending MDI batch before mutating bound buffers, descriptors, push constants,
	// or dynamic state for the incoming draw.
	FlushMdi(buffer);

	// End any incompatible or barriered render pass before CommitBindings records push-constant
	// state, since EndRendering() invalidates push-constant tracking for compute helpers.
	if (!buffer.IsRenderingWith(rendering) || shader_write_stages) {
		buffer.EndRendering();
	}

	// Resource preparation above may synchronously finish and restart the scheduler. From this
	// point onward, every operation targets the current command buffer and cannot touch guest
	// memory.
	auto vk_buffer = buffer.Handle();
	if (!mesh_active) {
		if (vertex_bindings.count != 0) {
			if (gfx_state.ShouldBindVertexBuffers(vertex_bindings.count, vertex_bindings.buffers,
			                                      vertex_bindings.offsets, vertex_bindings.sizes)) {
				CommitVertexBuffers(vk_buffer, vertex_bindings);
			}
		} else if (gfx_state.active_vertex_count != 0) {
			gfx_state.active_vertex_count = 0;
			gfx_state.vertex_buffer_hash  = ComputeVertexBufferHash(0, {}, {}, {});
		}
	}
	GraphicsCommitExtra gfx_extra {
	    .draw_dword_count              = draw_dword_count,
	    .draw_dwords                   = draw_data,
	    .user_clip_planes              = user_clip_planes,
	    .compute_mdi_info              = mdi_eligible_shape,
	    .has_precomputed_mdi_hash      = precomputed_eligible,
	    .precomputed_mdi_bindings_hash = precomputed_hash,
	};
	CommitBindings(buffer, vk::PipelineBindPoint::eGraphics, pipeline, stages, &gfx_extra);
	if (!mesh_active && index_binding.buffer != nullptr &&
	    gfx_state.ShouldBindIndexBuffer(index_binding.buffer, index_binding.offset,
	                                    index_binding.type)) {
		CommitIndexBuffer(vk_buffer, index_binding);
	}

	dynamic_state.Commit(m_context.GetGraphics(), vk_buffer);

	if (shader_write_stages) {
		// As for dispatches, buffer writes wait for earlier accesses to the memory.
		ShaderWriteHazardBarrier(vk_buffer, shader_write_stages);
	}
	buffer.BeginRendering(rendering);
	if (gfx_state.ShouldBindPipeline(pipeline.pipeline)) {
		vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
	}

	if (mdi_eligible_shape && gfx_extra.out_can_begin_mdi) {
		const MdiBatchKey batch_key {
		    .pipeline     = static_cast<VkPipeline>(pipeline.pipeline),
		    .index_buffer = draw.indexed ? static_cast<VkBuffer>(index_binding.buffer) : nullptr,
		    .index_offset = draw.indexed ? index_binding.offset : 0u,
		    .index_type   = draw.indexed ? static_cast<VkIndexType>(index_binding.type)
		                                 : VK_INDEX_TYPE_UINT16,
		    .topology           = static_cast<uint32_t>(prim_type),
		    .object_id          = object_id,
		    .vertex_buffer_hash = gfx_state.vertex_buffer_hash,
		    .bindings_hash      = gfx_extra.out_mdi_bindings_hash,
		};
		mdi_batcher.BeginBatch(batch_key, draw.indexed);
		if (draw.indexed) {
			mdi_batcher.AppendIndexed({
			    .indexCount    = draw.index_count,
			    .instanceCount = draw.instance_count,
			    .firstIndex    = first_index,
			    .vertexOffset  = emit.vertex_offset,
			    .firstInstance = emit.first_instance,
			});
		} else {
			mdi_batcher.AppendDirect({
			    .vertexCount   = draw.index_count,
			    .instanceCount = draw.instance_count,
			    .firstVertex   = static_cast<uint32_t>(emit.vertex_offset),
			    .firstInstance = emit.first_instance,
			});
		}
		Common::DebugCounters::Add(Common::DebugCounters::Counter::Draws);
		Common::DebugCounters::Add(Common::DebugCounters::Counter::DrawInstances,
		                           draw.instance_count);
		return;
	}

	const int32_t occlusion_query = occlusion_queries.Begin(vk_buffer);
	if (mesh_task) {
		vk_buffer.drawMeshTasksEXT(emit.indirect.count, 1, 1);
	} else if (mesh_active) {
		vk_buffer.drawMeshTasksEXT(mesh_groups, draw.instance_count, 1);
	} else if (args_buffer.first == nullptr) {
		EmitDrawPrimitives(ucfg, vk_buffer, draw, emit, first_index);
	} else if (draw.indexed) {
		vk_buffer.drawIndexedIndirect(args_buffer.first->Handle(), args_buffer.second,
		                              emit.indirect.count, emit.indirect.stride);
	} else {
		vk_buffer.drawIndirect(args_buffer.first->Handle(), args_buffer.second,
		                       emit.indirect.count, emit.indirect.stride);
	}
	occlusion_queries.End(vk_buffer, occlusion_query);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::Draws);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::DrawInstances, draw.instance_count);

	if (shader_write_stages) {
		buffer.EndRendering();
		ShaderWriteBarrier(vk_buffer, shader_write_stages);
	}
}

// Vulkan offsets for a prepared draw, or nullopt when its indirect records need the CPU:
// quad-list draws for counts, tessellation draws for the index/SGPR prolog and mesh draws without
// room for the task payload. Indirect records carry their own offsets.
static std::optional<DrawEmitInfo> ResolveDrawEmit(const HW::UserConfig&        ucfg,
                                                   const ShaderVertexInputInfo& vs,
                                                   DrawOffsetSource             source,
                                                   int32_t                      first_vertex,
                                                   uint32_t                     first_instance,
                                                   const DrawIndirectArgs&      indirect) {
	if ((indirect.address != 0 || source == DrawOffsetSource::IndirectArgs) &&
	    ucfg.GetIndexOffset() != 0) {
		static std::once_flag warning_once;
		std::call_once(warning_once, [&] {
			Log::WriteToConsoleAndLog(fmt::format(
			    "GPU: indirect draw ignores GE_INDX_OFFSET=0x{:08x}; geometry may be incorrect.\n",
			    ucfg.GetIndexOffset()));
		});
	}
	if (indirect.address != 0) {
		if (!vs.indirect_draw ||
		    ucfg.GetPrimType() == Prospero::PrimitiveType::kQuadListLegacy ||
		    vs.indirect_offsets != indirect.offsets) {
			return std::nullopt;
		}
		return DrawEmitInfo {.indirect = indirect};
	}
	if (source == DrawOffsetSource::IndirectArgs) {
		// Native fetches consume the patched SGPRs; rewritten fetches need Vulkan offsets.
		return vs.has_dynamic_vertex_fetch ? DrawEmitInfo {first_vertex, first_instance} : DrawEmitInfo {};
	}
	const auto [vertex_offset, instance_offset] = ResolveDrawOffsets(ucfg.GetIndexOffset(), vs);
	return DrawEmitInfo {vertex_offset + first_vertex, instance_offset};
}

bool RenderExecutor::DrawIndex(CommandBuffer& buffer, const DrawIndexArgs& args) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	Common::LockGuard lock(m_context.GetMutex());
	if (args.index_count == 0 || (args.instance_count == 0 && args.indirect.address == 0)) {
		return true;
	}

	if (ConsumeMetadataColorOperation(buffer) || DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer)) {
		InvalidateRenderTargetCache();
		ResetBindings();
		return true;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return true;
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndex():Shader:\n");
		uc_print("GraphicsRenderDrawIndex():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndex():Parameters:\n"
		     "\t index_type_and_size = 0x%08" PRIx32 "\n"
		     "\t index_count         = 0x%08" PRIx32 "\n"
		     "\t index_addr          = 0x%016" PRIx64 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t base_vertex         = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.index_type_and_size, args.index_count,
		     reinterpret_cast<uint64_t>(args.index_addr), args.instance_count,
		     static_cast<uint32_t>(args.base_vertex), args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	DrawRenderState state {.indirect_offsets = args.indirect.offsets,
	                       .indirect_draw    = args.indirect.address != 0};
	if (!GetDrawTopology(ucfg, state.topology)) {
		return true;
	}

	DrawIndexBufferSource index_source {};
	index_source.address = reinterpret_cast<uint64_t>(args.index_addr);
	switch (static_cast<Prospero::IndexType>(args.index_type_and_size)) {
		case Prospero::IndexType::kIndex16: index_source.guest_element_size = 2; break;
		case Prospero::IndexType::kIndex32: index_source.guest_element_size = 4; break;
		case Prospero::IndexType::kIndex8: index_source.guest_element_size = 1; break;
		default: EXIT("unknown index_type_and_size: %u\n", args.index_type_and_size);
	}
	index_source.size = static_cast<uint64_t>(args.index_count) * index_source.guest_element_size;
	state.primitive_restart_enable = ResolvePrimitiveRestart(buffer, index_source);

	std::vector<uint16_t> expanded_indices;
	if (index_source.guest_element_size == 1) {
		EXIT_NOT_IMPLEMENTED(args.index_addr == nullptr);
		const auto* src = static_cast<const uint8_t*>(args.index_addr);
		expanded_indices.resize(args.index_count);
		for (uint32_t i = 0; i < args.index_count; i++) {
			expanded_indices[i] =
			    state.primitive_restart_enable && src[i] == 0xffu ? 0xffffu : src[i];
		}
		index_source.host_data = expanded_indices.data();
		index_source.size      = expanded_indices.size() * sizeof(uint16_t);
	}

	const DrawCallInfo draw {true, args.index_count, args.instance_count};
	if (!PrepareDrawRenderState(buffer, draw, state)) {
		ResetBindings();
		return true;
	}

	LogDrawStateIfNeeded(buffer, draw, state, args.index_type_and_size,
	                     args.index_addr);
	const auto emit = ResolveDrawEmit(ucfg, state.vertex_info[0], args.offset_source,
	                                  args.base_vertex, args.first_instance, args.indirect);
	if (emit) {
		ExecutePreparedDraw(buffer, draw, state, *emit, index_source);
	}
	ResetBindings();
	return emit.has_value();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool RenderExecutor::DrawAuto(CommandBuffer& buffer, const DrawAutoArgs& args) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	Common::LockGuard lock(m_context.GetMutex());
	if (args.indirect.address == 0 && (args.vertex_count == 0 || args.instance_count == 0)) {
		return true;
	}

	if (ConsumeMetadataColorOperation(buffer) || DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer)) {
		InvalidateRenderTargetCache();
		ResetBindings();
		return true;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return true;
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndexAuto():Shader:\n");
		uc_print("GraphicsRenderDrawIndexAuto():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndexAuto():Parameters:\n"
		     "\t vertex_count        = 0x%08" PRIx32 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t first_vertex        = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.vertex_count, args.instance_count, args.first_vertex, args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	const DrawCallInfo draw {false, args.vertex_count, args.instance_count};

	DrawRenderState state {.indirect_offsets = args.indirect.offsets,
	                       .indirect_draw    = args.indirect.address != 0};
	if (!GetDrawTopology(ucfg, state.topology)) {
		ResetBindings();
		return true;
	}
	if (!PrepareDrawRenderState(buffer, draw, state)) {
		ResetBindings();
		return true;
	}

	const bool rect_list = Prospero::IsRectList(ucfg.GetPrimType());
	if (rect_list && state.vertex_info[0].buffers_num == 0 &&
	    state.vertex_info[0].stage.module->info.param_export_mask == 0 &&
	    state.ps_input_info.input_num != 0) {
		if (graphics_debug_dump_enabled()) {
			LOGF("DrawIndexAuto: skipping rect-list draw with no VS param exports and PS inputs: "
			     "ps_inputs=%u ps=0x%016" PRIx64 " es=0x%016" PRIx64 " gs=0x%016" PRIx64 "\n",
			     state.ps_input_info.input_num, sh_ctx.GetPs().ps_regs.data_addr,
			     sh_ctx.GetVs().es_regs.data_addr, sh_ctx.GetVs().gs_regs.data_addr);
		}
		ResetBindings();
		return true;
	}

	LogDrawStateIfNeeded(buffer, draw, state, 0, nullptr);
	const auto emit = ResolveDrawEmit(ucfg, state.vertex_info[0], args.offset_source,
	                                  static_cast<int32_t>(args.first_vertex), args.first_instance,
	                                  args.indirect);
	if (emit) {
		ExecutePreparedDraw(buffer, draw, state, *emit, {});
	}
	ResetBindings();
	return emit.has_value();
}

bool RenderExecutor::ResolveColorTargets(CommandBuffer& buffer) {
	const auto& hw = buffer.GetRegisters();
	if (hw.GetColorControl().mode != 3) {
		return false;
	}

	const auto& src_rt = hw.GetRenderTarget(0);
	const auto& dst_rt = hw.GetRenderTarget(1);
	if (src_rt.base.addr == 0 || dst_rt.base.addr == 0) {
		return false;
	}
	FlushMdi(buffer);
	InvalidateRenderTargetCache();

	RenderColorInfo src {};
	RenderColorInfo dst {};
	ResolveRenderColorTarget(buffer, src, 0, true, true);
	ResolveRenderColorTarget(buffer, dst, 1, true, true);
	if (src.desc.info.data.address == dst.desc.info.data.address &&
	    src.guest_mip_level == dst.guest_mip_level &&
	    src.guest_array_layer == dst.guest_array_layer) {
		return true;
	}

	auto& cache = m_context.GetTextureCache();
	cache.MarkGpuWritten(dst.image_id);
	auto& source      = cache.GetImage(src.image_id);
	auto& destination = cache.GetImage(dst.image_id);
	destination.Resolve(source,
	                    {src.desc.view_info.base_level, 1, src.desc.view_info.base_layer, 1},
	                    {dst.desc.view_info.base_level, 1, dst.desc.view_info.base_layer, 1});
	return true;
}

} // namespace Libs::Graphics
