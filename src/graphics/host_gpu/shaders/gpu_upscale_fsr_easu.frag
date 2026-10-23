#version 450

// AMD FidelityFX Super Resolution (FSR 1.0) Pass 1: Edge-Adaptive Spatial Upsampling (EASU)
// 12-tap anisotropic directional Lanczos2 reconstruction with anti-ringing neighborhood clamp.
// Compatible with all Vulkan 1.3 GPUs (NVIDIA RTX/GTX, AMD Radeon RX, Intel Arc/iGPU) in SDR & HDR10.

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D src_tex;

layout(push_constant) uniform EasuConstants {
	vec2 inv_src_size;
	vec2 inv_dst_size;
};

float Luma(vec3 rgb) {
	return dot(rgb, vec3(0.299, 0.587, 0.114));
}

void AccumulateDir(inout vec2 dir, inout float len, vec2 offset, float lC, float lW, float lE, float lN, float lS) {
	float gx  = lE - lW;
	float gy  = lS - lN;
	float g2  = gx * gx + gy * gy;
	float inv = inversesqrt(max(g2, 1e-6));
	vec2  d   = vec2(gx, gy) * inv;
	float w   = clamp(1.0 - abs(offset.x), 0.0, 1.0) * clamp(1.0 - abs(offset.y), 0.0, 1.0);
	dir += d * w;
	float luma_range = max(max(max(lW, lE), max(lN, lS)), lC) - min(min(min(lW, lE), min(lN, lS)), lC);
	len += clamp(sqrt(g2) / max(luma_range, 1e-4), 0.0, 1.0) * w;
}

float EasuTapWeight(vec2 delta, vec2 dir, vec2 stretch, float lobe) {
	// Rotate delta into local edge frame (along edge vs across edge)
	vec2 rotated = vec2(
	    delta.x * dir.y - delta.y * dir.x,
	    delta.x * dir.x + delta.y * dir.y) * stretch;
	float d2   = min(dot(rotated, rotated), (2.0 - lobe) * (2.0 - lobe));
	float wB   = 0.4 * d2 - 1.0;
	float wA   = lobe * d2 - 1.0;
	wB *= wB;
	wA *= wA;
	wB = 1.5625 * wB - 0.5625;
	return wB * wA;
}

void main() {
	vec2 src_size = 1.0 / max(inv_src_size, vec2(1e-6));
	vec2 tex_pos  = uv * src_size - vec2(0.5);
	vec2 base_idx = floor(tex_pos);
	vec2 frac_pos = tex_pos - base_idx;
	vec2 base_uv  = (base_idx + vec2(0.5)) * inv_src_size;

	// Fetch 12-tap cross-plus-corners neighborhood:
	//       b   c
	//   e   f   g   h
	//   i   j   k   l
	//       n   o
	vec4 b = textureLod(src_tex, base_uv + vec2( 0.0, -1.0) * inv_src_size, 0.0);
	vec4 c = textureLod(src_tex, base_uv + vec2( 1.0, -1.0) * inv_src_size, 0.0);
	vec4 e = textureLod(src_tex, base_uv + vec2(-1.0,  0.0) * inv_src_size, 0.0);
	vec4 f = textureLod(src_tex, base_uv + vec2( 0.0,  0.0) * inv_src_size, 0.0);
	vec4 g = textureLod(src_tex, base_uv + vec2( 1.0,  0.0) * inv_src_size, 0.0);
	vec4 h = textureLod(src_tex, base_uv + vec2( 2.0,  0.0) * inv_src_size, 0.0);
	vec4 i = textureLod(src_tex, base_uv + vec2(-1.0,  1.0) * inv_src_size, 0.0);
	vec4 j = textureLod(src_tex, base_uv + vec2( 0.0,  1.0) * inv_src_size, 0.0);
	vec4 k = textureLod(src_tex, base_uv + vec2( 1.0,  1.0) * inv_src_size, 0.0);
	vec4 l = textureLod(src_tex, base_uv + vec2( 2.0,  1.0) * inv_src_size, 0.0);
	vec4 n = textureLod(src_tex, base_uv + vec2( 0.0,  2.0) * inv_src_size, 0.0);
	vec4 o = textureLod(src_tex, base_uv + vec2( 1.0,  2.0) * inv_src_size, 0.0);

	float lb = Luma(b.rgb), lc = Luma(c.rgb);
	float le = Luma(e.rgb), lf = Luma(f.rgb), lg = Luma(g.rgb), lh = Luma(h.rgb);
	float li = Luma(i.rgb), lj = Luma(j.rgb), lk = Luma(k.rgb), ll = Luma(l.rgb);
	float ln = Luma(n.rgb), lo = Luma(o.rgb);

	vec2  dir = vec2(0.0);
	float len = 0.0;
	AccumulateDir(dir, len, frac_pos - vec2(0.0, 0.0), lf, le, lg, lb, lj);
	AccumulateDir(dir, len, frac_pos - vec2(1.0, 0.0), lg, lf, lh, lc, lk);
	AccumulateDir(dir, len, frac_pos - vec2(0.0, 1.0), lj, li, lk, lf, ln);
	AccumulateDir(dir, len, frac_pos - vec2(1.0, 1.0), lk, lj, ll, lg, lo);

	float dir_sq = dot(dir, dir);
	if (dir_sq < 1e-5) {
		dir = vec2(1.0, 0.0);
	} else {
		dir *= inversesqrt(dir_sq);
	}

	len = clamp(len * 0.5, 0.0, 1.0);
	len *= len;
	float stretch_along = 1.0 / (1.0 + 0.45 * len);
	float stretch_perp  = 1.0 + 0.25 * len;
	vec2  stretch       = vec2(stretch_along, stretch_perp);
	float lobe          = 0.5 - 0.24 * len;

	float wb = EasuTapWeight(vec2( 0.0, -1.0) - frac_pos, dir, stretch, lobe);
	float wc = EasuTapWeight(vec2( 1.0, -1.0) - frac_pos, dir, stretch, lobe);
	float we = EasuTapWeight(vec2(-1.0,  0.0) - frac_pos, dir, stretch, lobe);
	float wf = EasuTapWeight(vec2( 0.0,  0.0) - frac_pos, dir, stretch, lobe);
	float wg = EasuTapWeight(vec2( 1.0,  0.0) - frac_pos, dir, stretch, lobe);
	float wh = EasuTapWeight(vec2( 2.0,  0.0) - frac_pos, dir, stretch, lobe);
	float wi = EasuTapWeight(vec2(-1.0,  1.0) - frac_pos, dir, stretch, lobe);
	float wj = EasuTapWeight(vec2( 0.0,  1.0) - frac_pos, dir, stretch, lobe);
	float wk = EasuTapWeight(vec2( 1.0,  1.0) - frac_pos, dir, stretch, lobe);
	float wl = EasuTapWeight(vec2( 2.0,  1.0) - frac_pos, dir, stretch, lobe);
	float wn = EasuTapWeight(vec2( 0.0,  2.0) - frac_pos, dir, stretch, lobe);
	float wo = EasuTapWeight(vec2( 1.0,  2.0) - frac_pos, dir, stretch, lobe);

	vec3 accum = b.rgb * wb + c.rgb * wc +
	             e.rgb * we + f.rgb * wf + g.rgb * wg + h.rgb * wh +
	             i.rgb * wi + j.rgb * wj + k.rgb * wk + l.rgb * wl +
	             n.rgb * wn + o.rgb * wo;
	float w_sum = wb + wc + we + wf + wg + wh + wi + wj + wk + wl + wn + wo;
	vec3  rgb   = accum / max(w_sum, 1e-5);

	// Anti-ringing neighborhood box clamp using the inner 2x2 quad (f, g, j, k)
	vec3 min4 = min(min(f.rgb, g.rgb), min(j.rgb, k.rgb));
	vec3 max4 = max(max(f.rgb, g.rgb), max(j.rgb, k.rgb));
	rgb       = clamp(rgb, min4, max4);

	float alpha = mix(mix(f.a, g.a, frac_pos.x), mix(j.a, k.a, frac_pos.x), frac_pos.y);
	out_color   = vec4(rgb, alpha);
}
