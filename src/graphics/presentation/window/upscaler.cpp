#include "graphics/presentation/window/upscaler.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_upscale_dlss_tsr_spv.h"
#include "gpu_blit_shaders/gpu_upscale_fsr_easu_spv.h"
#include "gpu_blit_shaders/gpu_upscale_fsr_rcas_spv.h"

#include <algorithm>
#include <array>
#include <span>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Libs::Graphics {

namespace {

vk::Pipeline CreateUpscalePipeline(vk::Device device, vk::PipelineLayout layout,
                                   std::span<const uint32_t> fragment_code, vk::Format format) {
	const auto vertex   = CompileSPV(GPU_BLIT_FS_TRIANGLE_SPV, device);
	const auto fragment = CompileSPV(fragment_code, device);
	std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
	stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
	stages[0].module = vertex;
	stages[0].pName  = "main";
	stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
	stages[1].module = fragment;
	stages[1].pName  = "main";
	vk::PipelineVertexInputStateCreateInfo   vertex_input {};
	vk::PipelineInputAssemblyStateCreateInfo assembly {};
	assembly.topology = vk::PrimitiveTopology::eTriangleList;
	vk::PipelineViewportStateCreateInfo viewport {};
	viewport.viewportCount = 1;
	viewport.scissorCount  = 1;
	vk::PipelineRasterizationStateCreateInfo rasterization {};
	rasterization.lineWidth = 1.0f;
	vk::PipelineMultisampleStateCreateInfo multisample {};
	multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
	vk::PipelineColorBlendAttachmentState attachment {};
	attachment.blendEnable    = VK_FALSE;
	attachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	                            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
	vk::PipelineColorBlendStateCreateInfo blend {};
	blend.attachmentCount = 1;
	blend.pAttachments    = &attachment;
	const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
	vk::PipelineDynamicStateCreateInfo dynamic {};
	dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
	dynamic.pDynamicStates    = dynamic_states.data();
	vk::PipelineRenderingCreateInfo rendering {};
	rendering.colorAttachmentCount    = 1;
	rendering.pColorAttachmentFormats = &format;
	vk::GraphicsPipelineCreateInfo create {};
	create.pNext               = &rendering;
	create.stageCount          = static_cast<uint32_t>(stages.size());
	create.pStages             = stages.data();
	create.pVertexInputState   = &vertex_input;
	create.pInputAssemblyState = &assembly;
	create.pViewportState      = &viewport;
	create.pRasterizationState = &rasterization;
	create.pMultisampleState   = &multisample;
	create.pColorBlendState    = &blend;
	create.pDynamicState       = &dynamic;
	create.layout              = layout;
	vk::Pipeline pipeline      = nullptr;
	RequireVulkanSuccess(device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &pipeline),
	                     "create upscaler graphics pipeline");
	device.destroyShaderModule(fragment, nullptr);
	device.destroyShaderModule(vertex, nullptr);
	return pipeline;
}

void TransitUpscalerImage(vk::CommandBuffer command, VulkanImage& img, vk::ImageLayout new_layout,
                          vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask                    = img.state.pl_stage;
	barrier.srcAccessMask                   = img.state.access_mask;
	barrier.dstStageMask                    = dst_stage;
	barrier.dstAccessMask                   = dst_access;
	barrier.oldLayout                       = img.state.layout;
	barrier.newLayout                       = new_layout;
	barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                           = img.image;
	barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.baseMipLevel   = 0;
	barrier.subresourceRange.levelCount     = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount     = 1;
	vk::DependencyInfo dep {};
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dep);
	img.state = {dst_stage, dst_access, new_layout};
}

void TransitRawImage(vk::CommandBuffer command, vk::Image image, vk::ImageLayout old_layout,
                     vk::ImageLayout new_layout, vk::PipelineStageFlags2 src_stage,
                     vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                     vk::AccessFlags2 dst_access) {
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask                    = src_stage;
	barrier.srcAccessMask                   = src_access;
	barrier.dstStageMask                    = dst_stage;
	barrier.dstAccessMask                   = dst_access;
	barrier.oldLayout                       = old_layout;
	barrier.newLayout                       = new_layout;
	barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                           = image;
	barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.baseMipLevel   = 0;
	barrier.subresourceRange.levelCount     = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount     = 1;
	vk::DependencyInfo dep {};
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dep);
}

void DrawFullscreenPass(vk::CommandBuffer command, vk::Pipeline pipeline, vk::ImageView target_view,
                        vk::Extent2D target_extent) {
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = target_view;
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp      = vk::AttachmentLoadOp::eDontCare;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = target_extent;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	command.beginRendering(&rendering);
	const vk::Viewport viewport {0.0f,
	                             0.0f,
	                             static_cast<float>(target_extent.width),
	                             static_cast<float>(target_extent.height),
	                             0.0f,
	                             1.0f};
	const vk::Rect2D   scissor {{0, 0}, target_extent};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.draw(3, 1, 0, 0);
	command.endRendering();
}

} // namespace

UpscalerPass::UpscalerPass(GraphicContext& graphics): m_graphics(graphics) {
	ProbeNativeNgxRuntime();
}

UpscalerPass::~UpscalerPass() {
	Release();
}

void UpscalerPass::ProbeNativeNgxRuntime() {
	const auto& props = m_graphics.GetPhysicalDeviceProperties();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (props.vendorID == 0x10deu) {
		HMODULE ngx_mod = ::LoadLibraryExW(
		    L"nvngx_dlss.dll", nullptr,
		    LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 |
		        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		if (ngx_mod == nullptr) {
			ngx_mod = ::LoadLibraryExW(
			    L"_nvngx.dll", nullptr,
			    LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 |
			        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		}
		if (ngx_mod != nullptr) {
			m_native_ngx_available = true;
			::FreeLibrary(ngx_mod);
			LOGF("Upscaler: NVIDIA GPU (%s) detected with NGX runtime available; Vulkan 1.3 "
			     "DLSS/TSR + FSR pipelines ready\n",
			     props.deviceName.data());
			return;
		}
	}
#endif
	LOGF("Upscaler: Cross-vendor Vulkan 1.3 DLSS (TSR) and FSR (EASU+RCAS) pipelines ready on %s "
	     "(vendor=0x%04x)\n",
	     props.deviceName.data(), props.vendorID);
}

void UpscalerPass::Release() {
	auto device = m_graphics.device;
	if (device == nullptr) {
		return;
	}
	if (m_intermediate_view != nullptr) {
		device.destroyImageView(m_intermediate_view, nullptr);
		m_intermediate_view = nullptr;
	}
	if (m_intermediate_image.image != nullptr) {
		m_graphics.DeleteImage(m_intermediate_image);
		m_intermediate_image.image = nullptr;
	}
	if (m_history_view != nullptr) {
		device.destroyImageView(m_history_view, nullptr);
		m_history_view = nullptr;
	}
	if (m_history_image.image != nullptr) {
		m_graphics.DeleteImage(m_history_image);
		m_history_image.image = nullptr;
	}
	if (m_prev_input_view != nullptr) {
		device.destroyImageView(m_prev_input_view, nullptr);
		m_prev_input_view = nullptr;
	}
	if (m_prev_input_image.image != nullptr) {
		m_graphics.DeleteImage(m_prev_input_image);
		m_prev_input_image.image = nullptr;
	}
	if (m_easu_pipeline != nullptr) {
		device.destroyPipeline(m_easu_pipeline, nullptr);
		m_easu_pipeline = nullptr;
	}
	if (m_rcas_pipeline != nullptr) {
		device.destroyPipeline(m_rcas_pipeline, nullptr);
		m_rcas_pipeline = nullptr;
	}
	if (m_tsr_pipeline != nullptr) {
		device.destroyPipeline(m_tsr_pipeline, nullptr);
		m_tsr_pipeline = nullptr;
	}
	if (m_easu_layout != nullptr) {
		device.destroyPipelineLayout(m_easu_layout, nullptr);
		m_easu_layout = nullptr;
	}
	if (m_rcas_layout != nullptr) {
		device.destroyPipelineLayout(m_rcas_layout, nullptr);
		m_rcas_layout = nullptr;
	}
	if (m_tsr_layout != nullptr) {
		device.destroyPipelineLayout(m_tsr_layout, nullptr);
		m_tsr_layout = nullptr;
	}
	if (m_single_tex_desc != nullptr) {
		device.destroyDescriptorSetLayout(m_single_tex_desc, nullptr);
		m_single_tex_desc = nullptr;
	}
	if (m_tsr_tex_desc != nullptr) {
		device.destroyDescriptorSetLayout(m_tsr_tex_desc, nullptr);
		m_tsr_tex_desc = nullptr;
	}
	if (m_sampler != nullptr) {
		device.destroySampler(m_sampler, nullptr);
		m_sampler = nullptr;
	}
	m_src_extent      = {};
	m_src_format      = vk::Format::eUndefined;
	m_dst_extent      = {};
	m_dst_format      = vk::Format::eUndefined;
	m_pipeline_format = vk::Format::eUndefined;
	m_history_valid   = false;
}

void UpscalerPass::EnsurePipelines(vk::Format dst_format) {
	auto device = m_graphics.device;
	if (m_sampler == nullptr) {
		vk::SamplerCreateInfo sampler {};
		sampler.magFilter    = vk::Filter::eLinear;
		sampler.minFilter    = vk::Filter::eLinear;
		sampler.mipmapMode   = vk::SamplerMipmapMode::eNearest;
		sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		RequireVulkanSuccess(device.createSampler(&sampler, nullptr, &m_sampler),
		                     "create upscaler sampler");

		const vk::DescriptorSetLayoutBinding single_binding {
		    0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment};
		vk::DescriptorSetLayoutCreateInfo single_desc {};
		single_desc.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		single_desc.bindingCount = 1;
		single_desc.pBindings    = &single_binding;
		RequireVulkanSuccess(
		    device.createDescriptorSetLayout(&single_desc, nullptr, &m_single_tex_desc),
		    "create upscaler 1-tex descriptor layout");

		const std::array<vk::DescriptorSetLayoutBinding, 3> tsr_bindings {{
		    {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
		    {1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
		    {2, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
		}};
		vk::DescriptorSetLayoutCreateInfo tsr_desc {};
		tsr_desc.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		tsr_desc.bindingCount = static_cast<uint32_t>(tsr_bindings.size());
		tsr_desc.pBindings    = tsr_bindings.data();
		RequireVulkanSuccess(device.createDescriptorSetLayout(&tsr_desc, nullptr, &m_tsr_tex_desc),
		                     "create upscaler TSR descriptor layout");

		const vk::PushConstantRange range16 {vk::ShaderStageFlagBits::eFragment, 0, 16};
		vk::PipelineLayoutCreateInfo layout16 {};
		layout16.setLayoutCount         = 1;
		layout16.pSetLayouts            = &m_single_tex_desc;
		layout16.pushConstantRangeCount = 1;
		layout16.pPushConstantRanges    = &range16;
		RequireVulkanSuccess(device.createPipelineLayout(&layout16, nullptr, &m_easu_layout),
		                     "create EASU pipeline layout");
		RequireVulkanSuccess(device.createPipelineLayout(&layout16, nullptr, &m_rcas_layout),
		                     "create RCAS pipeline layout");

		const vk::PushConstantRange range24 {vk::ShaderStageFlagBits::eFragment, 0, 24};
		vk::PipelineLayoutCreateInfo layout24 {};
		layout24.setLayoutCount         = 1;
		layout24.pSetLayouts            = &m_tsr_tex_desc;
		layout24.pushConstantRangeCount = 1;
		layout24.pPushConstantRanges    = &range24;
		RequireVulkanSuccess(device.createPipelineLayout(&layout24, nullptr, &m_tsr_layout),
		                     "create TSR pipeline layout");
	}

	if (m_pipeline_format != dst_format) {
		if (m_easu_pipeline != nullptr) {
			device.destroyPipeline(m_easu_pipeline, nullptr);
			m_easu_pipeline = nullptr;
		}
		if (m_rcas_pipeline != nullptr) {
			device.destroyPipeline(m_rcas_pipeline, nullptr);
			m_rcas_pipeline = nullptr;
		}
		if (m_tsr_pipeline != nullptr) {
			device.destroyPipeline(m_tsr_pipeline, nullptr);
			m_tsr_pipeline = nullptr;
		}
	}

	if (m_easu_pipeline == nullptr) {
		m_easu_pipeline   = CreateUpscalePipeline(device, m_easu_layout, GPU_UPSCALE_FSR_EASU_SPV, dst_format);
		m_rcas_pipeline   = CreateUpscalePipeline(device, m_rcas_layout, GPU_UPSCALE_FSR_RCAS_SPV, dst_format);
		m_tsr_pipeline    = CreateUpscalePipeline(device, m_tsr_layout, GPU_UPSCALE_DLSS_TSR_SPV, dst_format);
		m_pipeline_format = dst_format;
	}
}

void UpscalerPass::EnsureTargets(vk::Extent2D src_extent, vk::Format src_format,
                                 vk::Extent2D dst_extent, vk::Format dst_format) {
	auto device = m_graphics.device;

	const bool dst_changed = m_intermediate_image.image == nullptr ||
	                         m_dst_extent.width != dst_extent.width ||
	                         m_dst_extent.height != dst_extent.height || m_dst_format != dst_format;
	if (dst_changed) {
		if (m_intermediate_view != nullptr || m_history_view != nullptr) {
			Common::LockGuard lock(m_graphics.queue_mutex);
			(void)m_graphics.queue.waitIdle();
		}
		if (m_intermediate_view != nullptr) {
			device.destroyImageView(m_intermediate_view, nullptr);
			m_intermediate_view = nullptr;
		}
		if (m_intermediate_image.image != nullptr) {
			m_graphics.DeleteImage(m_intermediate_image);
			m_intermediate_image.image = nullptr;
		}
		if (m_history_view != nullptr) {
			device.destroyImageView(m_history_view, nullptr);
			m_history_view = nullptr;
		}
		if (m_history_image.image != nullptr) {
			m_graphics.DeleteImage(m_history_image);
			m_history_image.image = nullptr;
		}

		vk::ImageCreateInfo img_info {};
		img_info.imageType     = vk::ImageType::e2D;
		img_info.extent        = {dst_extent.width, dst_extent.height, 1};
		img_info.mipLevels     = 1;
		img_info.arrayLayers   = 1;
		img_info.format        = dst_format;
		img_info.tiling        = vk::ImageTiling::eOptimal;
		img_info.initialLayout = vk::ImageLayout::eUndefined;
		img_info.usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
		                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
		img_info.sharingMode = vk::SharingMode::eExclusive;
		img_info.samples     = vk::SampleCountFlagBits::e1;

		EXIT_IF(!m_graphics.CreateImage(img_info, m_intermediate_image));
		EXIT_IF(!m_graphics.CreateImage(img_info, m_history_image));

		vk::ImageViewCreateInfo view_info {};
		view_info.viewType         = vk::ImageViewType::e2D;
		view_info.format           = dst_format;
		view_info.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

		view_info.image = m_intermediate_image.image;
		RequireVulkanSuccess(device.createImageView(&view_info, nullptr, &m_intermediate_view),
		                     "create upscaler intermediate view");
		view_info.image = m_history_image.image;
		RequireVulkanSuccess(device.createImageView(&view_info, nullptr, &m_history_view),
		                     "create upscaler history view");

		m_dst_extent    = dst_extent;
		m_dst_format    = dst_format;
		m_history_valid = false;
	}

	const bool src_changed = m_prev_input_image.image == nullptr ||
	                         m_src_extent.width != src_extent.width ||
	                         m_src_extent.height != src_extent.height || m_src_format != src_format;
	if (src_changed) {
		if (m_prev_input_view != nullptr) {
			Common::LockGuard lock(m_graphics.queue_mutex);
			(void)m_graphics.queue.waitIdle();
		}
		if (m_prev_input_view != nullptr) {
			device.destroyImageView(m_prev_input_view, nullptr);
			m_prev_input_view = nullptr;
		}
		if (m_prev_input_image.image != nullptr) {
			m_graphics.DeleteImage(m_prev_input_image);
			m_prev_input_image.image = nullptr;
		}

		vk::ImageCreateInfo img_info {};
		img_info.imageType     = vk::ImageType::e2D;
		img_info.extent        = {src_extent.width, src_extent.height, 1};
		img_info.mipLevels     = 1;
		img_info.arrayLayers   = 1;
		img_info.format        = src_format;
		img_info.tiling        = vk::ImageTiling::eOptimal;
		img_info.initialLayout = vk::ImageLayout::eUndefined;
		img_info.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
		                 vk::ImageUsageFlagBits::eTransferSrc;
		img_info.sharingMode = vk::SharingMode::eExclusive;
		img_info.samples     = vk::SampleCountFlagBits::e1;

		EXIT_IF(!m_graphics.CreateImage(img_info, m_prev_input_image));

		vk::ImageViewCreateInfo view_info {};
		view_info.image            = m_prev_input_image.image;
		view_info.viewType         = vk::ImageViewType::e2D;
		view_info.format           = src_format;
		view_info.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		RequireVulkanSuccess(device.createImageView(&view_info, nullptr, &m_prev_input_view),
		                     "create upscaler prev input view");

		m_src_extent    = src_extent;
		m_src_format    = src_format;
		m_history_valid = false;
	}
}

void UpscalerPass::Record(vk::CommandBuffer command, vk::Image src_image, vk::ImageView src_view,
                          vk::Extent2D src_extent, vk::Format src_format, vk::ImageView dst_view,
                          vk::Extent2D dst_extent, vk::Format dst_format,
                          Config::UpscalerType mode, uint32_t sharpness_percent,
                          bool is_midpoint) {
	EnsurePipelines(dst_format);
	EnsureTargets(src_extent, src_format, dst_extent, dst_format);

	if (mode != m_last_mode) {
		m_history_valid = false;
		m_last_mode     = mode;
	}

	const float inv_src_x = 1.0f / static_cast<float>(std::max(src_extent.width, 1u));
	const float inv_src_y = 1.0f / static_cast<float>(std::max(src_extent.height, 1u));
	const float inv_dst_x = 1.0f / static_cast<float>(std::max(dst_extent.width, 1u));
	const float inv_dst_y = 1.0f / static_cast<float>(std::max(dst_extent.height, 1u));

	TransitUpscalerImage(command, m_intermediate_image, vk::ImageLayout::eColorAttachmentOptimal,
	                     vk::PipelineStageFlagBits2::eColorAttachmentOutput,
	                     vk::AccessFlagBits2::eColorAttachmentWrite);

	if (mode == Config::UpscalerType::Dlss && !is_midpoint) {
		if (!m_history_valid) {
			TransitUpscalerImage(command, m_history_image,
			                     vk::ImageLayout::eShaderReadOnlyOptimal,
			                     vk::PipelineStageFlagBits2::eFragmentShader,
			                     vk::AccessFlagBits2::eShaderRead);
			TransitUpscalerImage(command, m_prev_input_image,
			                     vk::ImageLayout::eShaderReadOnlyOptimal,
			                     vk::PipelineStageFlagBits2::eFragmentShader,
			                     vk::AccessFlagBits2::eShaderRead);
		}

		const std::array<vk::DescriptorImageInfo, 3> tsr_images {{
		    {m_sampler, src_view, vk::ImageLayout::eShaderReadOnlyOptimal},
		    {m_sampler, m_history_view, vk::ImageLayout::eShaderReadOnlyOptimal},
		    {m_sampler, m_prev_input_view, vk::ImageLayout::eShaderReadOnlyOptimal},
		}};
		std::array<vk::WriteDescriptorSet, 3> tsr_writes {};
		tsr_writes[0].dstBinding      = 0;
		tsr_writes[0].descriptorCount = 1;
		tsr_writes[0].descriptorType  = vk::DescriptorType::eCombinedImageSampler;
		tsr_writes[0].pImageInfo      = &tsr_images[0];
		tsr_writes[1].dstBinding      = 1;
		tsr_writes[1].descriptorCount = 1;
		tsr_writes[1].descriptorType  = vk::DescriptorType::eCombinedImageSampler;
		tsr_writes[1].pImageInfo      = &tsr_images[1];
		tsr_writes[2].dstBinding      = 2;
		tsr_writes[2].descriptorCount = 1;
		tsr_writes[2].descriptorType  = vk::DescriptorType::eCombinedImageSampler;
		tsr_writes[2].pImageInfo      = &tsr_images[2];
		command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_tsr_layout, 0,
		                             static_cast<uint32_t>(tsr_writes.size()), tsr_writes.data());

		struct TsrConstants {
			float inv_src_size[2];
			float inv_dst_size[2];
			float history_weight;
			float reset_history;
		};
		static_assert(sizeof(TsrConstants) == 24);
		const TsrConstants tsr_constants {
		    {inv_src_x, inv_src_y},
		    {inv_dst_x, inv_dst_y},
		    0.20f,
		    m_history_valid ? 0.0f : 1.0f,
		};
		command.pushConstants(m_tsr_layout, vk::ShaderStageFlagBits::eFragment, 0,
		                      sizeof(tsr_constants), &tsr_constants);
		DrawFullscreenPass(command, m_tsr_pipeline, m_intermediate_view, dst_extent);

		// Copy reconstructed output to m_history_image for the next frame
		TransitUpscalerImage(command, m_intermediate_image, vk::ImageLayout::eTransferSrcOptimal,
		                     vk::PipelineStageFlagBits2::eTransfer,
		                     vk::AccessFlagBits2::eTransferRead);
		TransitUpscalerImage(command, m_history_image, vk::ImageLayout::eTransferDstOptimal,
		                     vk::PipelineStageFlagBits2::eTransfer,
		                     vk::AccessFlagBits2::eTransferWrite);
		vk::ImageCopy hist_copy {};
		hist_copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		hist_copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		hist_copy.extent         = {dst_extent.width, dst_extent.height, 1};
		command.copyImage(m_intermediate_image.image, vk::ImageLayout::eTransferSrcOptimal,
		                  m_history_image.image, vk::ImageLayout::eTransferDstOptimal, 1,
		                  &hist_copy);
		TransitUpscalerImage(command, m_history_image, vk::ImageLayout::eShaderReadOnlyOptimal,
		                     vk::PipelineStageFlagBits2::eFragmentShader,
		                     vk::AccessFlagBits2::eShaderRead);

		// Copy current input to m_prev_input_image for next frame's motion estimation
		TransitRawImage(command, src_image, vk::ImageLayout::eShaderReadOnlyOptimal,
		                vk::ImageLayout::eTransferSrcOptimal,
		                vk::PipelineStageFlagBits2::eFragmentShader,
		                vk::AccessFlagBits2::eShaderRead, vk::PipelineStageFlagBits2::eTransfer,
		                vk::AccessFlagBits2::eTransferRead);
		TransitUpscalerImage(command, m_prev_input_image, vk::ImageLayout::eTransferDstOptimal,
		                     vk::PipelineStageFlagBits2::eTransfer,
		                     vk::AccessFlagBits2::eTransferWrite);
		vk::ImageCopy input_copy {};
		input_copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		input_copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		input_copy.extent         = {src_extent.width, src_extent.height, 1};
		command.copyImage(src_image, vk::ImageLayout::eTransferSrcOptimal,
		                  m_prev_input_image.image, vk::ImageLayout::eTransferDstOptimal, 1,
		                  &input_copy);
		TransitUpscalerImage(command, m_prev_input_image, vk::ImageLayout::eShaderReadOnlyOptimal,
		                     vk::PipelineStageFlagBits2::eFragmentShader,
		                     vk::AccessFlagBits2::eShaderRead);
		TransitRawImage(command, src_image, vk::ImageLayout::eTransferSrcOptimal,
		                vk::ImageLayout::eShaderReadOnlyOptimal,
		                vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
		                vk::PipelineStageFlagBits2::eFragmentShader,
		                vk::AccessFlagBits2::eShaderRead);
		m_history_valid = true;
	} else {
		// FSR Pass 1: EASU (12-tap Edge-Adaptive Spatial Upsampling)
		const vk::DescriptorImageInfo easu_img {m_sampler, src_view,
		                                        vk::ImageLayout::eShaderReadOnlyOptimal};
		vk::WriteDescriptorSet easu_write {};
		easu_write.dstBinding      = 0;
		easu_write.descriptorCount = 1;
		easu_write.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
		easu_write.pImageInfo      = &easu_img;
		command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_easu_layout, 0, 1,
		                             &easu_write);

		struct EasuConstants {
			float inv_src_size[2];
			float inv_dst_size[2];
		};
		static_assert(sizeof(EasuConstants) == 16);
		const EasuConstants easu_constants {{inv_src_x, inv_src_y}, {inv_dst_x, inv_dst_y}};
		command.pushConstants(m_easu_layout, vk::ShaderStageFlagBits::eFragment, 0,
		                      sizeof(easu_constants), &easu_constants);
		DrawFullscreenPass(command, m_easu_pipeline, m_intermediate_view, dst_extent);
	}

	// Pass 2 (both FSR and DLSS): RCAS (Robust Contrast-Adaptive Sharpening) -> Swapchain image
	TransitUpscalerImage(command, m_intermediate_image, vk::ImageLayout::eShaderReadOnlyOptimal,
	                     vk::PipelineStageFlagBits2::eFragmentShader,
	                     vk::AccessFlagBits2::eShaderRead);

	const vk::DescriptorImageInfo rcas_img {m_sampler, m_intermediate_view,
	                                        vk::ImageLayout::eShaderReadOnlyOptimal};
	vk::WriteDescriptorSet rcas_write {};
	rcas_write.dstBinding      = 0;
	rcas_write.descriptorCount = 1;
	rcas_write.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
	rcas_write.pImageInfo      = &rcas_img;
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_rcas_layout, 0, 1,
	                             &rcas_write);

	struct RcasConstants {
		float inv_size[2];
		float sharpness;
		float pad;
	};
	static_assert(sizeof(RcasConstants) == 16);
	const float normalized_sharpness =
	    static_cast<float>(std::min(sharpness_percent, 100u)) / 100.0f;
	const RcasConstants rcas_constants {{inv_dst_x, inv_dst_y}, normalized_sharpness, 0.0f};
	command.pushConstants(m_rcas_layout, vk::ShaderStageFlagBits::eFragment, 0,
	                      sizeof(rcas_constants), &rcas_constants);
	DrawFullscreenPass(command, m_rcas_pipeline, dst_view, dst_extent);
}

} // namespace Libs::Graphics
