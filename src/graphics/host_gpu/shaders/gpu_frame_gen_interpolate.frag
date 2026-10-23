#version 450

// Motion-Compensated Frame Generation Shader (Midpoint Frame F_{t-0.5} Synthesis)
// Estimates bidirectional motion vectors between prev_tex (F_{t-1}) and curr_tex (F_t) using a
// coarse-to-fine search, warps both frames to t = 0.5, and applies a scene-cut / disocclusion
// fallback toward curr_tex so camera cuts and UI transitions never ghost or tear.

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D prev_tex;
layout(set = 0, binding = 1) uniform sampler2D curr_tex;

layout(push_constant) uniform FrameGenConstants {
	vec2  inv_size;
	float blend_alpha;         // nominal 0.5 midpoint weight (0.0 disables interpolation)
	float scene_cut_threshold; // nominal 0.35 photometric discrepancy threshold
};

float Luma(vec3 rgb) {
	return dot(rgb, vec3(0.299, 0.587, 0.114));
}

float SymmetricCost(vec2 center_uv, vec2 half_mv) {
	vec3 p0 = textureLod(prev_tex, center_uv - half_mv, 0.0).rgb;
	vec3 c0 = textureLod(curr_tex, center_uv + half_mv, 0.0).rgb;
	vec3 p1 = textureLod(prev_tex, center_uv - half_mv + vec2(inv_size.x, 0.0), 0.0).rgb;
	vec3 c1 = textureLod(curr_tex, center_uv + half_mv + vec2(inv_size.x, 0.0), 0.0).rgb;
	vec3 p2 = textureLod(prev_tex, center_uv - half_mv + vec2(0.0, inv_size.y), 0.0).rgb;
	vec3 c2 = textureLod(curr_tex, center_uv + half_mv + vec2(0.0, inv_size.y), 0.0).rgb;

	float diff0 = abs(Luma(p0) - Luma(c0));
	float diff1 = abs(Luma(p1) - Luma(c1));
	float diff2 = abs(Luma(p2) - Luma(c2));
	return (diff0 * 2.0 + diff1 + diff2) * 0.25;
}

void main() {
	vec4 curr_center = textureLod(curr_tex, uv, 0.0);
	if (blend_alpha <= 0.001) {
		out_color = curr_center;
		return;
	}

	vec4  prev_center = textureLod(prev_tex, uv, 0.0);
	float zero_diff   = abs(Luma(curr_center.rgb) - Luma(prev_center.rgb));
	if (zero_diff < 0.004) {
		out_color = mix(prev_center, curr_center, blend_alpha);
		return;
	}

	// Stage 1: Coarse 5x5 search (stride = 2 texels) for half-motion vector
	vec2  best_half_mv = vec2(0.0);
	float best_cost    = SymmetricCost(uv, vec2(0.0));

	for (int dy = -2; dy <= 2; ++dy) {
		for (int dx = -2; dx <= 2; ++dx) {
			if (dx == 0 && dy == 0) {
				continue;
			}
			vec2  cand_half_mv = vec2(float(dx * 2), float(dy * 2)) * inv_size;
			float cost         = SymmetricCost(uv, cand_half_mv) +
			                     0.0015 * float(abs(dx) + abs(dy));
			if (cost < best_cost) {
				best_cost    = cost;
				best_half_mv = cand_half_mv;
			}
		}
	}

	// Stage 2: Fine 3x3 sub-texel refinement around coarse winner
	vec2 coarse_winner = best_half_mv;
	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			if (dx == 0 && dy == 0) {
				continue;
			}
			vec2  cand_half_mv = coarse_winner + vec2(float(dx), float(dy)) * (0.5 * inv_size);
			float cost         = SymmetricCost(uv, cand_half_mv) +
			                     0.0008 * float(abs(dx) + abs(dy));
			if (cost < best_cost) {
				best_cost    = cost;
				best_half_mv = cand_half_mv;
			}
		}
	}

	vec2 uv_prev = clamp(uv - best_half_mv, vec2(0.0), vec2(1.0));
	vec2 uv_curr = clamp(uv + best_half_mv, vec2(0.0), vec2(1.0));

	vec4 warped_prev = textureLod(prev_tex, uv_prev, 0.0);
	vec4 warped_curr = textureLod(curr_tex, uv_curr, 0.0);

	// Scene-cut and disocclusion guard: attenuate toward unwarped curr_center when photometric
	// error is high so camera cuts or fast disocclusions never produce ghost silhouettes.
	float thresh     = max(scene_cut_threshold, 0.05);
	float confidence = 1.0 - smoothstep(thresh * 0.45, thresh, best_cost);

	vec4  warped_mid = mix(warped_prev, warped_curr, clamp(blend_alpha, 0.0, 1.0));
	out_color        = mix(curr_center, warped_mid, confidence);
}
