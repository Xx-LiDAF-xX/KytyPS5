#version 450

// Universal DLSS / Temporal Super Resolution (TSR) Reconstruction Pass
// Performs 5-tap Catmull-Rom spatial reconstruction, local screen-space motion vector estimation
// between curr_tex and prev_input_tex, motion-compensated history reprojection, and YCoCg 3x3
// variance neighborhood clamping (gamma = 1.25) to eliminate ghosting across disocclusions and cuts.

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D curr_tex;
layout(set = 0, binding = 1) uniform sampler2D history_tex;
layout(set = 0, binding = 2) uniform sampler2D prev_input_tex;

layout(push_constant) uniform DlssTsrConstants {
	vec2  inv_src_size;
	vec2  inv_dst_size;
	float history_weight; // nominal temporal blend weight, e.g. 0.20
	float reset_history;  // > 0.5 forces 100% current frame
};

float Luma(vec3 rgb) {
	return dot(rgb, vec3(0.299, 0.587, 0.114));
}

vec3 RgbToYCoCg(vec3 c) {
	return vec3(
	    0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
	    0.5  * c.r             - 0.5  * c.b,
	   -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 YCoCgToRgb(vec3 y) {
	float tmp = y.x - y.z;
	return vec3(tmp + y.y, y.x + y.z, tmp - y.y);
}

vec4 SampleCatmullRom5Tap(sampler2D tex, vec2 sample_uv, vec2 inv_tex_size) {
	vec2 tex_size = 1.0 / max(inv_tex_size, vec2(1e-6));
	vec2 pos      = sample_uv * tex_size;
	vec2 center   = floor(pos - 0.5) + 0.5;
	vec2 f        = pos - center;
	vec2 f2       = f * f;
	vec2 f3       = f2 * f;

	vec2 w0  = -0.5 * f3 + f2 - 0.5 * f;
	vec2 w1  =  1.5 * f3 - 2.5 * f2 + 1.0;
	vec2 w2  = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
	vec2 w3  =  0.5 * f3 - 0.5 * f2;
	vec2 w12 = w1 + w2;
	vec2 ofs = w2 / max(w12, vec2(1e-5));

	vec2 tc0  = (center - 1.0) * inv_tex_size;
	vec2 tc3  = (center + 2.0) * inv_tex_size;
	vec2 tc12 = (center + ofs) * inv_tex_size;

	float s0 = w12.x * w0.y;
	float s1 = w0.x  * w12.y;
	float s2 = w12.x * w12.y;
	float s3 = w3.x  * w12.y;
	float s4 = w12.x * w3.y;
	float ws = s0 + s1 + s2 + s3 + s4;

	vec4 c = textureLod(tex, vec2(tc12.x, tc0.y),  0.0) * s0 +
	         textureLod(tex, vec2(tc0.x,  tc12.y), 0.0) * s1 +
	         textureLod(tex, vec2(tc12.x, tc12.y), 0.0) * s2 +
	         textureLod(tex, vec2(tc3.x,  tc12.y), 0.0) * s3 +
	         textureLod(tex, vec2(tc12.x, tc3.y),  0.0) * s4;
	return c / max(ws, 1e-5);
}

void main() {
	vec4 curr_sample = SampleCatmullRom5Tap(curr_tex, uv, inv_src_size);

	// Gather 3x3 neighborhood in YCoCg space for variance clipping & anti-ringing
	vec3 m1    = vec3(0.0);
	vec3 m2    = vec3(0.0);
	vec3 c_min = vec3(1e9);
	vec3 c_max = vec3(-1e9);

	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			vec2 tap_uv = uv + vec2(float(dx), float(dy)) * inv_src_size;
			vec3 rgb    = textureLod(curr_tex, tap_uv, 0.0).rgb;
			c_min       = min(c_min, rgb);
			c_max       = max(c_max, rgb);
			vec3 ycocg  = RgbToYCoCg(rgb);
			m1 += ycocg;
			m2 += ycocg * ycocg;
		}
	}

	// Clamp Catmull-Rom negative lobe overshoot to 3x3 RGB box
	vec3 curr_rgb = clamp(curr_sample.rgb, c_min, c_max);
	if (reset_history > 0.5 || history_weight <= 0.001) {
		out_color = vec4(curr_rgb, curr_sample.a);
		return;
	}

	// Estimate local screen-space motion vector between curr_tex and prev_input_tex
	float curr_luma = Luma(curr_rgb);
	vec2  best_mv   = vec2(0.0);
	float best_sad  = abs(curr_luma - Luma(textureLod(prev_input_tex, uv, 0.0).rgb));

	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			if (dx == 0 && dy == 0) {
				continue;
			}
			vec2  cand_mv  = vec2(float(dx), float(dy)) * inv_src_size;
			float prev_l   = Luma(textureLod(prev_input_tex, uv - cand_mv, 0.0).rgb);
			float cand_sad = abs(curr_luma - prev_l) + 0.002 * float(abs(dx) + abs(dy));
			if (cand_sad < best_sad) {
				best_sad = cand_sad;
				best_mv  = cand_mv;
			}
		}
	}

	vec2 reproj_uv = clamp(uv - best_mv, vec2(0.0), vec2(1.0));
	vec3 hist_rgb  = SampleCatmullRom5Tap(history_tex, reproj_uv, inv_dst_size).rgb;
	vec3 hist_ycg  = RgbToYCoCg(hist_rgb);

	// YCoCg variance clipping (gamma = 1.25) eliminates ghosting on moving edges
	vec3 mu    = m1 * (1.0 / 9.0);
	vec3 sigma = sqrt(max(m2 * (1.0 / 9.0) - mu * mu, vec3(0.0)));
	const float kGamma = 1.25;
	vec3 clamped_ycg = clamp(hist_ycg, mu - kGamma * sigma, mu + kGamma * sigma);
	vec3 clamped_hist = max(YCoCgToRgb(clamped_ycg), vec3(0.0));

	// Attenuate history weight on large photometric changes (scene cut / disocclusion guard)
	float confidence = 1.0 - smoothstep(0.08, 0.30, best_sad);
	float blend_w    = clamp(history_weight * confidence, 0.0, 0.35);

	vec3 final_rgb = mix(curr_rgb, clamped_hist, blend_w);
	out_color      = vec4(final_rgb, curr_sample.a);
}
