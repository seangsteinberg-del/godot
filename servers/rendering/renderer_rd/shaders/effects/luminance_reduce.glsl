#[compute]

#version 450

#VERSION_DEFINES

// THE EYE IS A CAMERA (PROJECT LONGSHOT, the fork's patch 57, 10-24; docs/manual/sky.md THE EYE IS A CAMERA): the frame is metered as
// a photographic matrix meter meters it - a histogram of the lit pixels' log luminance, weighted toward the frame's centre, read
// between two percentiles (the darkest and the brightest shares of the frame ignored: space about a disc, a sun's disc, a glint, the
// stars alone), the adaptation in log space at two speeds. Two passes: HISTOGRAM - one workgroup of 8 x 8 threads per 64 x 64 tile,
// each thread 8 x 8 pixels, the tile's 64 bins summed in shared memory and written to the tiles buffer; RESOLVE - one workgroup sums
// the tiles, cuts the window, takes the window's log mean, clamps it to the camera's window, adapts from the last frame's reading and
// writes the one texel the tonemapper reads. The log-mean reading (Reinhard, Stark, Shirley & Ferwerda 2002; patch 13), the centre mask
// (SpaceEngine's AutoExpMask; patch 38), a pixel that is no number weighing nothing and an overflow counting as the buffer's top (patch
// 13), the history rescaled across a pre-exposure change (patch 17) and a frame of no lit area metering at the window's floor (patches
// 20 and 47) stand inside it; the holds, the floors and the key law of patches 21, 22, 42, 43 and 47 are gone with the reduce chain.

#define BLOCK_SIZE 8
#define TILE 64
#define BINS 64
#define LOG_LO -24.0 // the first bin's log2: the half float's floor region (a lit pixel under it lands in the first bin)
#define LOG_HI 16.0 // the last bin's log2: the half float's top (65504)
#define BIN_SCALE (float(BINS) / (LOG_HI - LOG_LO))
#define BLACK 1.0e-7 // a pixel at or under this is no light and no subject (the buffer's own black, a ten-millionth of a white ground)
#define W_FIX 1024.0 // the weight's fixed point
#define L_FIX 4096.0 // the weighted log's fixed point (|log2| <= 24, a tile of 4096 pixels: within an int)

layout(local_size_x = BLOCK_SIZE, local_size_y = BLOCK_SIZE, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	ivec2 source_size;
	ivec2 tiles; // the tile grid: ceil(size / 64)
	float max_luminance; // the camera's window (the buffer's units)
	float min_luminance;
	float adapt_up; // this frame's blend toward a brighter reading, 1 - exp(-speed dt); 1 (or more) sets the reading at once
	float adapt_down; // ... and toward a darker one
	float history_scale; // THE METER'S HISTORY ACROSS THE LIFT (patch 17): this frame's exposure multiplier over the last's
	float window_low; // the share of the lit weight under the window
	float window_high; // the share of the lit weight under the window's top
	float min_weight; // the least lit weight (the centre mask's sum) that is a subject; under it the frame meters at the floor
	float has_excluded; // one when a pass declared its luminance in the excluded texture (set 2)
}
params;

#ifdef MODE_HISTOGRAM

layout(set = 0, binding = 0) uniform sampler2D source_texture;
layout(set = 1, binding = 0, std430) restrict writeonly buffer Tiles {
	vec2 bins[]; // [tile * BINS + bin]: x the bin's weight, y the bin's sum of weight times log2 luminance
}
tiles;
layout(set = 2, binding = 0) uniform sampler2D excluded_texture; // THE PICTURE DECLARES ITSELF TO THE METER: the luminance a display-referred pass drew into the buffer at this pixel (the night sky's picture, drawn relative to the frame's own gain), left out of the reading

shared uint s_w[BINS];
shared int s_l[BINS];

void main() {
	uint t = gl_LocalInvocationID.y * BLOCK_SIZE + gl_LocalInvocationID.x;
	s_w[t] = 0u;
	s_l[t] = 0;
	groupMemoryBarrier();
	barrier();

	ivec2 tile_origin = ivec2(gl_WorkGroupID.xy) * TILE;
	ivec2 cell = tile_origin + ivec2(gl_LocalInvocationID.xy) * BLOCK_SIZE;
	for (int j = 0; j < BLOCK_SIZE; j++) {
		for (int i = 0; i < BLOCK_SIZE; i++) {
			ivec2 pos = cell + ivec2(i, j);
			if (any(greaterThanEqual(pos, params.source_size))) {
				continue; // a texel past the frame's edge is no pixel
			}
			vec3 v = texelFetch(source_texture, pos, 0).rgb;
			float l = dot(v, vec3(0.2126, 0.7152, 0.0722));
			if (isnan(l)) {
				continue; // no number is no light (patch 13)
			}
			l = min(l, 65504.0); // an overflow counts as the buffer's top (patch 13)
			if (params.has_excluded > 0.5) {
				l -= texelFetch(excluded_texture, pos, 0).r; // THE PICTURE DECLARES ITSELF TO THE METER: a picture drawn over the frame's own gain would meter itself (the exposure hunted); its light leaves the reading, the sky's beside it stays
			}
			if (l <= BLACK) {
				continue; // no light is no subject (patch 20)
			}
			// THE CENTRE MASK (patch 38, the template's): one over the middle half of the frame, falling to nought at the corners by the square
			vec2 rc = (vec2(pos) + 0.5) / vec2(params.source_size) * 2.0 - 1.0;
			float rr = length(rc);
			float w = rr < 0.5 ? 1.0 : max(0.0, 1.0 - (rr - 0.5) * (rr - 0.5) / 0.8464);
			if (w <= 0.0) {
				continue;
			}
			float lg = log2(l);
			int bin = clamp(int((lg - LOG_LO) * BIN_SCALE), 0, BINS - 1);
			atomicAdd(s_w[bin], uint(w * W_FIX + 0.5));
			atomicAdd(s_l[bin], int(round(w * lg * L_FIX)));
		}
	}
	groupMemoryBarrier();
	barrier();

	uint tile = gl_WorkGroupID.y * uint(params.tiles.x) + gl_WorkGroupID.x;
	tiles.bins[tile * BINS + t] = vec2(float(s_w[t]) / W_FIX, float(s_l[t]) / L_FIX);
}

#endif

#ifdef MODE_RESOLVE

layout(set = 0, binding = 0, std430) restrict readonly buffer Tiles {
	vec2 bins[];
}
tiles;
layout(r32f, set = 1, binding = 0) uniform restrict writeonly image2D dest_luminance; // the one texel the tonemapper reads
layout(set = 2, binding = 0) uniform sampler2D prev_luminance; // the last frame's reading (the history)

shared float r_w[BINS];
shared float r_l[BINS];
shared float r_cum[BINS];

void main() {
	uint t = gl_LocalInvocationID.y * BLOCK_SIZE + gl_LocalInvocationID.x; // thread t owns bin t
	float w = 0.0;
	float l = 0.0;
	int n = params.tiles.x * params.tiles.y;
	for (int k = 0; k < n; k++) {
		vec2 b = tiles.bins[uint(k) * BINS + t];
		w += b.x;
		l += b.y;
	}
	r_w[t] = w;
	r_l[t] = l;
	groupMemoryBarrier();
	barrier();

	if (t == 0u) {
		// the cumulative weight by bin, the window cut on it, the window's log mean
		float cum = 0.0;
		for (int b = 0; b < BINS; b++) {
			cum += r_w[b];
			r_cum[b] = cum;
		}
		float total = cum;
		float reading;
		if (total <= max(params.min_weight, 1.0e-9)) {
			reading = params.min_luminance; // THE SUBJECT IS A SURFACE (patch 47's law, kept): too little lit area is no subject - the window's floor
		} else {
			float lo = params.window_low * total;
			float hi = max(params.window_high * total, lo + 1.0e-6);
			float sum_w = 0.0;
			float sum_l = 0.0;
			float c0 = 0.0;
			for (int b = 0; b < BINS; b++) {
				float c1 = r_cum[b];
				float span = c1 - c0;
				if (span > 0.0) {
					float f = clamp((min(c1, hi) - max(c0, lo)) / span, 0.0, 1.0); // the bin's share inside the window
					sum_w += f * r_w[b];
					sum_l += f * r_l[b];
				}
				c0 = c1;
			}
			reading = sum_w > 0.0 ? exp2(sum_l / sum_w) : params.min_luminance;
		}
		float lo_l = params.min_luminance;
		float hi_l = max(params.max_luminance, lo_l);
		reading = clamp(reading, lo_l, hi_l);
		if (params.adapt_up < 1.0 || params.adapt_down < 1.0) {
			float prev = texelFetch(prev_luminance, ivec2(0, 0), 0).r;
			if (isnan(prev) || isinf(prev) || prev <= 0.0) {
				prev = reading; // a history that is no number restarts at this frame's own
			}
			prev *= params.history_scale; // THE METER'S HISTORY ACROSS THE LIFT (patch 17)
			// THE ADAPTATION IN LOG SPACE at two speeds: toward a brighter scene by adapt_up (the eye closes fast), toward a darker one by
			// adapt_down (it opens slowly) - never a linear blend of luminances across decades
			float lp = log2(max(prev, 1.0e-30));
			float lr = log2(max(reading, 1.0e-30));
			float k = lr > lp ? params.adapt_up : params.adapt_down;
			reading = exp2(lp + (lr - lp) * clamp(k, 0.0, 1.0));
			reading = clamp(reading, lo_l, hi_l);
		}
		imageStore(dest_luminance, ivec2(0, 0), vec4(reading));
	}
}

#endif
