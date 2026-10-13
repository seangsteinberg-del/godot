#[compute]

#version 450

#VERSION_DEFINES

#define BLOCK_SIZE 8

layout(local_size_x = BLOCK_SIZE, local_size_y = BLOCK_SIZE, local_size_z = 1) in;

shared float tmp_data[BLOCK_SIZE * BLOCK_SIZE];
shared float tmp_w[BLOCK_SIZE * BLOCK_SIZE]; // THE METER READS THE SUBJECT (patch 20): the pixels' weights beside their logs
shared float tmp_h[BLOCK_SIZE * BLOCK_SIZE]; // THE HIGHLIGHT HOLDS THE WINDOW (patch 21): the capped light, summed to the blocks' means, then their max
shared float tmp_f[BLOCK_SIZE * BLOCK_SIZE]; // THE FILLED LIGHT (patch 22): the light uncapped, summed to the 8 x 8 means, their min over each 32 x 32 block, then the blocks' max
shared float tmp_q[4]; // THE FILLED LIGHT (patch 22): the blocks' rung's four 32 x 32 quarters, each its least 8 x 8 mean

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
layout(rgba32f, set = 0, binding = 0) uniform restrict readonly image2D source_luminance; // THE METER READS THE SUBJECT (patch 20): r the weighted log's mean, g the weight's mean; THE HIGHLIGHT (patch 21): b the blocks' highlight; THE FILLED LIGHT (patch 22): a

#endif

#ifdef WRITE_LUMINANCE
layout(r32f, set = 1, binding = 0) uniform restrict writeonly image2D dest_luminance; // the final: the effective luminance alone
#else
layout(rgba32f, set = 1, binding = 0) uniform restrict writeonly image2D dest_luminance;
#endif

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
	// THE HIGHLIGHT HOLDS THE WINDOW (patch 21, Longshot): the share of the frame's brightest lit block the meter reads at least (0: no
	// law) - the day's window opens downward only as far as a sunlit cloud, a snowfield or a white hull stays at the display's
	// white; a pixel counts at most the cap (twice a white ground in the buffer's units: a glint or a sun's disc is a light, not a
	// surface); the block is 64 x 64 pixels (the pixels' means at the first rung, the blocks' means at the second, their max after)
	float highlight_hold;
	float highlight_cap;
	int level;
	// THE FILLED LIGHT (patch 22, Longshot): the share of the frame's brightest FILLED block the meter reads at least (0: no law) -
	// a block's light is the least of its 8 x 8 sub-blocks' uncapped means, so a light counts whole only where it fills 32 x 32
	// pixels (about a degree and a half of a 1080p frame at 50 degrees): a near entry fire's layer - face on, edge on or round a
	// backshell seen from behind - or an engine's flame does, a glint, a star, the Sun's or the Moon's disc at a normal lens or a
	// far fire's head never does. The game sets it where the scene has no key light of its own (the night, the shadow): there a light
	// that fills the eye is the scene's light, and the eye exposes for it as a camera does - the sheet at the surface near white,
	// its colour around it - where the log mean of a frame mostly black left a near fire many stops past white
	float light_hold;
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
		// THE METER READS THE SUBJECT (patch 20, Longshot): a pixel under the window's floor is no subject - space about a
		// planet's disc, a night ground under the eye's darkest adaptation - and carries no weight (the log-mean over every
		// pixel at the floor metered a disc in black space dark and opened the eye to its ceiling whatever the disc's light);
		// the pixels with light weigh toward the frame's centre, the template's centre-weighted histogram (0.3 at the corners)
		// THE SUBJECT IS EVERY LIT PIXEL (patch 20 amended): the line under which a pixel is no subject is the buffer's own
		// black - a ten-millionth of a white ground - never the window's floor (by day the key over its headroom: a dim
		// disc's twilight half fell out with the space about it and the meter clamped at the ceiling on the bright half)
		float floor_l = 1e-7;
		vec2 rc = (vec2(pos) + 0.5) / vec2(params.source_size) * 2.0 - 1.0;
		float wc = 0.3 + 0.7 * exp(-2.0 * dot(rc, rc));
		float w = l > floor_l ? wc : 0.0;
		tmp_data[t] = w * log(max(l, floor_l));
		tmp_w[t] = w;
		tmp_h[t] = min(l, params.highlight_cap); // THE HIGHLIGHT (patch 21): every pixel, space at nought, the light capped
		tmp_f[t] = l; // THE FILLED LIGHT (patch 22): every pixel's whole light
#else
		vec4 sl = imageLoad(source_luminance, pos);
		tmp_data[t] = sl.r;
		tmp_w[t] = sl.g;
		tmp_h[t] = sl.b;
		tmp_f[t] = sl.a;
#endif
	} else {
		tmp_data[t] = 0.0;
		tmp_w[t] = 0.0;
		tmp_h[t] = 0.0;
		tmp_f[t] = params.level == 1 ? 3.0e38 : 0.0; // THE FILLED LIGHT (patch 22): no texel is no part of a block's least, nor of a sum or a max
	}

	groupMemoryBarrier();
	barrier();

	// THE FILLED LIGHT (patch 22): at the blocks' rung each of the 64 x 64 region's four 32 x 32 quarters takes the least of its
	// 4 x 4 eight-pixel means (a quarter past the frame's edge has no light: nought), before the tree below sums or maxes the rest
	if (params.level == 1) {
		if (t < 4u) {
			uint qx = (t & 1u) * 4u;
			uint qy = (t >> 1u) * 4u;
			float m = 3.0e38;
			for (uint yy = 0u; yy < 4u; yy++) {
				for (uint xx = 0u; xx < 4u; xx++) {
					m = min(m, tmp_f[(qy + yy) * BLOCK_SIZE + qx + xx]);
				}
			}
			tmp_q[t] = m >= 3.0e38 ? 0.0 : m;
		}
		groupMemoryBarrier();
		barrier();
	}

	uint size = (BLOCK_SIZE * BLOCK_SIZE) >> 1;
	bool h_max = params.level >= 2; // THE HIGHLIGHT (patch 21): the pixels and the blocks' means are summed, the blocks' max is kept after

	do {
		if (t < size) {
			tmp_data[t] += tmp_data[t + size];
			tmp_w[t] += tmp_w[t + size];
			tmp_h[t] = h_max ? max(tmp_h[t], tmp_h[t + size]) : tmp_h[t] + tmp_h[t + size];
			// THE FILLED LIGHT (patch 22): the pixels summed to the 8 x 8 means, the blocks' max past the blocks' rung (the blocks'
			// rung takes its quarters below)
			if (params.level != 1) {
				tmp_f[t] = params.level == 0 ? tmp_f[t] + tmp_f[t + size] : max(tmp_f[t], tmp_f[t + size]);
			}
		}
		groupMemoryBarrier();
		barrier();

		size >>= 1;
	} while (size >= 1);

	if (t == 0) {
		//compute rect size
		ivec2 rect_size = min(params.source_size - pos, ivec2(BLOCK_SIZE));
		float avg = tmp_data[0] / float(rect_size.x * rect_size.y);
		float wavg = tmp_w[0] / float(rect_size.x * rect_size.y);
		float hblk = h_max ? tmp_h[0] : tmp_h[0] / float(rect_size.x * rect_size.y); // THE HIGHLIGHT (patch 21): the block's mean, or the blocks' max
		// THE FILLED LIGHT (patch 22): the 8 x 8 mean, the region's best 32 x 32 quarter's least, or the blocks' max
		float fblk = params.level == 0 ? tmp_f[0] / float(rect_size.x * rect_size.y) : (params.level == 1 ? max(max(tmp_q[0], tmp_q[1]), max(tmp_q[2], tmp_q[3])) : tmp_f[0]);
		pos /= ivec2(BLOCK_SIZE);
#ifdef WRITE_LUMINANCE
		// THE METER READS THE SUBJECT (patch 20): the weighted logs over the weight - a frame with no lit pixel meters at the
		// floor, as every pixel did before
		avg = wavg > 1e-9 ? avg / wavg : log(max(params.min_luminance, 1e-20));   // no lit pixel: the window's floor, as before
		// the mean of the logs back to a luminance (the frame's geometric mean), written as the EFFECTIVE luminance: the
		// adaptation times the day's key over the key it earns (the readers divide by it and multiply by the day's key),
		// the window mapped through the same law; the history is effective too
		avg = exp(avg);
		float lo = params.min_luminance * key_ratio_at(params.min_luminance);
		float hi = max(params.max_luminance * key_ratio_at(params.max_luminance), lo);
		avg *= key_ratio_at(clamp(avg, params.min_luminance, params.max_luminance));
		// THE HIGHLIGHT HOLDS THE WINDOW (patch 21): the meter reads at least the hold's share of the frame's brightest lit block, so
		// the gain (the scale over the reading) is at most one over the block's light times the scale over the hold - the day's
		// opening stops where its brightest surface reaches white; nought by night (the night's eye keeps patch 13's own law)
		if (params.highlight_hold > 0.0) {
			avg = max(avg, params.highlight_hold * hblk);
		}
		// THE FILLED LIGHT (patch 22): and at least the light hold's share of the frame's brightest filled block - a light that fills
		// the eye lands there at the key's scale over the hold, however dark the rest of the frame reads
		if (params.light_hold > 0.0) {
			avg = max(avg, params.light_hold * fblk);
		}
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
		imageStore(dest_luminance, pos, vec4(avg));
#else
		imageStore(dest_luminance, pos, vec4(avg, wavg, hblk, fblk)); // THE METER READS THE SUBJECT: the two means down the chain, THE HIGHLIGHT and THE FILLED LIGHT beside them
#endif
	}
}
