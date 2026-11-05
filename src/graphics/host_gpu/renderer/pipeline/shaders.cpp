#include "common/assert.h"
#include "common/common.h"
#include "common/profiler.h"
#include "gpu_blit_shaders/gpu_mesh_indirect_spv.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/rectListShader.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <vector>

namespace Libs::Graphics {

static vk::BlendFactor GetBlendFactor(uint32_t factor) {
	switch (static_cast<Prospero::BlendFactor>(factor)) {
		case Prospero::BlendFactor::kZero: return vk::BlendFactor::eZero;
		case Prospero::BlendFactor::kOne: return vk::BlendFactor::eOne;
		case Prospero::BlendFactor::kSrcColor: return vk::BlendFactor::eSrcColor;
		case Prospero::BlendFactor::kOneMinusSrcColor: return vk::BlendFactor::eOneMinusSrcColor;
		case Prospero::BlendFactor::kSrcAlpha: return vk::BlendFactor::eSrcAlpha;
		case Prospero::BlendFactor::kOneMinusSrcAlpha: return vk::BlendFactor::eOneMinusSrcAlpha;
		case Prospero::BlendFactor::kDstAlpha: return vk::BlendFactor::eDstAlpha;
		case Prospero::BlendFactor::kOneMinusDstAlpha: return vk::BlendFactor::eOneMinusDstAlpha;
		case Prospero::BlendFactor::kDstColor: return vk::BlendFactor::eDstColor;
		case Prospero::BlendFactor::kOneMinusDstColor: return vk::BlendFactor::eOneMinusDstColor;
		case Prospero::BlendFactor::kSrcAlphaSaturate: return vk::BlendFactor::eSrcAlphaSaturate;
		case Prospero::BlendFactor::kConstantColor: return vk::BlendFactor::eConstantColor;
		case Prospero::BlendFactor::kOneMinusConstantColor:
			return vk::BlendFactor::eOneMinusConstantColor;
		case Prospero::BlendFactor::kSrc1Color: return vk::BlendFactor::eSrc1Color;
		case Prospero::BlendFactor::kOneMinusSrc1Color: return vk::BlendFactor::eOneMinusSrc1Color;
		case Prospero::BlendFactor::kSrc1Alpha: return vk::BlendFactor::eSrc1Alpha;
		case Prospero::BlendFactor::kOneMinusSrc1Alpha: return vk::BlendFactor::eOneMinusSrc1Alpha;
		case Prospero::BlendFactor::kConstantAlpha: return vk::BlendFactor::eConstantAlpha;
		case Prospero::BlendFactor::kOneMinusConstantAlpha:
			return vk::BlendFactor::eOneMinusConstantAlpha;
		default: EXIT("unknown factor: %u\n", factor);
	}
	return vk::BlendFactor::eZero;
}

static vk::BlendOp GetBlendOp(uint32_t op) {
	switch (static_cast<Prospero::BlendOp>(op)) {
		case Prospero::BlendOp::kAdd: return vk::BlendOp::eAdd;
		case Prospero::BlendOp::kSubtract: return vk::BlendOp::eSubtract;
		case Prospero::BlendOp::kMin: return vk::BlendOp::eMin;
		case Prospero::BlendOp::kMax: return vk::BlendOp::eMax;
		case Prospero::BlendOp::kReverseSubtract: return vk::BlendOp::eReverseSubtract;
		default: EXIT("unknown op: %u\n", op);
	}
	return vk::BlendOp::eAdd;
}

static void AddLayoutBindings(std::vector<vk::DescriptorSetLayoutBinding>& descriptor_bindings,
                              const ShaderRecompiler::IR::CompiledShaderInfo& program,
                              vk::ShaderStageFlagBits              stage) {
	for (auto mask = program.bindings.descriptor_mask; mask != 0; mask &= mask - 1) {
		const auto index = std::countr_zero(mask);
		const auto kind = static_cast<ShaderRecompiler::IR::DescriptorBindingKind>(index);
		descriptor_bindings.push_back(
		    {ShaderRecompiler::IR::NativeBinding(program.stage, kind), NativeDescriptorType(kind),
		     program.bindings.descriptor_counts[index], stage, nullptr});
	}
}

// Creates the descriptor set layout and the pipeline layout that uses it.
static void CreateLayouts(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                          std::span<const vk::DescriptorSetLayoutBinding> bindings) {
	const auto& limits = graphics.GetPhysicalDeviceProperties().limits;
	uint64_t descriptor_count = 0;
	uint64_t storage_buffers = 0;
	uint64_t samplers = 0;
	std::array<uint64_t, 32> stage_buffers {}, stage_resources {}, stage_samplers {};
	for (const auto& binding: bindings) {
		descriptor_count += binding.descriptorCount;
		const bool storage = binding.descriptorType == vk::DescriptorType::eStorageBuffer;
		const bool sampler = binding.descriptorType == vk::DescriptorType::eSampler;
		if (storage) storage_buffers += binding.descriptorCount;
		if (sampler) samplers += binding.descriptorCount;
		for (auto stages = static_cast<uint32_t>(binding.stageFlags); stages != 0; stages &= stages - 1u) {
			const auto stage = std::countr_zero(stages);
			if (storage) stage_buffers[stage] += binding.descriptorCount;
			if (sampler) stage_samplers[stage] += binding.descriptorCount;
			else
				stage_resources[stage] += binding.descriptorCount;
			if (stage_buffers[stage] > limits.maxPerStageDescriptorStorageBuffers ||
			    stage_resources[stage] > limits.maxPerStageResources ||
			    stage_samplers[stage] > limits.maxPerStageDescriptorSamplers) {
				EXIT("native descriptor stage %u exceeds device limits: buffers=%" PRIu64
				     "/%u resources=%" PRIu64 "/%u samplers=%" PRIu64 "/%u\n", stage, stage_buffers[stage],
				     limits.maxPerStageDescriptorStorageBuffers, stage_resources[stage], limits.maxPerStageResources,
				     stage_samplers[stage], limits.maxPerStageDescriptorSamplers);
			}
		}
	}
	if (storage_buffers > limits.maxDescriptorSetStorageBuffers) {
		EXIT("native storage-buffer descriptors exceed device limit: %" PRIu64 "/%u\n",
		     storage_buffers, limits.maxDescriptorSetStorageBuffers);
	}
	if (samplers > limits.maxDescriptorSetSamplers) {
		EXIT("native sampler descriptors exceed device limit: %" PRIu64 "/%u\n",
		     samplers, limits.maxDescriptorSetSamplers);
	}
	pipeline.uses_push_descriptors = descriptor_count <= graphics.max_push_descriptors;

	vk::DescriptorSetLayoutCreateInfo create {};
	create.flags        = pipeline.uses_push_descriptors
	                          ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
	                          : vk::DescriptorSetLayoutCreateFlags {};
	create.bindingCount = static_cast<uint32_t>(bindings.size());
	create.pBindings    = bindings.data();
	EXIT_IF(graphics.device.createDescriptorSetLayout(
	            &create, nullptr, &pipeline.descriptor_set_layout) != vk::Result::eSuccess);
	const vk::PushConstantRange push_constants {pipeline.push_stages, 0,
	                                            ShaderRecompiler::IR::NativePushConstantSize};
	const std::array set_layouts {pipeline.descriptor_set_layout, graphics.image_heap_layout};

	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = pipeline.uses_image_heap ? 2u : 1u;
	pipeline_layout_info.pSetLayouts            = set_layouts.data();
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_constants;

	EXIT_NOT_IMPLEMENTED(graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                          &pipeline.pipeline_layout) !=
	                     vk::Result::eSuccess);
}

namespace {

// A fragment shader with no inputs and no outputs. The guest can issue a draw whose pixel shader
// has no observable side effects (a depth-only pass, for example). Vulkan still requires fragment
// shader state for a pipeline that rasterizes, so this stands in for it. Depth writes keep using
// the interpolated fragment depth, exactly like the guest's depth block.
constexpr std::array<uint32_t, 32> kNullFragmentShaderSpirv {
    0x07230203, 0x00010000, 0x00000000, 0x00000005, 0x00000000, // version 1.0, bound 5
    0x00020011, 0x00000001,                                     // OpCapability Shader
    0x0003000E, 0x00000000, 0x00000001,                         // OpMemoryModel Logical GLSL450
    0x0005000F, 0x00000004, 0x00000001, 0x6E69616D, 0x00000000, // OpEntryPoint Fragment %1 "main"
    0x00030010, 0x00000001, 0x00000007,                         // OpExecutionMode %1 OriginUpperLeft
    0x00020013, 0x00000002,                                     // %2 = OpTypeVoid
    0x00030021, 0x00000003, 0x00000002,                         // %3 = OpTypeFunction %2
    0x00050036, 0x00000002, 0x00000001, 0x00000000, 0x00000003, // %1 = OpFunction %2 None %3
    0x000200F8, 0x00000004,                                     // %4 = OpLabel
    0x000100FD,                                                 // OpReturn
    0x00010038};                                                // OpFunctionEnd

// Every state object a graphics pipeline is built from, kept in one place so the pNext chains
// and the arrays they point at stay alive together. It refers to its own members, so fill it in
// place and never copy or move it.
struct GraphicsPipelineState {
	// Modules that exist only for this creation.
	vk::ShaderModule null_fragment_module = nullptr;
	vk::ShaderModule task_module          = nullptr;
	vk::ShaderModule tess_control_module  = nullptr;
	vk::ShaderModule tess_eval_module     = nullptr;
	bool             rect_list           = false;
	bool             mesh                = false;
	bool             use_depth           = false;

	// VS/HS/DS plus a task stage and the two generated rectangle-list stages.
	std::array<vk::PipelineShaderStageCreateInfo, 8> pre_raster_stages {};
	uint32_t                                         pre_raster_stage_count = 0;
	vk::PipelineShaderStageCreateInfo                fragment_stage {};

	std::array<vk::VertexInputAttributeDescription, ShaderVertexInputInfo::RES_MAX> input_attr {};
	std::array<vk::VertexInputBindingDescription, ShaderVertexInputInfo::RES_MAX>   input_desc {};
	vk::PipelineVertexInputStateCreateInfo   vertex_input {};
	vk::PipelineInputAssemblyStateCreateInfo input_assembly {};

	vk::PipelineTessellationStateCreateInfo tessellation {};

	vk::PipelineViewportDepthClipControlCreateInfoEXT depth_clip_control {};
	vk::PipelineViewportStateCreateInfo               viewport {};

	vk::PipelineRasterizationDepthClipStateCreateInfoEXT      clip_ext {};
	vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT provoking_vertex {};
	vk::PipelineRasterizationStateCreateInfo                   rasterizer {};

	vk::PipelineMultisampleStateCreateInfo multisampling {};
	vk::PipelineDepthStencilStateCreateInfo depth_stencil {};

	std::array<vk::PipelineColorBlendAttachmentState, RENDER_COLOR_ATTACHMENTS_MAX>
	    color_blend_attachment {};
	vk::PipelineColorBlendStateCreateInfo color_blending {};

	std::vector<vk::DynamicState>      dynamic_states;
	vk::PipelineDynamicStateCreateInfo dynamic_state {};

	vk::PipelineRenderingCreateInfo rendering {};

	GraphicsPipelineState()                                        = default;
	GraphicsPipelineState(const GraphicsPipelineState&)            = delete;
	GraphicsPipelineState& operator=(const GraphicsPipelineState&) = delete;
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void BuildGraphicsPipelineState(GraphicContext& graphics, GraphicsPipelineState& state,
                                const GraphicsPipelineKey&                 key,
                                const std::array<const ShaderModule*, 4>& stages) {
	const auto& rendering     = key.rendering;
	const auto& vertex_input  = key.vertex_input;
	const auto& static_params = key.static_params;
	const auto& vertex        = *stages[0];
	const auto* pixel         = stages[3];
	state.use_depth           = rendering.depth_format != vk::Format::eUndefined ||
	                            rendering.stencil_format != vk::Format::eUndefined;
	state.mesh = vertex.info.stage == ShaderType::Mesh;
	EXIT_NOT_IMPLEMENTED(state.mesh && !graphics.mesh_shader_enabled);
	state.rect_list = static_params.patch_control_points != 0 && stages[1] == nullptr;

	if (vertex.mesh_task) {
		state.task_module = CompileSPV(GPU_MESH_INDIRECT_SPV, graphics.device);
	}
	if (state.rect_list) {
		std::span<const RectListParameter> parameters;
		if (pixel != nullptr) parameters = pixel->rect_parameters;
		const auto shaders = BuildRectListShaders(parameters, vertex.info.param_export_mask);
		state.tess_control_module = CompileSPV(shaders.control, graphics.device);
		state.tess_eval_module    = CompileSPV(shaders.evaluation, graphics.device);
	}

	// The fragment stage travels separately: the library split needs the stages before it.
	for (const auto* stage: stages) {
		if (stage != nullptr && stage != pixel) {
			state.pre_raster_stages[state.pre_raster_stage_count++] = {
			    .stage = NativeShaderStage(stage->info.stage), .module = stage->module,
			    .pName = "main"};
		}
	}
	if (state.task_module) {
		state.pre_raster_stages[state.pre_raster_stage_count++] = {
		    .stage = vk::ShaderStageFlagBits::eTaskEXT, .module = state.task_module,
		    .pName = "main"};
	}
	if (state.rect_list) {
		state.pre_raster_stages[state.pre_raster_stage_count++] = {
		    .stage = vk::ShaderStageFlagBits::eTessellationControl,
		    .module = state.tess_control_module, .pName = "main"};
		state.pre_raster_stages[state.pre_raster_stage_count++] = {
		    .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
		    .module = state.tess_eval_module, .pName = "main"};
	}
	if (pixel != nullptr) {
		state.fragment_stage = {
		    .stage = vk::ShaderStageFlagBits::eFragment, .module = pixel->module, .pName = "main"};
	} else {
		state.null_fragment_module = CompileSPV(kNullFragmentShaderSpirv, graphics.device);
		state.fragment_stage       = {.stage  = vk::ShaderStageFlagBits::eFragment,
		                              .module = state.null_fragment_module,
		                              .pName  = "main"};
	}

	for (uint32_t binding = 0; binding < vertex_input.binding_count; binding++) {
		state.input_desc[binding].binding   = binding;
		state.input_desc[binding].stride    = vertex_input.bindings[binding].stride;
		state.input_desc[binding].inputRate = vertex_input.bindings[binding].instance
		                                          ? vk::VertexInputRate::eInstance
		                                          : vk::VertexInputRate::eVertex;
	}
	for (uint32_t index = 0; index < vertex_input.attribute_count; index++) {
		state.input_attr[index].binding  = vertex_input.attributes[index].binding;
		state.input_attr[index].location = index;
		state.input_attr[index].offset   = vertex_input.attributes[index].offset;
		state.input_attr[index].format   = vertex.vertex_formats[index];
	}

	state.vertex_input.vertexBindingDescriptionCount   = vertex_input.binding_count;
	state.vertex_input.pVertexBindingDescriptions      = state.input_desc.data();
	state.vertex_input.vertexAttributeDescriptionCount = vertex_input.attribute_count;
	state.vertex_input.pVertexAttributeDescriptions    = state.input_attr.data();

	state.input_assembly.topology = static_params.topology;
	state.input_assembly.primitiveRestartEnable =
	    static_params.primitive_restart_enable ? VK_TRUE : VK_FALSE;

	state.depth_clip_control.negativeOneToOne =
	    (static_params.negative_one_to_one ? VK_TRUE : VK_FALSE);
	state.viewport.pNext = &state.depth_clip_control;

	state.clip_ext.depthClipEnable = static_params.depth_clip_enable ? VK_TRUE : VK_FALSE;

	// MoltenVK lacks VK_EXT_depth_clip_enable; omit the depth-clip struct on macOS and accept
	// Vulkan's default depth clipping (enabled) instead of the PS5's clamp behavior.
#if !defined(__APPLE__)
	// The DB clamps depth to the viewport range after polygon offset is applied.
	state.rasterizer.depthClampEnable = VK_TRUE;
	state.rasterizer.pNext            = &state.clip_ext;
#endif
	EXIT_NOT_IMPLEMENTED(static_params.provoking_vtx_last &&
	                     !graphics.provoking_vertex_last_enabled);
	if (graphics.provoking_vertex_last_enabled) {
		state.provoking_vertex.provokingVertexMode = static_params.provoking_vtx_last
		    ? vk::ProvokingVertexModeEXT::eLastVertex : vk::ProvokingVertexModeEXT::eFirstVertex;
		state.provoking_vertex.pNext = state.rasterizer.pNext;
		state.rasterizer.pNext       = &state.provoking_vertex;
	}
	state.rasterizer.polygonMode = static_params.polygon_mode;

	state.multisampling.sampleShadingEnable =
	    static_params.sample_shading_enable ? VK_TRUE : VK_FALSE;
	state.multisampling.rasterizationSamples = vulkan_sample_count(static_params.samples);
	state.multisampling.minSampleShading     = 1.0f;
	state.multisampling.alphaToCoverageEnable =
	    static_params.alpha_to_coverage_enable ? VK_TRUE : VK_FALSE;

	for (uint32_t i = 0; i < rendering.color_count; i++) {
		auto& attachment = state.color_blend_attachment[i];
		EXIT_NOT_IMPLEMENTED((static_params.color_mask[i] & ~0x0fu) != 0);
		attachment.colorWriteMask = vk::ColorComponentFlags {static_params.color_mask[i]};
		attachment.blendEnable    = static_params.blend_enable[i] ? VK_TRUE : VK_FALSE;
		attachment.srcColorBlendFactor = GetBlendFactor(static_params.color_srcblend[i]);
		attachment.dstColorBlendFactor = GetBlendFactor(static_params.color_destblend[i]);
		attachment.colorBlendOp        = GetBlendOp(static_params.color_comb_fcn[i]);
		attachment.srcAlphaBlendFactor =
		    (static_params.separate_alpha_blend[i]
		         ? GetBlendFactor(static_params.alpha_srcblend[i])
		         : attachment.srcColorBlendFactor);
		attachment.dstAlphaBlendFactor =
		    (static_params.separate_alpha_blend[i]
		         ? GetBlendFactor(static_params.alpha_destblend[i])
		         : attachment.dstColorBlendFactor);
		attachment.alphaBlendOp =
		    (static_params.separate_alpha_blend[i] ? GetBlendOp(static_params.alpha_comb_fcn[i])
		                                           : attachment.colorBlendOp);
	}

	state.color_blending.attachmentCount = rendering.color_count;
	state.color_blending.pAttachments    = state.color_blend_attachment.data();

	state.dynamic_states = {
	    vk::DynamicState::eViewportWithCount,
	    vk::DynamicState::eScissorWithCount,
	    vk::DynamicState::eLineWidth,
	    vk::DynamicState::eCullMode,
	    vk::DynamicState::eFrontFace,
	    vk::DynamicState::eDepthTestEnable,
	    vk::DynamicState::eDepthWriteEnable,
	    vk::DynamicState::eDepthCompareOp,
	    vk::DynamicState::eDepthBiasEnable,
	    vk::DynamicState::eDepthBias,
	    vk::DynamicState::eStencilTestEnable,
	    vk::DynamicState::eStencilOp,
	    vk::DynamicState::eStencilCompareMask,
	    vk::DynamicState::eStencilReference,
	    vk::DynamicState::eStencilWriteMask,
	    vk::DynamicState::eBlendConstants,
	};
#if !defined(__APPLE__)
	state.dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
	state.dynamic_states.push_back(vk::DynamicState::eDepthBounds);
#endif
	if (graphics.attachment_feedback_loop_enabled) {
		state.dynamic_states.push_back(vk::DynamicState::eAttachmentFeedbackLoopEnableEXT);
	}

	state.dynamic_state.dynamicStateCount = static_cast<uint32_t>(state.dynamic_states.size());
	state.dynamic_state.pDynamicStates    = state.dynamic_states.data();

	state.tessellation.patchControlPoints    = static_params.patch_control_points;
	state.rendering.colorAttachmentCount     = rendering.color_count;
	state.rendering.pColorAttachmentFormats  = rendering.color_formats.data();
	state.rendering.depthAttachmentFormat    = rendering.depth_format;
	state.rendering.stencilAttachmentFormat  = rendering.stencil_format;
}

// The descriptor set layout and pipeline layout every graphics pipeline needs. Built once, so
// the library path and the monolithic path can never disagree about them.
void PreparePipelineLayout(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                           const std::array<const ShaderModule*, 4>& stages,
                           vk::ShaderModule task_module) {
	std::vector<vk::DescriptorSetLayoutBinding> descriptor_bindings;
	pipeline.push_stages = vk::ShaderStageFlagBits::eFragment;
	if (task_module) {
		pipeline.push_stages |= vk::ShaderStageFlagBits::eTaskEXT;
	}
	for (const auto* stage: stages) {
		if (stage != nullptr) {
			const auto native_stage = NativeShaderStage(stage->info.stage);
			AddLayoutBindings(descriptor_bindings, stage->info, native_stage);
			pipeline.uses_image_heap |= stage->info.info.uses_image_heap;
			pipeline.push_stages |= native_stage;
		}
	}
	CreateLayouts(graphics, pipeline, descriptor_bindings);
}

void DestroyTemporaryModules(vk::Device device, GraphicsPipelineState& state) {
	if (state.null_fragment_module != nullptr) {
		device.destroyShaderModule(state.null_fragment_module, nullptr);
	}
	if (state.task_module != nullptr) {
		device.destroyShaderModule(state.task_module, nullptr);
	}
	if (state.tess_control_module != nullptr) {
		device.destroyShaderModule(state.tess_control_module, nullptr);
	}
	if (state.tess_eval_module != nullptr) {
		device.destroyShaderModule(state.tess_eval_module, nullptr);
	}
	state.null_fragment_module = nullptr;
	state.task_module          = nullptr;
	state.tess_control_module  = nullptr;
	state.tess_eval_module     = nullptr;
}

// The whole pipeline in one create, which is what a device without pipeline libraries gets.
void CreatePipelineFromState(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                             GraphicsPipelineState& state, vk::PipelineCache driver_cache) {
	std::array<vk::PipelineShaderStageCreateInfo, 8> shader_stages {};
	uint32_t                                         shader_stage_count = 0;
	for (uint32_t i = 0; i < state.pre_raster_stage_count; i++) {
		shader_stages[shader_stage_count++] = state.pre_raster_stages[i];
	}
	if (state.fragment_stage.module != nullptr) {
		shader_stages[shader_stage_count++] = state.fragment_stage;
	}

	vk::GraphicsPipelineCreateInfo pipeline_info {};
	pipeline_info.pNext               = &state.rendering;
	pipeline_info.stageCount          = shader_stage_count;
	pipeline_info.pStages             = shader_stages.data();
	pipeline_info.pVertexInputState   = state.mesh ? nullptr : &state.vertex_input;
	pipeline_info.pInputAssemblyState = state.mesh ? nullptr : &state.input_assembly;
	pipeline_info.pTessellationState =
	    state.tessellation.patchControlPoints != 0 ? &state.tessellation : nullptr;
	pipeline_info.pViewportState      = &state.viewport;
	pipeline_info.pRasterizationState = &state.rasterizer;
	pipeline_info.pMultisampleState   = &state.multisampling;
	pipeline_info.pDepthStencilState  = state.use_depth ? &state.depth_stencil : nullptr;
	pipeline_info.pColorBlendState    = &state.color_blending;
	pipeline_info.pDynamicState       = &state.dynamic_state;
	pipeline_info.layout              = pipeline.pipeline_layout;

	const auto result = graphics.device.createGraphicsPipelines(driver_cache, 1, &pipeline_info,
	                                                            nullptr, &pipeline.pipeline);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

// The four libraries of one graphics pipeline. The handles are owned by the pipeline so the
// background relink can use them; the create infos point into the caller's state.
struct GraphicsPipelineLibrarySet {
	std::array<vk::GraphicsPipelineCreateInfo, 4>         infos {};
	// Every library has to name the subsets it defines; without this struct the create info
	// declares none of them and the library comes out empty.
	std::array<vk::GraphicsPipelineLibraryCreateInfoEXT, 4> subsets {};
	std::array<vk::Pipeline, 4>                             handles {};
	uint32_t                                                count = 0;

	// `subset` is stored in the set, so the create info may point into it until the create call.
	void Add(const vk::GraphicsPipelineCreateInfo& info, uint32_t index,
	         vk::GraphicsPipelineLibraryFlagBitsEXT subset) {
		subsets[index].flags = subset;
		infos[index]         = info;
		infos[index].pNext   = &subsets[index];
		count                = index + 1;
	}
};

void BuildLibraryCreateInfos(GraphicsPipelineState& state, vk::PipelineLayout layout,
                             GraphicsPipelineLibrarySet& set) {
	const auto flags = vk::PipelineCreateFlagBits::eLibraryKHR |
	                   vk::PipelineCreateFlagBits::eRetainLinkTimeOptimizationInfoEXT;

	// Vertex input: the vertex input and input assembly state, and nothing else.
	vk::GraphicsPipelineCreateInfo vertex_input_info {};
	vertex_input_info.flags               = flags;
	vertex_input_info.pVertexInputState   = &state.vertex_input;
	vertex_input_info.pInputAssemblyState = &state.input_assembly;
	vertex_input_info.pDynamicState       = &state.dynamic_state;
	vertex_input_info.layout              = layout;
	set.Add(vertex_input_info, 0, vk::GraphicsPipelineLibraryFlagBitsEXT::eVertexInputInterface);

	// Pre-rasterization: the stages before the fragment shader and the state feeding them. The
	// multisample state is not part of this subset, but the spec still demands it here.
	vk::GraphicsPipelineCreateInfo pre_raster_info {};
	pre_raster_info.flags              = flags;
	pre_raster_info.stageCount         = state.pre_raster_stage_count;
	pre_raster_info.pStages            = state.pre_raster_stages.data();
	pre_raster_info.pTessellationState = state.tessellation.patchControlPoints != 0
	                                         ? &state.tessellation : nullptr;
	pre_raster_info.pViewportState      = &state.viewport;
	pre_raster_info.pRasterizationState = &state.rasterizer;
	pre_raster_info.pMultisampleState   = &state.multisampling;
	pre_raster_info.pDynamicState       = &state.dynamic_state;
	pre_raster_info.layout              = layout;
	set.Add(pre_raster_info, 1, vk::GraphicsPipelineLibraryFlagBitsEXT::ePreRasterizationShaders);

	// Fragment shader: the pixel stage with the state it reads. The multisample state is part of
	// this subset and of the fragment output interface, so both have to name the same values.
	uint32_t next = 2;
	if (state.fragment_stage.module != nullptr) {
		vk::GraphicsPipelineCreateInfo fragment_info {};
		fragment_info.flags              = flags;
		fragment_info.stageCount         = 1;
		fragment_info.pStages            = &state.fragment_stage;
		fragment_info.pMultisampleState  = &state.multisampling;
		fragment_info.pDepthStencilState = &state.depth_stencil;
		fragment_info.pDynamicState      = &state.dynamic_state;
		fragment_info.layout             = layout;
		set.Add(fragment_info, next++, vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentShader);
	}

	// Fragment output: colour blend and the formats the render pass would otherwise carry.
	vk::GraphicsPipelineCreateInfo fragment_output_info {};
	fragment_output_info.flags             = flags;
	fragment_output_info.pColorBlendState  = &state.color_blending;
	fragment_output_info.pMultisampleState = &state.multisampling;
	// Colour write enable belongs to this subset, so its dynamic declaration has to be here; a
	// linked pipeline only inherits the dynamic states its libraries declare.
	fragment_output_info.pDynamicState = &state.dynamic_state;
	fragment_output_info.layout        = layout;
	set.Add(fragment_output_info, next, vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentOutputInterface);
	// The formats belong to this subset, so they travel behind the subset description.
	set.subsets[next].pNext = &state.rendering;
}

vk::Pipeline LinkPipeline(GraphicContext& graphics, vk::PipelineLayout layout,
                          const GraphicsPipelineLibrarySet& set, bool link_time_optimization,
                          vk::PipelineCache driver_cache) {
	vk::PipelineLibraryCreateInfoKHR library_info {};
	library_info.libraryCount = set.count;
	library_info.pLibraries   = set.handles.data();

	// A link names only the libraries, the layout and the flags; re-declaring linked state is
	// what the spec forbids.
	vk::GraphicsPipelineCreateInfo link_info {};
	link_info.pNext  = &library_info;
	link_info.layout = layout;
	if (link_time_optimization) {
		link_info.flags = vk::PipelineCreateFlagBits::eLinkTimeOptimizationEXT;
	}

	vk::Pipeline linked = nullptr;
	if (graphics.device.createGraphicsPipelines(driver_cache, 1, &link_info, nullptr, &linked) !=
	    vk::Result::eSuccess) {
		return nullptr;
	}
	return linked;
}

// False when the driver rejects the library path; the pipeline is left untouched then so the
// single-create path can still run.
bool CreatePipelineFromLibraries(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                                 GraphicsPipelineState& state, vk::PipelineCache driver_cache) {
	GraphicsPipelineLibrarySet set;
	BuildLibraryCreateInfos(state, pipeline.pipeline_layout, set);

	if (graphics.device.createGraphicsPipelines(driver_cache, set.count, set.infos.data(), nullptr,
	                                            set.handles.data()) != vk::Result::eSuccess) {
		for (const auto handle: set.handles) {
			if (handle != nullptr) graphics.device.destroyPipeline(handle, nullptr);
		}
		return false;
	}

	// Without fast linking there is nothing to gain from a first, unoptimized link.
	const bool fast_linking = graphics.graphics_pipeline_library_fast_linking;
	const auto linked       = LinkPipeline(graphics, pipeline.pipeline_layout, set, !fast_linking,
	                                       driver_cache);
	if (linked == nullptr) {
		for (const auto handle: set.handles) {
			graphics.device.destroyPipeline(handle, nullptr);
		}
		return false;
	}

	pipeline.pipeline      = linked;
	pipeline.libraries     = set.handles;
	pipeline.library_count = set.count;
	// A link that already ran with LTO has nothing left to improve.
	pipeline.lto_done = !fast_linking;
	return true;
}

} // namespace

vk::Pipeline LinkPipelineLibraries(GraphicContext& graphics, vk::PipelineLayout layout,
                                   std::span<const vk::Pipeline> libraries,
                                   bool                          link_time_optimization,
                                   vk::PipelineCache             driver_cache) {
	GraphicsPipelineLibrarySet set;
	set.count = static_cast<uint32_t>(libraries.size());
	for (uint32_t i = 0; i < set.count; i++) {
		set.handles[i] = libraries[i];
	}
	return LinkPipeline(graphics, layout, set, link_time_optimization, driver_cache);
}

bool CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const GraphicsPipelineKey&                 key,
                            const std::array<const ShaderModule*, 4>& stages,
                            vk::PipelineCache driver_cache, bool allow_pipeline_library) {
	GraphicsPipelineState state;
	BuildGraphicsPipelineState(graphics, state, key, stages);
	PreparePipelineLayout(graphics, pipeline, stages, state.task_module);

	// Mesh pipelines have no vertex input library, so they stay on the single-create path.
	const bool libraries =
	    allow_pipeline_library && !state.mesh &&
	    CreatePipelineFromLibraries(graphics, pipeline, state, driver_cache);
	if (!libraries) {
		CreatePipelineFromState(graphics, pipeline, state, driver_cache);
	}
	DestroyTemporaryModules(graphics.device, state);
	return libraries;
}

void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const ShaderModule& module, vk::PipelineCache driver_cache) {
	vk::PipelineShaderStageCreateInfo                     comp_shader_stage_info {};
	vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo comp_subgroup_size {};
	comp_shader_stage_info.stage  = vk::ShaderStageFlagBits::eCompute;
	comp_shader_stage_info.module = module.module;
	comp_shader_stage_info.pName  = "main";
	const auto wave_size = module.info.wave_size;
	if (graphics.compute_subgroup_size_control_enabled &&
	    wave_size >= graphics.min_subgroup_size && wave_size <= graphics.max_subgroup_size) {
		comp_subgroup_size.requiredSubgroupSize = wave_size;
		comp_shader_stage_info.pNext            = &comp_subgroup_size;
	}

	std::vector<vk::DescriptorSetLayoutBinding> descriptor_bindings;
	AddLayoutBindings(descriptor_bindings, module.info, vk::ShaderStageFlagBits::eCompute);
	pipeline.uses_image_heap = module.info.info.uses_image_heap;
	pipeline.push_stages     = vk::ShaderStageFlagBits::eCompute;
	CreateLayouts(graphics, pipeline, descriptor_bindings);

	vk::ComputePipelineCreateInfo info {};
	if (module.info.info.uses_gds) {
		info.flags = vk::PipelineCreateFlagBits::eDispatchBase;
	}
	info.stage  = comp_shader_stage_info;
	info.layout = pipeline.pipeline_layout;

	const auto result =
	    graphics.device.createComputePipelines(driver_cache, 1, &info, nullptr, &pipeline.pipeline);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

} // namespace Libs::Graphics
