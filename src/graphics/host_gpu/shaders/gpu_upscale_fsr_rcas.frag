#version 450

// AMD FidelityFX Super Resolution (FSR 1.0) Pass 2: Robust Contrast-Adaptive Sharpening (RCAS)
// Shared by both FSR and DLSS upscaler pipelines. Preserves SDR and HDR10 headroom without clipping.

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D src_tex;

layout(push_constant) uniform RcasConstants {
	vec2  inv_size;
	float sharpness; // 0.0 .. 1.0 (mapped from 0..100%)
	float pad;
};

float Luma(vec3 rgb) {
	return dot(rgb, vec3(0.299, 0.587, 0.114));
}

void main() {
	vec4 e = textureLod(src_tex, uv, 0.0);
	if (sharpness <= 0.001) {
		out_color = e;
		return;
	}

	vec3 b = textureLod(src_tex, uv + vec2( 0.0, -inv_size.y), 0.0).rgb;
	vec3 d = textureLod(src_tex, uv + vec2(-inv_size.x,  0.0), 0.0).rgb;
	vec3 f = textureLod(src_tex, uv + vec2( inv_size.x,  0.0), 0.0).rgb;
	vec3 h = textureLod(src_tex, uv + vec2( 0.0,  inv_size.y), 0.0).rgb;

	float lb = Luma(b);
	float ld = Luma(d);
	float le = Luma(e.rgb);
	float lf = Luma(f);
	float lh = Luma(h);

	float min_l = min(min(lb, ld), min(lf, lh));
	float max_l = max(max(lb, ld), max(lf, lh));
	float min_all = min(min_l, le);
	float max_all = max(max_l, le);

	// Contrast-adaptive lobe limit so bright HDR highlights and dark shadows never ring
	float range    = max_all - min_all;
	float peak     = max(max_all, 1.0);
	float contrast = clamp(1.0 - range / max(peak, 1e-4), 0.0, 1.0);

	// RCAS negative lobe weight in [-0.1875 * sharpness, 0.0]
	float w = -0.1875 * clamp(sharpness, 0.0, 1.0) * contrast * contrast;

	vec3 sharpened = (w * (b + d + f + h) + e.rgb) / max(1.0 + 4.0 * w, 1e-4);
	vec3 min4      = min(min(b, d), min(f, h));
	vec3 max4      = max(max(b, d), max(f, h));
	sharpened      = clamp(sharpened, min(min4, e.rgb), max(max4, e.rgb));

	out_color = vec4(sharpened, e.a);
}
