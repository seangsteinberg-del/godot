#[compute]

#version 450

#VERSION_DEFINES

#define BLOCK_SIZE 8

layout(local_size_x = BLOCK_SIZE, local_size_y = BLOCK_SIZE, local_size_z = 1) in;

shared float tmp_data[BLOCK_SIZE * BLOCK_SIZE];

// THE LOG-MEAN METER (the fork's patch 13, Longshot): the frame's adapting luminance is the geometric mean of its pixels'
// luminance - Reinhard, Stark, Shirley & Ferwerda 2002, the log-average as the scene's key - so a small bright source (a Moon's
// disc, a lamp, a flame) moves the meter by its share times its log ratio, never by its light: the arithmetic mean of
// max(r, g, b) stopped a night frame down tens of times for a Moon in it. Each pixel counts at least the window's floor (the
// eye's darkest adaptation); the chain averages the logs, the last pass takes the exponential.

#ifdef READ_TEXTURE

//use for main texture
layout(set = 0, binding = 0) uniform sampler2D source_texture;

#else

//use for intermediate textures
layout(r32f, set = 0, binding = 0) uniform restrict readonly image2D source_luminance;

#endif

layout(r32f, set = 1, binding = 0) uniform restrict writeonly image2D dest_luminance;

#ifdef WRITE_LUMINANCE
layout(set = 2, binding = 0) uniform sampler2D prev_luminance;
#endif

layout(push_constant, std430) uniform Params {
	ivec2 source_size;
	float max_luminance;
	float min_luminance;
	float exposure_adjust;
	float key_ratio; // THE KEY FOLLOWS THE ADAPTATION (patch 13): the day's key over the night's (1: no law)
	float ln_scotopic; // the log of the luminance under which the eye is wholly night-adapted (the buffer's units)
	float ln_photopic; // the log of the luminance over which the day's key stands
	float history_scale; // THE METER'S HISTORY ACROSS THE LIFT (patch 17): this frame's exposure multiplier over the last's
}
params;

// THE KEY FOLLOWS THE ADAPTATION (patch 13): the day's key over the key at this adaptation - one at the photopic edge and
// over, the whole ratio at the scotopic edge and under, its power log-linear between (CIE 191:2010's mesopic range)
float key_ratio_at(float l) {
	if (params.key_ratio <= 1.0 || params.ln_photopic <= params.ln_scotopic) {
		return 1.0;
	}
	float t = clamp((log(max(l, 1e-30)) - params.ln_scotopic) / (params.ln_photopic - params.ln_scotopic), 0.0, 1.0);
	return pow(params.key_ratio, 1.0 - t);
}

void main() {
	uint t = gl_LocalInvocationID.y * BLOCK_SIZE + gl_LocalInvocationID.x;
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);

	if (all(lessThan(pos, params.source_size))) { // a texel past the edge on either axis is no pixel (a log is no zero)
#ifdef READ_TEXTURE
		vec3 v = texelFetch(source_texture, pos, 0).rgb;
		// A PIXEL IS READ AS A NUMBER (patch 13): past the half float (an overflow's infinity) it counts as the buffer's top,
		// no number at all as no light - one pixel never makes the eye's adaptation a NaN
		float l = dot(v, vec3(0.2126, 0.7152, 0.0722));
		l = isnan(l) ? 0.0 : min(l, 65504.0);
		tmp_data[t] = log(max(l, max(params.min_luminance, 1e-20)));
#else
		tmp_data[t] = imageLoad(source_luminance, pos).r;
#endif
	} else {
		tmp_data[t] = 0.0;
	}

	groupMemoryBarrier();
	barrier();

	uint size = (BLOCK_SIZE * BLOCK_SIZE) >> 1;

	do {
		if (t < size) {
			tmp_data[t] += tmp_data[t + size];
		}
		groupMemoryBarrier();
		barrier();

		size >>= 1;
	} while (size >= 1);

	if (t == 0) {
		//compute rect size
		ivec2 rect_size = min(params.source_size - pos, ivec2(BLOCK_SIZE));
		float avg = tmp_data[0] / float(rect_size.x * rect_size.y);
		//float avg = tmp_data[0] / float(BLOCK_SIZE*BLOCK_SIZE);
		pos /= ivec2(BLOCK_SIZE);
#ifdef WRITE_LUMINANCE
		// the mean of the logs back to a luminance (the frame's geometric mean), written as the EFFECTIVE luminance: the
		// adaptation times the day's key over the key it earns (the readers divide by it and multiply by the day's key),
		// the window mapped through the same law; the history is effective too
		avg = exp(avg);
		float lo = params.min_luminance * key_ratio_at(params.min_luminance);
		float hi = max(params.max_luminance * key_ratio_at(params.max_luminance), lo);
		avg *= key_ratio_at(clamp(avg, params.min_luminance, params.max_luminance));
		if (params.exposure_adjust < 1.0) { // the immediate frame (the blend at one) never reads the history: it may be unwritten
			float prev_lum = texelFetch(prev_luminance, ivec2(0, 0), 0).r; //1 pixel previous exposure
			if (isnan(prev_lum) || isinf(prev_lum)) {
				prev_lum = avg; // a history that is no number restarts at this frame's own
			}
			// THE METER'S HISTORY ACROSS THE LIFT (patch 17): the history is in the buffer's pre-exposed units; a changed
			// multiplier (a night's lift at warp, a re-seat, a flame) moves it by the ratio, so the adaptation stands still in
			// the scene's own light instead of chasing the lift for a second
			prev_lum *= params.history_scale;
			avg = prev_lum + (avg - prev_lum) * params.exposure_adjust;
		}
		avg = clamp(avg, lo, hi);
#endif
		imageStore(dest_luminance, pos, vec4(avg));
	}
}
