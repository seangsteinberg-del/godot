/* clang-format off */
#[vertex]

#version 450

#VERSION_DEFINES

#include "luminance_reduce_raster_inc.glsl"

layout(location = 0) out vec2 uv_interp;
/* clang-format on */

void main() {
	vec2 base_arr[3] = vec2[](vec2(-1.0, -1.0), vec2(-1.0, 3.0), vec2(3.0, -1.0));
	gl_Position = vec4(base_arr[gl_VertexIndex], 0.0, 1.0);
	uv_interp = clamp(gl_Position.xy, vec2(0.0, 0.0), vec2(1.0, 1.0)) * 2.0; // saturate(x) * 2.0
}

/* clang-format off */
#[fragment]

#version 450

#VERSION_DEFINES

#include "luminance_reduce_raster_inc.glsl"

layout(location = 0) in vec2 uv_interp;
/* clang-format on */

layout(set = 0, binding = 0) uniform sampler2D source_exposure;

#ifdef FINAL_PASS
layout(set = 1, binding = 0) uniform sampler2D prev_luminance;
#endif

layout(location = 0) out highp float luminance;

// THE LOG-MEAN METER (the fork's patch 13, Longshot): the frame's adapting luminance is the geometric mean of its pixels'
// luminance - Reinhard, Stark, Shirley & Ferwerda 2002, the log-average as the scene's key - so a small bright source (a Moon's
// disc, a lamp, a flame) moves the meter by its share times its log ratio, never by its light: the arithmetic mean of
// max(r, g, b) stopped a night frame down tens of times for a Moon in it. Each pixel counts at least the window's floor (the
// eye's darkest adaptation); the chain averages the logs, the last pass takes the exponential.

void main() {
	ivec2 dest_pos = ivec2(uv_interp * settings.dest_size);
	ivec2 src_pos = ivec2(uv_interp * settings.source_size);

	ivec2 next_pos = (dest_pos + ivec2(1)) * settings.source_size / settings.dest_size;
	next_pos = max(next_pos, src_pos + ivec2(1)); //so it at least reads one pixel

	highp float sum = 0.0;
	for (int i = src_pos.x; i < next_pos.x; i++) {
		for (int j = src_pos.y; j < next_pos.y; j++) {
#ifdef FIRST_PASS
			highp vec3 c = texelFetch(source_exposure, ivec2(i, j), 0).rgb;
			// A PIXEL IS READ AS A NUMBER (patch 13): an overflow's infinity counts as the buffer's top, a NaN as no light
			highp float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
			l = isnan(l) ? 0.0 : min(l, 65504.0);
			sum += log(max(l, max(settings.min_luminance, 1e-20)));
#else
			sum += texelFetch(source_exposure, ivec2(i, j), 0).r;
#endif
		}
	}

	luminance = sum / float((next_pos.x - src_pos.x) * (next_pos.y - src_pos.y));

#ifdef FINAL_PASS
	// Obtain our target luminance: the mean of the logs back to a luminance, then the window
	luminance = clamp(exp(luminance), settings.min_luminance, settings.max_luminance);

	// Now smooth to our transition (the immediate frame, the blend at one, never reads the history: it may be unwritten)
	if (settings.exposure_adjust < 1.0) {
		highp float prev_lum = texelFetch(prev_luminance, ivec2(0, 0), 0).r; //1 pixel previous luminance
		if (isnan(prev_lum) || isinf(prev_lum)) {
			prev_lum = luminance; // a history that is no number restarts at this frame's own
		}
		prev_lum *= settings.history_scale; // THE METER'S HISTORY ACROSS THE LIFT (patch 17)
		luminance = prev_lum + (luminance - prev_lum) * clamp(settings.exposure_adjust, 0.0, 1.0);
	}
#endif
}
