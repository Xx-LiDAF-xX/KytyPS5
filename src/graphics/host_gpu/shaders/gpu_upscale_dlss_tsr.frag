#version 450 core

// Vulkan 1.3 Temporal Super Resolution (DLSS Temporal Reconstruction + YCoCg Clamping)
// Combines 5-tap Catmull-Rom spatial reconstruction, fast 5-point diamond screen-space motion
// estimation, and 3x3 YCoCg neighborhood variance clipping to eliminate ghosting.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_curr_input;
layout(set = 0, binding = 1) uniform sampler2D u_history_output;
layout(set = 0, binding = 2) uniform sampler2D u_prev_input;

layout(push_constant) uniform Constants {
	vec2  inv_src_size;
	vec2  inv_dst_size;
	float history_weight; // default 0.20 (80% history, 20% current)
	float reset_history;  // 1.0 on first frame / resize / mode switch, 0.0 otherwise
} pc;

vec3 RgbToYCoCg(vec3 c) {
	return vec3(
		 0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
		 0.50 * c.r             - 0.50 * c.b,
		-0.25 * c.r + 0.5 * c.g - 0.25 * c.b
	);
}

vec3 YCoCgToRgb(vec3 y) {
	float tmp = y.x - y.z;
	return vec3(tmp + y.y, y.x + y.z, tmp - y.y);
}

// 5-tap Catmull-Rom bicubic filter using hardware bilinear taps
vec3 SampleCatmullRom(sampler2D tex, vec2 uv, vec2 inv_size) {
	vec2 tex_size = 1.0 / max(inv_size, vec2(1e-6));
	vec2 pos = uv * tex_size;
	vec2 center = floor(pos - 0.5) + 0.5;
	vec2 f = pos - center;
	vec2 f2 = f * f;
	vec2 f3 = f2 * f;

	vec2 w0 = f2 - 0.5 * (f3 + f);
	vec2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
	vec2 w3 = 0.5 * (f3 - f2);
	vec2 w2 = 1.0 - w0 - w1 - w3;

	vec2 w12 = w1 + w2;
	vec2 tc0 = (center - 1.0) * inv_size;
	vec2 tc3 = (center + 2.0) * inv_size;
	vec2 tc12 = (center + w2 / max(w12, vec2(1e-5))) * inv_size;

	vec3 s0 = texture(tex, vec2(tc12.x, tc0.y)).rgb * (w12.x * w0.y);
	vec3 s1 = texture(tex, vec2(tc0.x,  tc12.y)).rgb * (w0.x  * w12.y);
	vec3 s2 = texture(tex, vec2(tc12.x, tc12.y)).rgb * (w12.x * w12.y);
	vec3 s3 = texture(tex, vec2(tc3.x,  tc12.y)).rgb * (w3.x  * w12.y);
	vec3 s4 = texture(tex, vec2(tc12.x, tc3.y)).rgb * (w12.x * w3.y);

	float total_w = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) + (w3.x * w12.y) + (w12.x * w3.y);
	return max(vec3(0.0), (s0 + s1 + s2 + s3 + s4) / max(total_w, 1e-5));
}

void main() {
	vec3 curr_spatial = SampleCatmullRom(u_curr_input, v_uv, pc.inv_src_size);

	if (pc.reset_history > 0.5) {
		out_color = vec4(curr_spatial, 1.0);
		return;
	}

	// Compute 3x3 YCoCg moments (mean & std-dev) in source texel neighborhood
	vec3 m1 = vec3(0.0);
	vec3 m2 = vec3(0.0);
	vec3 c_min = vec3( 1e9);
	vec3 c_max = vec3(-1e9);
	float center_y = 0.0;

	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			vec2 off = vec2(float(dx), float(dy)) * pc.inv_src_size;
			vec3 s = RgbToYCoCg(texture(u_curr_input, v_uv + off).rgb);
			if (dx == 0 && dy == 0) {
				center_y = s.x;
			}
			m1 += s;
			m2 += s * s;
			c_min = min(c_min, s);
			c_max = max(c_max, s);
		}
	}

	vec3 mean = m1 / 9.0;
	vec3 stddev = sqrt(max(vec3(0.0), (m2 / 9.0) - mean * mean));

	// Fast 5-point Diamond motion vector search in source space using hardware bilinear samples
	float best_diff = abs(center_y - RgbToYCoCg(texture(u_prev_input, v_uv).rgb).x);
	vec2  best_mv   = vec2(0.0);
	const vec2 search_dirs[4] = vec2[4](
		vec2(-1.5,  0.0), vec2(1.5, 0.0),
		vec2( 0.0, -1.5), vec2(0.0, 1.5)
	);
	for (int i = 0; i < 4; ++i) {
		vec2 mv = search_dirs[i] * pc.inv_src_size;
		float prev_y = RgbToYCoCg(texture(u_prev_input, v_uv - mv).rgb).x;
		float diff   = abs(center_y - prev_y);
		if (diff < best_diff) {
			best_diff = diff;
			best_mv   = mv;
		}
	}

	// Reproject high-resolution history along best_mv and clamp to YCoCg variance box
	vec2 hist_uv = clamp(v_uv - best_mv, vec2(0.0), vec2(1.0));
	vec3 hist_ycocg = RgbToYCoCg(texture(u_history_output, hist_uv).rgb);

	const float gamma = 1.25;
	vec3 box_min = max(c_min, mean - gamma * stddev);
	vec3 box_max = min(c_max, mean + gamma * stddev);
	vec3 clamped_hist_ycocg = clamp(hist_ycocg, box_min, box_max);

	// Increase current-frame weight when motion residual is high (anti-ghosting on disocclusions)
	float motion_Reject = smoothstep(0.05, 0.25, best_diff);
	float alpha = mix(clamp(pc.history_weight, 0.05, 1.0), 0.85, motion_Reject);

	vec3 curr_ycocg = RgbToYCoCg(curr_spatial);
	vec3 blended_ycocg = mix(clamped_hist_ycocg, curr_ycocg, alpha);
	vec3 final_rgb = max(vec3(0.0), YCoCgToRgb(blended_ycocg));

	out_color = vec4(final_rgb, 1.0);
}
