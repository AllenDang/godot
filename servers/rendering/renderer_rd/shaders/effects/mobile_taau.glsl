#[compute]

#version 450

#VERSION_DEFINES

#include "motion_vector_inc.glsl"

// Cross-backend (Vulkan / Metal / D3D12) single-dispatch Temporal AA & Upscaler (TAAU).
// Reads color, depth, velocity, and history strictly via sampler2D so RB_TEX_COLOR and
// RB_TEX_VELOCITY never require STORAGE_BIT (preserving mobile AFBC/UBWC compression).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D u_color;
layout(set = 0, binding = 1) uniform sampler2D u_depth;
layout(set = 0, binding = 2) uniform sampler2D u_velocity;
layout(set = 0, binding = 3) uniform sampler2D u_history;

#if defined(OUTPUT_R11G11B10F)
layout(r11f_g11f_b10f, set = 0, binding = 4) uniform restrict writeonly image2D u_output;
#elif defined(OUTPUT_RGB10A2)
layout(rgb10_a2, set = 0, binding = 4) uniform restrict writeonly image2D u_output;
#else
layout(rgba16f, set = 0, binding = 4) uniform restrict writeonly image2D u_output;
#endif

layout(push_constant, std430) uniform Params {
	highp mat4 reprojection_matrix;
	highp vec2 internal_size;
	highp vec2 target_size;
	highp vec2 inv_internal_size;
	highp vec2 inv_target_size;
	highp vec2 jitter_pixels; // Jitter in internal texels (±0.5)
	float variance_gamma;
	float reset_history;
}
params;

vec3 rgb_to_ycocg(vec3 c) {
	return vec3(
			0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
			0.5 * c.r - 0.5 * c.b,
			-0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 ycocg_to_rgb(vec3 y) {
	float tmp = y.x - y.z;
	return max(vec3(tmp + y.y, y.x + y.z, tmp - y.y), vec3(0.0));
}

vec3 sanitize_hdr(vec3 c) {
	if (any(isnan(c)) || any(isinf(c))) {
		return vec3(0.0);
	}
	return clamp(c, vec3(0.0), vec3(65000.0));
}

vec3 hdr_tonemap_ycocg(vec3 rgb) {
	rgb = sanitize_hdr(rgb);
	float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
	return rgb_to_ycocg(rgb / (1.0 + luma));
}

vec3 hdr_untonemap_ycocg(vec3 ycocg) {
	vec3 rgb = ycocg_to_rgb(ycocg);
	float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
	return rgb / max(1.0 - min(luma, 0.9995), 0.0005);
}

// 5-tap Jimenez Catmull-Rom bicubic filter (drops 4 corner taps, saving 44% history bandwidth).
vec3 sample_history_catmull_rom_5(highp vec2 uv) {
	highp vec2 sample_pos = uv * params.target_size;
	highp vec2 tc1 = floor(sample_pos - 0.5) + 0.5;
	highp vec2 f = sample_pos - tc1;
	highp vec2 f2 = f * f;
	highp vec2 f3 = f2 * f;

	highp vec2 w0 = -0.5 * f3 + f2 - 0.5 * f;
	highp vec2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
	highp vec2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
	highp vec2 w3 = 0.5 * f3 - 0.5 * f2;

	highp vec2 w12 = w1 + w2;
	highp vec2 tc0 = (tc1 - 1.0) * params.inv_target_size;
	highp vec2 tc3 = (tc1 + 2.0) * params.inv_target_size;
	highp vec2 tc12 = (tc1 + w2 / max(w12, vec2(1e-5))) * params.inv_target_size;

	float wc = w12.x * w12.y;
	float wl = w0.x * w12.y;
	float wr = w3.x * w12.y;
	float wt = w12.x * w0.y;
	float wb = w12.x * w3.y;
	float w_sum = wc + wl + wr + wt + wb;

	vec3 accum =
			sanitize_hdr(textureLod(u_history, vec2(tc12.x, tc12.y), 0.0).rgb) * wc +
			sanitize_hdr(textureLod(u_history, vec2(tc0.x, tc12.y), 0.0).rgb) * wl +
			sanitize_hdr(textureLod(u_history, vec2(tc3.x, tc12.y), 0.0).rgb) * wr +
			sanitize_hdr(textureLod(u_history, vec2(tc12.x, tc0.y), 0.0).rgb) * wt +
			sanitize_hdr(textureLod(u_history, vec2(tc12.x, tc3.y), 0.0).rgb) * wb;
	return max(accum / max(w_sum, 1e-5), vec3(0.0));
}

vec3 clip_ycocg_aabb(vec3 aabb_min, vec3 aabb_max, vec3 center, vec3 history) {
	vec3 d = history - center;
	vec3 half_ext = max((aabb_max - aabb_min) * 0.5, vec3(1e-5));
	vec3 unit = abs(d / half_ext);
	float max_comp = max(unit.x, max(unit.y, unit.z));
	if (max_comp > 1.0) {
		return center + d / max_comp;
	}
	return history;
}

float blackman_harris_weight(vec2 delta) {
	float d2 = dot(delta, delta);
	return exp(-2.29 * d2);
}

void main() {
	ivec2 dst_px = ivec2(gl_GlobalInvocationID.xy);
	if (dst_px.x >= int(params.target_size.x) || dst_px.y >= int(params.target_size.y)) {
		return;
	}

	highp vec2 dst_uv = (vec2(dst_px) + 0.5) * params.inv_target_size;
	highp vec2 src_pos = dst_uv * params.internal_size;
	ivec2 src_center = clamp(ivec2(floor(src_pos)), ivec2(0), ivec2(params.internal_size) - ivec2(1));
	ivec2 max_src = ivec2(params.internal_size) - ivec2(1);

	// 1. Reversed-Z (near = 1.0, far = 0.0) 3x3 closest-depth dilation + YCoCg neighborhood moments.
	float closest_depth = -1.0;
	ivec2 closest_coord = src_center;

	vec3 m1 = vec3(0.0);
	vec3 m2 = vec3(0.0);
	vec3 filtered_ycocg = vec3(0.0);
	float weight_sum = 0.0;

	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			ivec2 coord = clamp(src_center + ivec2(dx, dy), ivec2(0), max_src);
			float d = texelFetch(u_depth, coord, 0).r;
			if (d > closest_depth) {
				closest_depth = d;
				closest_coord = coord;
			}

			vec3 c_ycocg = hdr_tonemap_ycocg(texelFetch(u_color, coord, 0).rgb);
			m1 += c_ycocg;
			m2 += c_ycocg * c_ycocg;

			// Sub-pixel jittered reconstruction kernel (un-jitters internal sample grid onto target pixel).
			vec2 sample_center = (vec2(coord) + 0.5) + params.jitter_pixels;
			float w = blackman_harris_weight(sample_center - src_pos);
			filtered_ycocg += c_ycocg * w;
			weight_sum += w;
		}
	}

	filtered_ycocg /= max(weight_sum, 1e-5);

	// 2. Resolve exact motion vector (deriving camera velocity inline when sentinel <= -1.0).
	highp vec2 raw_vel = texelFetch(u_velocity, closest_coord, 0).xy;
	if (any(isnan(raw_vel)) || any(isinf(raw_vel))) {
		raw_vel = vec2(-1.0);
	}
	highp vec2 velocity;
	if (all(lessThanEqual(raw_vel, vec2(-1.0)))) {
		highp vec2 closest_uv = (vec2(closest_coord) + 0.5) * params.inv_internal_size;
		velocity = derive_motion_vector(closest_uv, closest_depth, params.reprojection_matrix);
	} else {
		velocity = raw_vel;
	}

	highp vec2 prev_uv = dst_uv + velocity;
	bool offscreen = any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)));
	if (params.reset_history > 0.5 || offscreen) {
		vec3 out_rgb = hdr_untonemap_ycocg(filtered_ycocg);
		imageStore(u_output, dst_px, vec4(out_rgb, 1.0));
		return;
	}

	// 3. Variance AABB clipping in perceptual YCoCg HDR space.
	vec3 mu = m1 * (1.0 / 9.0);
	vec3 sigma = sqrt(max(m2 * (1.0 / 9.0) - mu * mu, vec3(0.0)));
	float speed_px = length(velocity * params.target_size);
	float gamma = mix(params.variance_gamma, max(0.75, params.variance_gamma * 0.68), clamp(speed_px * 0.08, 0.0, 1.0));
	vec3 aabb_min = mu - sigma * gamma;
	vec3 aabb_max = mu + sigma * gamma;

	vec3 history_rgb = sample_history_catmull_rom_5(prev_uv);
	vec3 history_ycocg = hdr_tonemap_ycocg(history_rgb);
	vec3 clamped_history = clip_ycocg_aabb(aabb_min, aabb_max, clamp(filtered_ycocg, aabb_min, aabb_max), history_ycocg);

	// 4. Upscale-aware convergence blend weight (scales by area ratio internal/target + anti-flicker).
	float upscale_ratio = clamp((params.internal_size.x * params.internal_size.y) / max(params.target_size.x * params.target_size.y, 1.0), 0.25, 1.0);
	float base_alpha = mix(0.05, 0.10, upscale_ratio) * clamp(weight_sum * 0.85, 0.35, 1.25);
	float luma_diff = abs(filtered_ycocg.x - clamped_history.x) / max(max(filtered_ycocg.x, clamped_history.x), 0.15);
	float alpha = clamp(mix(base_alpha * 0.65, base_alpha * 1.6, clamp(luma_diff, 0.0, 1.0)), 0.03, 0.35);

	vec3 resolved_ycocg = mix(clamped_history, filtered_ycocg, alpha);
	vec3 resolved_rgb = hdr_untonemap_ycocg(resolved_ycocg);
	imageStore(u_output, dst_px, vec4(resolved_rgb, 1.0));
}
