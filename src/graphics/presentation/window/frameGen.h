#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_FRAMEGEN_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_FRAMEGEN_H_

#include "common/common.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <cmath>

namespace Window {

// Pure scene-cut / disocclusion attenuation function for Frame Generation midpoint blending.
// Returns 0.0f when history is invalid or when photometric luma difference exceeds threshold,
// 0.5f when luma difference is small, and smoothly attenuates near the scene-cut threshold.
inline float ComputeFrameGenBlendWeight(float luma_diff, float scene_cut_threshold,
                                        bool history_valid) noexcept {
	if (!history_valid) {
		return 0.0f;
	}
	const float thresh = std::max(scene_cut_threshold, 0.05f);
	const float diff   = std::abs(luma_diff);
	if (diff >= thresh) {
		return 0.0f;
	}
	const float ramp_start = thresh * 0.45f;
	if (diff <= ramp_start) {
		return 0.5f;
	}
	const float t          = (diff - ramp_start) / std::max(thresh - ramp_start, 1e-5f);
	const float smoothstep = t * t * (3.0f - 2.0f * t);
	return 0.5f * (1.0f - smoothstep);
}

} // namespace Window

namespace Libs::Graphics {

using Window::ComputeFrameGenBlendWeight;

class FrameGenPass final {
public:
	explicit FrameGenPass(GraphicContext& graphics);
	~FrameGenPass();
	KYTY_CLASS_NO_COPY(FrameGenPass);

	void Reset() noexcept;
	void Release();

	[[nodiscard]] bool HasValidHistory(vk::Extent2D extent, vk::Format format) const noexcept {
		return m_has_prev && m_extent.width == extent.width && m_extent.height == extent.height &&
		       m_format == format;
	}

	// Synthesizes the t = 0.5 midpoint frame between m_prev_view and curr_view into m_interp_image.
	// Expects curr_view's image to be in eShaderReadOnlyOptimal.
	void SynthesizeMidpoint(vk::CommandBuffer command, vk::ImageView curr_view,
	                        vk::Extent2D extent, vk::Format format);

	// Copies curr_image (expected in eTransferSrcOptimal) into m_prev_image and transitions
	// m_prev_image to eShaderReadOnlyOptimal for the next flip.
	void UpdateHistory(vk::CommandBuffer command, vk::Image curr_image, vk::Extent2D extent,
	                   vk::Format format);

	[[nodiscard]] VulkanImage&   GetInterpolatedImage() noexcept { return m_interp_image; }
	[[nodiscard]] vk::ImageView  GetInterpolatedView() const noexcept { return m_interp_view; }

private:
	void EnsureResources(vk::Extent2D extent, vk::Format format);

	GraphicContext&         m_graphics;
	vk::Extent2D            m_extent {};
	vk::Format              m_format      = vk::Format::eUndefined;
	vk::Format              m_pipe_format = vk::Format::eUndefined;
	bool                    m_has_prev    = false;
	VulkanImage             m_prev_image {};
	vk::ImageView           m_prev_view   = nullptr;
	VulkanImage             m_interp_image {};
	vk::ImageView           m_interp_view = nullptr;
	vk::Sampler             m_sampler     = nullptr;
	vk::DescriptorSetLayout m_descriptors = nullptr;
	vk::PipelineLayout      m_layout      = nullptr;
	vk::Pipeline            m_pipeline    = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_FRAMEGEN_H_
