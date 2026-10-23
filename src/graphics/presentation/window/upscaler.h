#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_UPSCALER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_UPSCALER_H_

#include "common/common.h"
#include "common/emulatorConfig.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

namespace Libs::Graphics {

class UpscalerPass final {
public:
	explicit UpscalerPass(GraphicContext& graphics);
	~UpscalerPass();
	KYTY_CLASS_NO_COPY(UpscalerPass);

	void InvalidateHistory() noexcept { m_history_valid = false; }
	void Release();

	[[nodiscard]] bool IsNativeNgxAvailable() const noexcept { return m_native_ngx_available; }

	// Upscales src_image / src_view (expected in eShaderReadOnlyOptimal) into dst_view (expected
	// in eColorAttachmentOptimal) using either FSR (EASU + RCAS) or DLSS (Temporal Super
	// Resolution + YCoCg Variance Clamping + RCAS).
	void Record(vk::CommandBuffer command, vk::Image src_image, vk::ImageView src_view,
	            vk::Extent2D src_extent, vk::Format src_format, vk::ImageView dst_view,
	            vk::Extent2D dst_extent, vk::Format dst_format, Config::UpscalerType mode,
	            uint32_t sharpness_percent);

private:
	void ProbeNativeNgxRuntime();
	void EnsurePipelines(vk::Format dst_format);
	void EnsureTargets(vk::Extent2D src_extent, vk::Format src_format, vk::Extent2D dst_extent,
	                   vk::Format dst_format);

	GraphicContext&         m_graphics;
	bool                    m_native_ngx_available = false;
	bool                    m_history_valid        = false;
	Config::UpscalerType    m_last_mode            = Config::UpscalerType::Off;
	vk::Extent2D            m_src_extent {};
	vk::Format              m_src_format           = vk::Format::eUndefined;
	vk::Extent2D            m_dst_extent {};
	vk::Format              m_dst_format           = vk::Format::eUndefined;
	vk::Format              m_pipeline_format      = vk::Format::eUndefined;

	VulkanImage             m_intermediate_image {};
	vk::ImageView           m_intermediate_view    = nullptr;
	VulkanImage             m_history_image {};
	vk::ImageView           m_history_view         = nullptr;
	VulkanImage             m_prev_input_image {};
	vk::ImageView           m_prev_input_view      = nullptr;

	vk::Sampler             m_sampler              = nullptr;
	vk::DescriptorSetLayout m_single_tex_desc      = nullptr;
	vk::DescriptorSetLayout m_tsr_tex_desc         = nullptr;
	vk::PipelineLayout      m_easu_layout          = nullptr;
	vk::PipelineLayout      m_rcas_layout          = nullptr;
	vk::PipelineLayout      m_tsr_layout           = nullptr;
	vk::Pipeline            m_easu_pipeline        = nullptr;
	vk::Pipeline            m_rcas_pipeline        = nullptr;
	vk::Pipeline            m_tsr_pipeline         = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_UPSCALER_H_
