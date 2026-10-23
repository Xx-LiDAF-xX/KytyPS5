#include "graphics/presentation/window/frameGen.h"

#include "common/assert.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_frame_gen_interpolate_spv.h"

#include <array>
#include <span>

namespace Libs::Graphics {

namespace {

vk::Pipeline CreateFrameGenPipeline(vk::Device device, vk::PipelineLayout layout,
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
	                     "create frame-gen graphics pipeline");
	device.destroyShaderModule(fragment, nullptr);
	device.destroyShaderModule(vertex, nullptr);
	return pipeline;
}

void TransitImage(vk::CommandBuffer command, VulkanImage& img, vk::ImageLayout new_layout,
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

} // namespace

FrameGenPass::FrameGenPass(GraphicContext& graphics): m_graphics(graphics) {}

FrameGenPass::~FrameGenPass() {
	Release();
}

void FrameGenPass::Reset() noexcept {
	m_has_prev = false;
}

void FrameGenPass::Release() {
	auto device = m_graphics.device;
	if (device == nullptr) {
		return;
	}
	if (m_prev_view != nullptr) {
		device.destroyImageView(m_prev_view, nullptr);
		m_prev_view = nullptr;
	}
	if (m_prev_image.image != nullptr) {
		m_graphics.DeleteImage(m_prev_image);
		m_prev_image.image = nullptr;
	}
	if (m_interp_view != nullptr) {
		device.destroyImageView(m_interp_view, nullptr);
		m_interp_view = nullptr;
	}
	if (m_interp_image.image != nullptr) {
		m_graphics.DeleteImage(m_interp_image);
		m_interp_image.image = nullptr;
	}
	if (m_pipeline != nullptr) {
		device.destroyPipeline(m_pipeline, nullptr);
		m_pipeline = nullptr;
	}
	if (m_layout != nullptr) {
		device.destroyPipelineLayout(m_layout, nullptr);
		m_layout = nullptr;
	}
	if (m_descriptors != nullptr) {
		device.destroyDescriptorSetLayout(m_descriptors, nullptr);
		m_descriptors = nullptr;
	}
	if (m_sampler != nullptr) {
		device.destroySampler(m_sampler, nullptr);
		m_sampler = nullptr;
	}
	m_extent      = {};
	m_format      = vk::Format::eUndefined;
	m_pipe_format = vk::Format::eUndefined;
	m_has_prev    = false;
}

void FrameGenPass::EnsureResources(vk::Extent2D extent, vk::Format format) {
	auto device = m_graphics.device;
	if (m_layout == nullptr) {
		vk::SamplerCreateInfo sampler {};
		sampler.magFilter    = vk::Filter::eLinear;
		sampler.minFilter    = vk::Filter::eLinear;
		sampler.mipmapMode   = vk::SamplerMipmapMode::eNearest;
		sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		RequireVulkanSuccess(device.createSampler(&sampler, nullptr, &m_sampler),
		                     "create frame-gen sampler");

		const std::array<vk::DescriptorSetLayoutBinding, 2> bindings {{
		    {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
		    {1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
		}};
		vk::DescriptorSetLayoutCreateInfo desc_info {};
		desc_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		desc_info.bindingCount = static_cast<uint32_t>(bindings.size());
		desc_info.pBindings    = bindings.data();
		RequireVulkanSuccess(device.createDescriptorSetLayout(&desc_info, nullptr, &m_descriptors),
		                     "create frame-gen descriptor layout");

		const vk::PushConstantRange push_range {vk::ShaderStageFlagBits::eFragment, 0, 16};
		vk::PipelineLayoutCreateInfo layout_info {};
		layout_info.setLayoutCount         = 1;
		layout_info.pSetLayouts            = &m_descriptors;
		layout_info.pushConstantRangeCount = 1;
		layout_info.pPushConstantRanges    = &push_range;
		RequireVulkanSuccess(device.createPipelineLayout(&layout_info, nullptr, &m_layout),
		                     "create frame-gen pipeline layout");
	}

	if (m_pipeline != nullptr && m_pipe_format != format) {
		device.destroyPipeline(m_pipeline, nullptr);
		m_pipeline = nullptr;
	}
	if (m_pipeline == nullptr) {
		m_pipeline    = CreateFrameGenPipeline(device, m_layout, GPU_FRAME_GEN_INTERPOLATE_SPV, format);
		m_pipe_format = format;
	}

	if (m_prev_image.image != nullptr && m_extent.width == extent.width &&
	    m_extent.height == extent.height && m_format == format) {
		return;
	}

	if (m_prev_view != nullptr) {
		device.destroyImageView(m_prev_view, nullptr);
		m_prev_view = nullptr;
	}
	if (m_prev_image.image != nullptr) {
		m_graphics.DeleteImage(m_prev_image);
		m_prev_image.image = nullptr;
	}
	if (m_interp_view != nullptr) {
		device.destroyImageView(m_interp_view, nullptr);
		m_interp_view = nullptr;
	}
	if (m_interp_image.image != nullptr) {
		m_graphics.DeleteImage(m_interp_image);
		m_interp_image.image = nullptr;
	}

	vk::ImageCreateInfo img_info {};
	img_info.imageType     = vk::ImageType::e2D;
	img_info.extent        = {extent.width, extent.height, 1};
	img_info.mipLevels     = 1;
	img_info.arrayLayers   = 1;
	img_info.format        = format;
	img_info.tiling        = vk::ImageTiling::eOptimal;
	img_info.initialLayout = vk::ImageLayout::eUndefined;
	img_info.usage         = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
	                 vk::ImageUsageFlagBits::eTransferSrc |
	                 vk::ImageUsageFlagBits::eColorAttachment;
	img_info.sharingMode = vk::SharingMode::eExclusive;
	img_info.samples     = vk::SampleCountFlagBits::e1;

	EXIT_IF(!m_graphics.CreateImage(img_info, m_prev_image));
	EXIT_IF(!m_graphics.CreateImage(img_info, m_interp_image));

	vk::ImageViewCreateInfo view_info {};
	view_info.viewType         = vk::ImageViewType::e2D;
	view_info.format           = format;
	view_info.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

	view_info.image = m_prev_image.image;
	RequireVulkanSuccess(device.createImageView(&view_info, nullptr, &m_prev_view),
	                     "create frame-gen prev view");
	view_info.image = m_interp_image.image;
	RequireVulkanSuccess(device.createImageView(&view_info, nullptr, &m_interp_view),
	                     "create frame-gen interp view");

	m_extent   = extent;
	m_format   = format;
	m_has_prev = false;
}

void FrameGenPass::SynthesizeMidpoint(vk::CommandBuffer command, vk::ImageView curr_view,
                                      vk::Extent2D extent, vk::Format format) {
	EnsureResources(extent, format);
	EXIT_IF(!m_has_prev);

	TransitImage(command, m_interp_image, vk::ImageLayout::eColorAttachmentOptimal,
	             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
	             vk::AccessFlagBits2::eColorAttachmentWrite);

	const std::array<vk::DescriptorImageInfo, 2> image_infos {{
	    {m_sampler, m_prev_view, vk::ImageLayout::eShaderReadOnlyOptimal},
	    {m_sampler, curr_view, vk::ImageLayout::eShaderReadOnlyOptimal},
	}};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	writes[0].dstBinding      = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType  = vk::DescriptorType::eCombinedImageSampler;
	writes[0].pImageInfo      = &image_infos[0];
	writes[1].dstBinding      = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType  = vk::DescriptorType::eCombinedImageSampler;
	writes[1].pImageInfo      = &image_infos[1];
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_layout, 0,
	                             static_cast<uint32_t>(writes.size()), writes.data());

	struct Constants {
		float inv_size[2];
		float blend_alpha;
		float scene_cut_threshold;
	};
	static_assert(sizeof(Constants) == 16);
	const Constants constants {
	    {1.0f / static_cast<float>( std::max(extent.width, 1u)),
	     1.0f / static_cast<float>(std::max(extent.height, 1u))},
	    ComputeFrameGenBlendWeight(0.0f, 0.35f, m_has_prev),
	    0.35f,
	};
	command.pushConstants(m_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(constants),
	                      &constants);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline);

	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = m_interp_view;
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp      = vk::AttachmentLoadOp::eDontCare;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = extent;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	command.beginRendering(&rendering);

	const vk::Viewport viewport {
	    0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
	const vk::Rect2D scissor {{0, 0}, extent};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.draw(3, 1, 0, 0);
	command.endRendering();
}

void FrameGenPass::UpdateHistory(vk::CommandBuffer command, vk::Image curr_image,
                                 vk::Extent2D extent, vk::Format format) {
	EnsureResources(extent, format);

	TransitImage(command, m_prev_image, vk::ImageLayout::eTransferDstOptimal,
	             vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);

	vk::ImageCopy copy {};
	copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.extent         = {extent.width, extent.height, 1};
	command.copyImage(curr_image, vk::ImageLayout::eTransferSrcOptimal, m_prev_image.image,
	                  vk::ImageLayout::eTransferDstOptimal, 1, &copy);

	TransitImage(command, m_prev_image, vk::ImageLayout::eShaderReadOnlyOptimal,
	             vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);
	m_has_prev = true;
}

} // namespace Libs::Graphics
