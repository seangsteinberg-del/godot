///////////////////////////////////////////////////////////////////////////////////
// Copyright(c) 2016-2022 Panos Karabelas
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is furnished
// to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
///////////////////////////////////////////////////////////////////////////////////
// File changes (yyyy-mm-dd)
// 2026-11-15: LONGSHOT [patch] #113: a thin thing of geometry, once seen, is held until it is seen again (it shook at a still eye)
// 2026-10-05: LONGSHOT [patch] #10: the flicker guard bounded so a steady input always converges (a bright point's centre drew black)
// 2025-11-05: Jakub Brzyski: Added dynamic variance, base variance value adjusted to reduce ghosting
// 2022-05-06: Panos Karabelas: first commit
// 2020-12-05: Joan Fons: convert to Vulkan and Godot
///////////////////////////////////////////////////////////////////////////////////

#[compute]

#version 450

#VERSION_DEFINES

// Based on Spartan Engine's TAA implementation (without TAA upscale).
// <https://github.com/PanosK92/SpartanEngine/blob/a8338d0609b85dc32f3732a5c27fb4463816a3b9/Data/shaders/temporal_antialiasing.hlsl>

#define GROUP_SIZE 8
#define FLT_MIN 0.00000001
#define FLT_MAX 32767.0
#define RPC_9 0.11111111111
#define RPC_16 0.0625

#define DISOCCLUSION_SCALE 0.01 // Scale the weight of this pixel calculated as (change in velocity - threshold) * scale.

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

layout(rgba16f, set = 0, binding = 0) uniform restrict readonly image2D color_buffer;
layout(set = 0, binding = 1) uniform sampler2D depth_buffer;
layout(rg16f, set = 0, binding = 2) uniform restrict readonly image2D velocity_buffer;
layout(rg16f, set = 0, binding = 3) uniform restrict readonly image2D last_velocity_buffer;
layout(set = 0, binding = 4) uniform sampler2D history_buffer;
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D output_buffer;
// LONGSHOT patch #25 THE SETTLED GAS STANDS: a layer settled by its own history before the transparent pass (the exhaust's gas), its
// opacity in alpha - the transparent default where none was handed over
layout(set = 0, binding = 6) uniform sampler2D settled_buffer;

layout(push_constant, std430) uniform Params {
	vec2 resolution;
	float disocclusion_threshold; // 0.1 / max(params.resolution.x, params.resolution.y)
	float variance_dynamic;
}
params;

const ivec2 kOffsets3x3[9] = {
	ivec2(-1, -1),
	ivec2(0, -1),
	ivec2(1, -1),
	ivec2(-1, 0),
	ivec2(0, 0),
	ivec2(1, 0),
	ivec2(-1, 1),
	ivec2(0, 1),
	ivec2(1, 1),
};

/*------------------------------------------------------------------------------
						THREAD GROUP SHARED MEMORY (LDS)
------------------------------------------------------------------------------*/

const int kBorderSize = 1;
const int kGroupSize = GROUP_SIZE;
const int kTileDimension = kGroupSize + kBorderSize * 2;
const int kTileDimension2 = kTileDimension * kTileDimension;

// LONGSHOT [patch] #108, #109 THE HISTORY IS HELD TO ITS NEIGHBOURHOOD'S OWN RANGE. The filter clipped the history by the neighbourhood's
// variance alone. Beside a sun's disc - sixty thousand against one - that box runs from far under nought to tens of thousands
// and admits anything: a history the bicubic sample's negative lobes had driven under nought (floored at nought) stood as a
// black pixel on the disc's edge, held there by every frame's sample, a ring of them wherever the eye turned (the director: "when
// moving the sun gets a black circle inside of it sometimes"; white points and dark squares along a hull's edge before a bright
// disc). THE LAW: the variance box is MET WITH the neighbourhood's own least and most, so no clipped history is darker than its
// darkest neighbour or brighter than its brightest. The neighbourhood is weighed IN LINEAR LIGHT (#109: #108 weighed it in the
// bounded space, and with the box shut by motion the history is the neighbourhood's mean - a mean of tonemapped values keeps a
// fifth of the light a point's linear mean keeps: the stars' peaks halved while the eye panned). Only the last blend stands in
// the bounded space, as it did; a value that is no number is nought, one past the buffer's top is the top, and the way back is
// kept under the top (the inverse is finite)
const float BOUNDED_TOP = 0.99998474; // the buffer's top, 65504, in the bounded space
vec3 finite_color(vec3 hdr) {
	vec3 c = min(max(hdr, vec3(0.0)), vec3(65504.0));
	return mix(c, vec3(0.0), isnan(hdr));
}
vec3 to_bounded(vec3 hdr) {
	return hdr / (hdr + 1.0);
}
vec3 from_bounded(vec3 sdr) {
	vec3 c = clamp(sdr, vec3(0.0), vec3(BOUNDED_TOP));
	return c / (1.0 - c);
}

float get_depth(ivec2 thread_id) {
	return texelFetch(depth_buffer, thread_id, 0).r;
}

shared vec3 tile_color[kTileDimension][kTileDimension];
shared float tile_depth[kTileDimension][kTileDimension];

vec3 load_color(uvec2 group_thread_id) {
	group_thread_id += kBorderSize;
	return tile_color[group_thread_id.x][group_thread_id.y];
}

void store_color(uvec2 group_thread_id, vec3 color) {
	tile_color[group_thread_id.x][group_thread_id.y] = color;
}

float load_depth(uvec2 group_thread_id) {
	group_thread_id += kBorderSize;
	return tile_depth[group_thread_id.x][group_thread_id.y];
}

void store_depth(uvec2 group_thread_id, float depth) {
	tile_depth[group_thread_id.x][group_thread_id.y] = depth;
}

void store_color_depth(uvec2 group_thread_id, ivec2 thread_id) {
	// out of bounds clamp
	thread_id = clamp(thread_id, ivec2(0, 0), ivec2(params.resolution) - ivec2(1, 1));

	store_color(group_thread_id, imageLoad(color_buffer, thread_id).rgb);
	store_depth(group_thread_id, get_depth(thread_id));
}

void populate_group_shared_memory(uvec2 group_id, uint group_index) {
	// Populate group shared memory
	ivec2 group_top_left = ivec2(group_id) * kGroupSize - kBorderSize;
	if (group_index < (kTileDimension2 >> 2)) {
		ivec2 group_thread_id_1 = ivec2(group_index % kTileDimension, group_index / kTileDimension);
		ivec2 group_thread_id_2 = ivec2((group_index + (kTileDimension2 >> 2)) % kTileDimension, (group_index + (kTileDimension2 >> 2)) / kTileDimension);
		ivec2 group_thread_id_3 = ivec2((group_index + (kTileDimension2 >> 1)) % kTileDimension, (group_index + (kTileDimension2 >> 1)) / kTileDimension);
		ivec2 group_thread_id_4 = ivec2((group_index + kTileDimension2 * 3 / 4) % kTileDimension, (group_index + kTileDimension2 * 3 / 4) / kTileDimension);

		store_color_depth(group_thread_id_1, group_top_left + group_thread_id_1);
		store_color_depth(group_thread_id_2, group_top_left + group_thread_id_2);
		store_color_depth(group_thread_id_3, group_top_left + group_thread_id_3);
		store_color_depth(group_thread_id_4, group_top_left + group_thread_id_4);
	}

	// Wait for group threads to load store data.
	groupMemoryBarrier();
	barrier();
}

/*------------------------------------------------------------------------------
								VELOCITY
------------------------------------------------------------------------------*/

void depth_test_min(uvec2 pos, inout float min_depth, inout uvec2 min_pos) {
	float depth = load_depth(pos);

	if (depth < min_depth) {
		min_depth = depth;
		min_pos = pos;
	}
}

// Returns velocity with closest depth (3x3 neighborhood)
void get_closest_pixel_velocity_3x3(in uvec2 group_pos, uvec2 group_top_left, out vec2 velocity) {
	float min_depth = 1.0;
	uvec2 min_pos = group_pos;

	depth_test_min(group_pos + kOffsets3x3[0], min_depth, min_pos);
	depth_test_min(group_pos + kOffsets3x3[1], min_depth, min_pos);
	depth_test_min(group_pos + kOffsets3x3[2], min_depth, min_pos);
	depth_test_min(group_pos + kOffsets3x3[3], min_depth, min_pos);
	depth_test_min(group_pos + kOffsets3x3[4], min_depth, min_pos);
	depth_test_min(group_pos + kOffsets3x3[5], min_depth, min_pos);
	depth_test_min(group_pos + kOffsets3x3[6], min_depth, min_pos);
	depth_test_min(group_pos + kOffsets3x3[7], min_depth, min_pos);
	depth_test_min(group_pos + kOffsets3x3[8], min_depth, min_pos);

	// Velocity out
	velocity = imageLoad(velocity_buffer, ivec2(group_top_left + min_pos)).xy;
}

/*------------------------------------------------------------------------------
							  HISTORY SAMPLING
------------------------------------------------------------------------------*/

vec3 sample_catmull_rom_9(sampler2D stex, vec2 uv, vec2 resolution) {
	// Source: https://gist.github.com/TheRealMJP/c83b8c0f46b63f3a88a5986f4fa982b1
	// License: https://gist.github.com/TheRealMJP/bc503b0b87b643d3505d41eab8b332ae

	// We're going to sample a 4x4 grid of texels surrounding the target UV coordinate. We'll do this by rounding
	// down the sample location to get the exact center of our "starting" texel. The starting texel will be at
	// location [1, 1] in the grid, where [0, 0] is the top left corner.
	vec2 sample_pos = uv * resolution;
	vec2 texPos1 = floor(sample_pos - 0.5f) + 0.5f;

	// Compute the fractional offset from our starting texel to our original sample location, which we'll
	// feed into the Catmull-Rom spline function to get our filter weights.
	vec2 f = sample_pos - texPos1;

	// Compute the Catmull-Rom weights using the fractional offset that we calculated earlier.
	// These equations are pre-expanded based on our knowledge of where the texels will be located,
	// which lets us avoid having to evaluate a piece-wise function.
	vec2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
	vec2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
	vec2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
	vec2 w3 = f * f * (-0.5f + 0.5f * f);

	// Work out weighting factors and sampling offsets that will let us use bilinear filtering to
	// simultaneously evaluate the middle 2 samples from the 4x4 grid.
	vec2 w12 = w1 + w2;
	vec2 offset12 = w2 / (w1 + w2);

	// Compute the final UV coordinates we'll use for sampling the texture
	vec2 texPos0 = texPos1 - 1.0f;
	vec2 texPos3 = texPos1 + 2.0f;
	vec2 texPos12 = texPos1 + offset12;

	texPos0 /= resolution;
	texPos3 /= resolution;
	texPos12 /= resolution;

	vec3 result = vec3(0.0f, 0.0f, 0.0f);

	result += textureLod(stex, vec2(texPos0.x, texPos0.y), 0.0).xyz * w0.x * w0.y;
	result += textureLod(stex, vec2(texPos12.x, texPos0.y), 0.0).xyz * w12.x * w0.y;
	result += textureLod(stex, vec2(texPos3.x, texPos0.y), 0.0).xyz * w3.x * w0.y;

	result += textureLod(stex, vec2(texPos0.x, texPos12.y), 0.0).xyz * w0.x * w12.y;
	result += textureLod(stex, vec2(texPos12.x, texPos12.y), 0.0).xyz * w12.x * w12.y;
	result += textureLod(stex, vec2(texPos3.x, texPos12.y), 0.0).xyz * w3.x * w12.y;

	result += textureLod(stex, vec2(texPos0.x, texPos3.y), 0.0).xyz * w0.x * w3.y;
	result += textureLod(stex, vec2(texPos12.x, texPos3.y), 0.0).xyz * w12.x * w3.y;
	result += textureLod(stex, vec2(texPos3.x, texPos3.y), 0.0).xyz * w3.x * w3.y;

	return max(result, 0.0f);
}

/*------------------------------------------------------------------------------
							  HISTORY CLIPPING
------------------------------------------------------------------------------*/

// Based on "Temporal Reprojection Anti-Aliasing" - https://github.com/playdeadgames/temporal
vec3 clip_aabb(vec3 aabb_min, vec3 aabb_max, vec3 p, vec3 q) {
	vec3 r = q - p;
	vec3 rmax = (aabb_max - p.xyz);
	vec3 rmin = (aabb_min - p.xyz);

	if (r.x > rmax.x + FLT_MIN) {
		r *= (rmax.x / r.x);
	}
	if (r.y > rmax.y + FLT_MIN) {
		r *= (rmax.y / r.y);
	}
	if (r.z > rmax.z + FLT_MIN) {
		r *= (rmax.z / r.z);
	}

	if (r.x < rmin.x - FLT_MIN) {
		r *= (rmin.x / r.x);
	}
	if (r.y < rmin.y - FLT_MIN) {
		r *= (rmin.y / r.y);
	}
	if (r.z < rmin.z - FLT_MIN) {
		r *= (rmin.z / r.z);
	}

	return p + r;
}

// Clip history to the neighbourhood of the current sample, in linear light, within the neighbourhood's own range ([patch] #108, #109)
vec3 clip_history_3x3(uvec2 group_pos, vec3 color_history, vec2 velocity_closest) {
	// Sample a 3x3 neighbourhood
	vec3 s1 = finite_color(load_color(group_pos + kOffsets3x3[0]));
	vec3 s2 = finite_color(load_color(group_pos + kOffsets3x3[1]));
	vec3 s3 = finite_color(load_color(group_pos + kOffsets3x3[2]));
	vec3 s4 = finite_color(load_color(group_pos + kOffsets3x3[3]));
	vec3 s5 = finite_color(load_color(group_pos + kOffsets3x3[4]));
	vec3 s6 = finite_color(load_color(group_pos + kOffsets3x3[5]));
	vec3 s7 = finite_color(load_color(group_pos + kOffsets3x3[6]));
	vec3 s8 = finite_color(load_color(group_pos + kOffsets3x3[7]));
	vec3 s9 = finite_color(load_color(group_pos + kOffsets3x3[8]));

	// Compute min and max (with an adaptive box size, which greatly reduces ghosting)
	vec3 color_avg = (s1 + s2 + s3 + s4 + s5 + s6 + s7 + s8 + s9) * RPC_9;
	vec3 color_avg2 = ((s1 * s1) + (s2 * s2) + (s3 * s3) + (s4 * s4) + (s5 * s5) + (s6 * s6) + (s7 * s7) + (s8 * s8) + (s9 * s9)) * RPC_9;
	// Use variance clipping as described in https://developer.download.nvidia.com/gameworks/events/GDC2016/msalvi_temporal_supersampling.pdf
	float box_size = mix(0.0f, params.variance_dynamic, smoothstep(0.02f, 0.0f, length(velocity_closest)));
	vec3 dev = sqrt(abs(color_avg2 - (color_avg * color_avg))) * box_size;
	// ... MET WITH the neighbourhood's own range ([patch] #108): one outlier widens the variance past every sample there is
	vec3 near_min = min(min(min(s1, s2), min(s3, s4)), min(min(s5, s6), min(min(s7, s8), s9)));
	vec3 near_max = max(max(max(s1, s2), max(s3, s4)), max(max(s5, s6), max(max(s7, s8), s9)));
	vec3 color_min = max(color_avg - dev, near_min);
	vec3 color_max = min(color_avg + dev, near_max);

	// Variance clipping
	vec3 color = clip_aabb(color_min, color_max, clamp(color_avg, color_min, color_max), color_history);

	// the clip's own rounding never leaves the box (and the box is numbers: the samples are)
	color = clamp(color, color_min, color_max);

	return color;
}

/*------------------------------------------------------------------------------
									TAA
------------------------------------------------------------------------------*/

const vec3 lumCoeff = vec3(0.299f, 0.587f, 0.114f);

float luminance(vec3 color) {
	return max(dot(color, lumCoeff), 0.0001f);
}

// LONGSHOT [patch] #113 A THIN THING SEEN IS HELD UNTIL IT IS SEEN AGAIN. A piece of geometry thinner than a pixel - a lattice's
// lace, a window's bar, a handrail, a painted line a hand over its floor - is met by a frame's one jittered sample in some frames
// and missed in the rest. In a frame that misses it nothing of it stands in the pixel's 3x3 neighbourhood, so the clip
// (clip_history_3x3: the history held to its neighbourhood's own range, [patch] #108) takes it out of the history whole; the
// next hit puts a sixteenth of it back; the miss after takes that out again. At a still eye over still things the picture
// SHOOK wherever the site had thin steel (the director, 11-15, of the launch complex seen from the air: "some of the mesh
// parts look like they are shaking for example these windows"; measured, tools/site/shimmer.py, two frames of one still view
// eight frames apart: the worst 64-pixel tiles 7 to 16 per cent changed under this filter, none at all with no filter).
// THE LAW: a pixel whose sample MET a thin thing - it stands apart from both its neighbours across one of the four lines
// through it IN COLOUR AND IN DEPTH, proud of the plane between them (a line or a point of geometry: never an edge, where one side agrees;
// never a glint, a star or a mote of gas, which have no depth of their own and keep the filter exactly as it was) - is LOCKED:
// the lock rides the history's alpha, travels with the history's reprojection, fades over LOCK_FRAMES frames (longer than the
// jitter's sixteen, so a thing a sixteenth of a pixel wide is met again before its lock is gone) and is dropped wherever the
// pixel's motion is not its history's. While a pixel is locked its history is NOT clipped: it is the history's own texel (the plain sample: no
// spline lobe to hold dark, #108's fault), and the sixteenth blend converges on the thing's true share of the pixel - a
// steady faint line where there was a flashing one. Nothing held can stand for good: every frame still blends the input in.
#define LOCK_FRAMES 24.0
#define LOCK_COLOUR_LO 0.05 // the centre apart from both sides by this share of the brightest of the three: no lock under it ...
#define LOCK_COLOUR_HI 0.20 // ... a whole lock from here
// ... and PROUD OF THE PLANE BETWEEN ITS TWO NEIGHBOURS by this share of its own depth (a plane's depth is linear across the
// screen: nought). THE GROUND IS NOT A THIN THING: a hill's own roundness stands a pixel's middle proud of its neighbours too -
// by the pixel's width squared over twice the hill's radius, a few millionths of its depth for ground a kilometre off - and
// every grain of a grass texture is apart from its neighbours in colour. At one threshold of two millionths the whole
// textured ground locked (measured 11-15, the pads from the air: the worst tile 14.7 per cent changed, from 4.5), so the
// lock begins well over a hill's roundness and is whole at a bar's (a mullion a hand proud of its wall, seen aslant from
// seven hundred metres, stands seventy millionths proud)
#define LOCK_DEPTH_LO 0.00001
#define LOCK_DEPTH_HI 0.00005

float lum_bounded(vec3 hdr) {
	return luminance(to_bounded(finite_color(hdr)));
}

// 0 on a flat and on an edge; up to 1 where the sample met a line or a point of geometry about a pixel wide or thinner
float thin_thing(uvec2 group_pos) {
	const ivec2 across[4] = { ivec2(3, 5), ivec2(1, 7), ivec2(0, 8), ivec2(2, 6) };
	float c = lum_bounded(load_color(group_pos + kOffsets3x3[4]));
	float dc = load_depth(group_pos + kOffsets3x3[4]);
	if (dc <= 0.0) {
		return 0.0; // (the sky: nothing of geometry)
	}
	float found = 0.0;
	for (int i = 0; i < 4; i++) {
		float da = load_depth(group_pos + kOffsets3x3[across[i].x]);
		float db = load_depth(group_pos + kOffsets3x3[across[i].y]);
		// (the depth runs from 1 at the eye to 0 far away: nearer is greater, and across a plane it runs straight, so the
		// middle of two neighbours is the plane's own depth between them. PROUD OF THAT MIDDLE, never nearer than both: a
		// bar on a wall seen aslant is proud of its wall by less than the wall's own run from one pixel to the next - the
		// first form of this test held the lattices against the sky and left every window's bars shaking, measured 11-15)
		float proud = smoothstep(LOCK_DEPTH_LO, LOCK_DEPTH_HI, (dc - 0.5 * (da + db)) / dc);
		if (proud <= 0.0) {
			continue;
		}
		float a = lum_bounded(load_color(group_pos + kOffsets3x3[across[i].x]));
		float b = lum_bounded(load_color(group_pos + kOffsets3x3[across[i].y]));
		float apart = max(min(c - a, c - b), min(a - c, b - c)); // over both, or under both
		found = max(found, proud * smoothstep(LOCK_COLOUR_LO, LOCK_COLOUR_HI, apart / max(max(a, b), max(c, 0.02))));
	}
	return found;
}

// This is "velocity disocclusion" as described by https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/.
// We use texel space, so our scale and threshold differ.
float get_factor_disocclusion(vec2 uv_reprojected, vec2 velocity) {
	vec2 velocity_previous = imageLoad(last_velocity_buffer, ivec2(uv_reprojected * params.resolution)).xy;
	vec2 velocity_texels = velocity * params.resolution;
	vec2 prev_velocity_texels = velocity_previous * params.resolution;
	float disocclusion = length(prev_velocity_texels - velocity_texels) - params.disocclusion_threshold;
	return clamp(disocclusion * DISOCCLUSION_SCALE, 0.0, 1.0);
}

// (the resolved colour, and in alpha the pixel's lock: [patch] #113)
vec4 temporal_antialiasing(uvec2 pos_group_top_left, uvec2 pos_group, uvec2 pos_screen, vec2 uv, sampler2D tex_history) {
	// Get the velocity of the current pixel
	vec2 velocity = imageLoad(velocity_buffer, ivec2(pos_screen)).xy;

	// Get reprojected uv
	vec2 uv_reprojected = uv + velocity;

	// Get input color (a number of the buffer: [patch] #108)
	vec3 color_input = finite_color(load_color(pos_group));

	// Get history color (catmull-rom reduces a lot of the blurring that you get under motion)
	vec3 color_history = finite_color(sample_catmull_rom_9(tex_history, uv_reprojected, params.resolution).rgb);

	// Clip history to the neighbourhood of the current sample (fixes a lot of the ghosting).
	vec2 velocity_closest = vec2(0.0); // This is best done by using the velocity with the closest depth.
	get_closest_pixel_velocity_3x3(pos_group, pos_group_top_left, velocity_closest);
	color_history = clip_history_3x3(pos_group, color_history, velocity_closest);

	// If re-projected UV is out of screen, converge to current color immediately.
	float factor_screen = any(lessThan(uv_reprojected, vec2(0.0))) || any(greaterThan(uv_reprojected, vec2(1.0))) ? 1.0 : 0.0;

	// Increase blend factor when there is disocclusion (fixes a lot of the remaining ghosting).
	float factor_disocclusion = get_factor_disocclusion(uv_reprojected, velocity);

	// LONGSHOT [patch] #113 A THIN THING SEEN IS HELD UNTIL IT IS SEEN AGAIN: where the history is locked - it comes from a pixel
	// that met a thin thing of geometry within the last LOCK_FRAMES frames, and nothing was uncovered since - the history is its
	// own texel, unclipped; the lock is renewed where this frame's sample meets such a thing, and fades where it does not
	vec4 history_plain = textureLod(tex_history, uv_reprojected, 0.0);
	// (A LOCK IS DROPPED BY ANY MOTION OF ITS OWN: the filter's disocclusion waits for two and a half texels a frame, and a
	// thing crossing a locked pixel slower than that - a walker far off, a rocket on its way out - would be drawn through
	// the held lines behind it. Where the pixel's motion is not the motion its history had, to a tenth of a texel, the
	// lock is gone and the clip stands as it always did. A still eye over still things has none; a turning eye carries
	// the lock with the reprojection, both motions the eye's own.)
	vec2 velocity_was = imageLoad(last_velocity_buffer, ivec2(uv_reprojected * params.resolution)).xy;
	float own_motion = length((velocity_was - velocity) * params.resolution);
	float held = clamp(history_plain.a, 0.0, 1.0) * (1.0 - factor_screen) * (1.0 - smoothstep(0.1, 0.4, own_motion));
	color_history = mix(color_history, finite_color(history_plain.rgb), held);
	float lock = max(thin_thing(pos_group), held - 1.0 / LOCK_FRAMES);

	// Compute blend factor
	float blend_factor = RPC_16; // We want to be able to accumulate as many jitter samples as we generated, that is, 16.
	{
		// Add to the blend factor
		blend_factor = clamp(blend_factor + factor_screen + factor_disocclusion, 0.0, 1.0);
	}

	// Resolve
	vec3 color_resolved = vec3(0.0);
	{
		// Tonemap (the clipped history lies within its neighbourhood's own range: [patch] #108)
		color_history = to_bounded(color_history);
		color_input = to_bounded(color_input);

		// Reduce flickering
		float lum_color = luminance(color_input);
		float lum_history = luminance(color_history);
		float diff = abs(lum_color - lum_history) / max(lum_color, max(lum_history, 1.001));
		// LONGSHOT [patch] #10 THE STEADY LIGHT CONVERGES: the guard weighed the blend by (1 - diff)^2, nought for a total
		// disagreement, so a pixel whose history was dark never took a bright input and kept not taking it every frame -
		// the centre of every bright point (a star, a sun's point under a pixel, a glint on a hull) drew black while its
		// skirt, a small disagreement, converged. A one-frame outlier is what the guard is for, and the variance clipping
		// above already pulls such a history back into its neighbourhood on the next frame. A half-way disagreement is
		// flicker (a thin bright feature alternating under the jitter: the same quarter weight at diff 0.5 as before); a
		// total one is a change, and a change converges at the blend's own rate
		float steady = 1.0 - 0.75 * 4.0 * diff * (1.0 - diff);
		// (LONGSHOT [patch] #113: THE GUARD STANDS FOR A LOCKED PIXEL TOO. It is what keeps a held line still: a hit, a
		// half-way disagreement with the line's own running mean, enters at a quarter of the blend. Taken off the locked
		// pixels - every sample at the blend's whole sixteenth - the held lattices and bars swung with the jitter's cycle:
		// measured 11-15, the worst tiles 5.6, 7.3 and 10.4 per cent against 1.8 and 2.5 with it.)
		blend_factor *= steady;

		// LONGSHOT [patch] #25 THE SETTLED GAS STANDS: a layer settled by its own history on the pixels' centres (the exhaust's gas: its
		// own motion, its own jitter undone) is kept as it stands, its share of the pixel its opacity - settled again here by the motion
		// of the surface behind it, it smeared over the ground and the sky as it moved across them, sixteen frames deep
		blend_factor = max(blend_factor, clamp(textureLod(settled_buffer, uv, 0.0).a, 0.0, 1.0));

		// Lerp/blend
		color_resolved = mix(color_history, color_input, blend_factor);

		// Out of the bounded space, under the buffer's top
		color_resolved = from_bounded(color_resolved);
	}

	return vec4(color_resolved, lock);
}

void main() {
	populate_group_shared_memory(gl_WorkGroupID.xy, gl_LocalInvocationIndex);

	// Out of bounds check
	if (any(greaterThanEqual(vec2(gl_GlobalInvocationID.xy), params.resolution))) {
		return;
	}

	const uvec2 pos_group = gl_LocalInvocationID.xy;
	const uvec2 pos_group_top_left = gl_WorkGroupID.xy * kGroupSize - kBorderSize;
	const uvec2 pos_screen = gl_GlobalInvocationID.xy;
	const vec2 uv = (gl_GlobalInvocationID.xy + 0.5f) / params.resolution;

	// (the lock in alpha is the HISTORY's alone: taa.cpp keeps it there and hands the frame on with an alpha of one, as before)
	vec4 result = temporal_antialiasing(pos_group_top_left, pos_group, pos_screen, uv, history_buffer);
	imageStore(output_buffer, ivec2(gl_GlobalInvocationID.xy), result);
}
