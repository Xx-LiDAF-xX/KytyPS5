#version 450 core

// Vulkan 1.3 Bidirectional Motion-Compensated Frame Interpolation (Frame Generation)
// Hardware-filtered 5-point Diamond Search (coarse + fine) + symmetric midpoint warp
// with scene-cut and disocclusion fallback.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_prev_tex;
layout(set = 0, binding = 1) uniform sampler2D u_curr_tex;

layout(push_constant) uniform Constants {
	vec2  inv_size;
	float blend_alpha;         // 0.5 for midpoint frame
	float scene_cut_threshold; // default 0.35
} pc;

float Luma(vec3 c) {
	return dot(c, vec3(0.299, 0.587, 0.114));
}

void main() {
	vec3 curr_c = texture(u_curr_tex, v_uv).rgb;
	vec3 prev_c = texture(u_prev_tex, v_uv).rgb;

	if (pc.blend_alpha <= 0.001) {
		out_color = vec4(curr_c, 1.0);
		return;
	}

	float lc  = Luma(curr_c);
	float l_n = Luma(texture(u_curr_tex, v_uv + vec2(0.0, -pc.inv_size.y)).rgb);
	float l_s = Luma(texture(u_curr_tex, v_uv + vec2(0.0,  pc.inv_size.y)).rgb);
	float l_w = Luma(texture(u_curr_tex, v_uv + vec2(-pc.inv_size.x, 0.0)).rgb);
	float l_e = Luma(texture(u_curr_tex, v_uv + vec2( pc.inv_size.x, 0.0)).rgb);
	float curr_grad = abs(l_e - l_w) + abs(l_s - l_n);

	// Zero-motion baseline cost using hardware bilinear samples
	float best_sad = abs(lc - Luma(prev_c));
	vec2  best_mv  = vec2(0.0);

	// Pass 1: Coarse 4-point diamond search (+/- 3 texels)
	const vec2 coarse_dirs[4] = vec2[4](
		vec2(-3.0,  0.0), vec2( 3.0,  0.0),
		vec2( 0.0, -3.0), vec2( 0.0,  3.0)
	);
	for (int i = 0; i < 4; ++i) {
		vec2 mv = coarse_dirs[i] * pc.inv_size;
		float lp_f = Luma(texture(u_prev_tex, v_uv - mv * 0.5).rgb);
		float lc_b = Luma(texture(u_curr_tex, v_uv + mv * 0.5).rgb);
		float sad  = abs(lp_f - lc_b) + 0.35 * abs(lc - Luma(texture(u_prev_tex, v_uv - mv).rgb));
		if (sad < best_sad) {
			best_sad = sad;
			best_mv  = mv;
		}
	}

	// Pass 2: Fine 4-point diamond refinement (+/- 1 texel around best coarse MV)
	const vec2 fine_dirs[4] = vec2[4](
		vec2(-1.0,  0.0), vec2(1.0, 0.0),
		vec2( 0.0, -1.0), vec2(0.0, 1.0)
	);
	vec2 base_mv = best_mv;
	for (int i = 0; i < 4; ++i) {
		vec2 mv = base_mv + fine_dirs[i] * pc.inv_size;
		float lp_f = Luma(texture(u_prev_tex, v_uv - mv * 0.5).rgb);
		float lc_b = Luma(texture(u_curr_tex, v_uv + mv * 0.5).rgb);
		float sad  = abs(lp_f - lc_b) + 0.35 * abs(lc - Luma(texture(u_prev_tex, v_uv - mv).rgb));
		if (sad < best_sad) {
			best_sad = sad;
			best_mv  = mv;
		}
	}

	// Bidirectional warp along estimated velocity
	vec2 uv_prev = clamp(v_uv - best_mv * pc.blend_alpha, vec2(0.0), vec2(1.0));
	vec2 uv_curr = clamp(v_uv + best_mv * (1.0 - pc.blend_alpha), vec2(0.0), vec2(1.0));

	vec3 warp_prev = texture(u_prev_tex, uv_prev).rgb;
	vec3 warp_curr = texture(u_curr_tex, uv_curr).rgb;

	// Photometric consistency / scene-cut & disocclusion check
	float luma_diff  = abs(Luma(warp_prev) - Luma(warp_curr));
	float confidence = 1.0 - smoothstep(pc.scene_cut_threshold * 0.45,
	                                    pc.scene_cut_threshold,
	                                    luma_diff / max(curr_grad * 0.5 + 0.15, 0.15));

	vec3  warped = mix(warp_prev, warp_curr, pc.blend_alpha);
	vec3  final_rgb = mix(curr_c, warped, confidence);

	out_color = vec4(final_rgb, 1.0);
}
