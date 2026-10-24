/**************************************************************************/
/*  luminance.cpp                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "luminance.h"

#include "core/object/callable_mp.h"
#include "core/object/object.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

static const int EYE_TILE = 64; // the histogram pass's tile, pixels a side (luminance_reduce.glsl TILE)
static const int EYE_BINS = 64; // its bins (luminance_reduce.glsl BINS)
// THE SUBJECT IS A SURFACE (the fork's patch 47, kept as the meter's own definition): fewer lit pixels than this share of the frame's
// weight are no subject a matrix meter reads - the star points alone, a few glints - and the frame meters at the window's floor
static const float EYE_SUBJECT_MIN_SHARE = 0.005f;

Luminance::Luminance(bool p_prefer_raster_effects) {
	prefer_raster_effects = p_prefer_raster_effects;

	if (prefer_raster_effects) {
		Vector<String> luminance_reduce_modes;
		luminance_reduce_modes.push_back("\n#define FIRST_PASS\n"); // LUMINANCE_REDUCE_FRAGMENT_FIRST
		luminance_reduce_modes.push_back("\n"); // LUMINANCE_REDUCE_FRAGMENT
		luminance_reduce_modes.push_back("\n#define FINAL_PASS\n"); // LUMINANCE_REDUCE_FRAGMENT_FINAL

		luminance_reduce_raster.shader.initialize(luminance_reduce_modes);
		luminance_reduce_raster.shader_version = luminance_reduce_raster.shader.version_create();

		for (int i = 0; i < LUMINANCE_REDUCE_FRAGMENT_MAX; i++) {
			luminance_reduce_raster.pipelines[i].setup(luminance_reduce_raster.shader.version_get_shader(luminance_reduce_raster.shader_version, i), RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_disabled(), 0);
		}
	} else {
		// THE EYE IS A CAMERA (patch 57): the histogram pass and the resolve
		Vector<String> luminance_reduce_modes;
		luminance_reduce_modes.push_back("\n#define MODE_HISTOGRAM\n");
		luminance_reduce_modes.push_back("\n#define MODE_RESOLVE\n");

		luminance_reduce.shader.initialize(luminance_reduce_modes);
		luminance_reduce.shader_version = luminance_reduce.shader.version_create();

		for (int i = 0; i < LUMINANCE_REDUCE_MAX; i++) {
			luminance_reduce.pipelines[i] = RD::get_singleton()->compute_pipeline_create(luminance_reduce.shader.version_get_shader(luminance_reduce.shader_version, i));
		}

		for (int i = 0; i < LUMINANCE_REDUCE_FRAGMENT_MAX; i++) {
			luminance_reduce_raster.pipelines[i].clear();
		}
	}
}

Luminance::~Luminance() {
	if (prefer_raster_effects) {
		luminance_reduce_raster.shader.version_free(luminance_reduce_raster.shader_version);
	} else {
		luminance_reduce.shader.version_free(luminance_reduce.shader_version);
	}
}

void Luminance::LuminanceBuffers::set_prefer_raster_effects(bool p_prefer_raster_effects) {
	prefer_raster_effects = p_prefer_raster_effects;
}

void Luminance::LuminanceBuffers::configure(RenderSceneBuffersRD *p_render_buffers) {
	Size2i internal_size = p_render_buffers->get_internal_size();
	int w = internal_size.x;
	int h = internal_size.y;

	if (prefer_raster_effects) {
		while (true) {
			w = MAX(w / 8, 1);
			h = MAX(h / 8, 1);

			RD::TextureFormat tf;
			bool final = w == 1 && h == 1;
			tf.format = RD::DATA_FORMAT_R32_SFLOAT;
			tf.width = w;
			tf.height = h;
			tf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
			if (final) {
				tf.usage_bits |= RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
			}

			RID texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
			reduce.push_back(texture);

			if (final) {
				current = RD::get_singleton()->texture_create(tf, RD::TextureView());
				RD::get_singleton()->texture_clear(current, Color(0.0, 0.0, 0.0), 0u, 1u, 0u, 1u);
				break;
			}
		}
		return;
	}

	// THE EYE IS A CAMERA (patch 57): the tile histograms, the one result texel and the history texel - both readable back to the CPU
	tiles_x = (w + EYE_TILE - 1) / EYE_TILE;
	tiles_y = (h + EYE_TILE - 1) / EYE_TILE;
	tiles = RD::get_singleton()->storage_buffer_create(uint32_t(tiles_x) * uint32_t(tiles_y) * uint32_t(EYE_BINS) * 2u * sizeof(float));

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R32_SFLOAT;
	tf.width = 1;
	tf.height = 1;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	reduce.push_back(RD::get_singleton()->texture_create(tf, RD::TextureView()));
	current = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->texture_clear(current, Color(0.0, 0.0, 0.0), 0u, 1u, 0u, 1u);
}

void Luminance::LuminanceBuffers::free_data() {
	for (int i = 0; i < reduce.size(); i++) {
		RD::get_singleton()->free_rid(reduce[i]);
	}
	reduce.clear();

	if (current.is_valid()) {
		RD::get_singleton()->free_rid(current);
		current = RID();
	}
	if (tiles.is_valid()) {
		RD::get_singleton()->free_rid(tiles);
		tiles = RID();
	}
	tiles_x = 0;
	tiles_y = 0;
}

Ref<Luminance::LuminanceBuffers> Luminance::get_luminance_buffers(Ref<RenderSceneBuffersRD> p_render_buffers) {
	if (p_render_buffers->has_custom_data(RB_LUMINANCE_BUFFERS)) {
		return p_render_buffers->get_custom_data(RB_LUMINANCE_BUFFERS);
	}

	Ref<LuminanceBuffers> buffers;
	buffers.instantiate();
	buffers->set_prefer_raster_effects(prefer_raster_effects);
	buffers->configure(p_render_buffers.ptr());

	p_render_buffers->set_custom_data(RB_LUMINANCE_BUFFERS, buffers);

	return buffers;
}

RID Luminance::get_current_luminance_buffer(Ref<RenderSceneBuffersRD> p_render_buffers) {
	if (p_render_buffers->has_custom_data(RB_LUMINANCE_BUFFERS)) {
		Ref<LuminanceBuffers> buffers = p_render_buffers->get_custom_data(RB_LUMINANCE_BUFFERS);
		return buffers->current;
	}

	return RID();
}

// THE CENTRE MASK's integral over the frame (luminance_reduce.glsl's weight, a function of the normalized place alone: one mean for
// every aspect), so a share of the frame's weight is a number of the mask's units
static float eye_mask_integral(const Size2i &p_size) {
	const int nx = 64, ny = 36;
	double sum = 0.0;
	for (int j = 0; j < ny; j++) {
		for (int i = 0; i < nx; i++) {
			const double rx = (i + 0.5) / nx * 2.0 - 1.0, ry = (j + 0.5) / ny * 2.0 - 1.0;
			const double rr = Math::sqrt(rx * rx + ry * ry);
			sum += rr < 0.5 ? 1.0 : MAX(0.0, 1.0 - (rr - 0.5) * (rr - 0.5) / 0.8464);
		}
	}
	return float(sum / double(nx * ny) * double(p_size.x) * double(p_size.y));
}

// THE READBACK (patch 57): the device's asynchronous download of the one texel lands here a frame or two on; the buffers are found by
// their id so a viewport freed in between takes no call. The texel is the adapted luminance in THAT frame's buffer units (the scene
// times its exposure normalization, the pre-exposure); the normalization that frame rendered under is bound with the call and
// divided out here, so `measured` is the scene's own luminance whatever the pre-exposure has done since - a reading in the buffer's
// units read against a later frame's pre-exposure fed the script's lift back into its own meter
void Luminance::_eye_readback_done(const PackedByteArray &p_data, ObjectID p_buffers, float p_exposure_normalization) {
	Object *o = ObjectDB::get_instance(p_buffers);
	LuminanceBuffers *buffers = Object::cast_to<LuminanceBuffers>(o);
	if (buffers == nullptr || p_data.size() < int(sizeof(float))) {
		return;
	}
	float v = 0.0f;
	memcpy(&v, p_data.ptr(), sizeof(float));
	if (!Math::is_nan(v) && !Math::is_inf(v) && p_exposure_normalization > 0.0f) {
		buffers->measured = v / p_exposure_normalization;
	}
}

void Luminance::luminance_reduction(RID p_source_texture, RID p_excluded_texture, const Size2i p_source_size, Ref<LuminanceBuffers> p_luminance_buffers, float p_min_luminance, float p_max_luminance, float p_adapt_up, float p_adapt_down, bool p_set, float p_history_scale, float p_window_low, float p_window_high) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	// setup our uniforms
	RID default_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	if (prefer_raster_effects) {
		LuminanceReduceRasterPushConstant push_constant;
		memset(&push_constant, 0, sizeof(LuminanceReduceRasterPushConstant));

		push_constant.max_luminance = p_max_luminance;
		push_constant.min_luminance = p_min_luminance;
		push_constant.exposure_adjust = p_set ? 1.0f : p_adapt_up;
		push_constant.history_scale = p_history_scale > 0.0f ? p_history_scale : 1.0f; // THE METER'S HISTORY ACROSS THE LIFT (patch 17)

		for (int i = 0; i < p_luminance_buffers->reduce.size(); i++) {
			push_constant.source_size[0] = i == 0 ? p_source_size.x : push_constant.dest_size[0];
			push_constant.source_size[1] = i == 0 ? p_source_size.y : push_constant.dest_size[1];
			push_constant.dest_size[0] = MAX(push_constant.source_size[0] / 8, 1);
			push_constant.dest_size[1] = MAX(push_constant.source_size[1] / 8, 1);

			bool final = (push_constant.dest_size[0] == 1) && (push_constant.dest_size[1] == 1);
			LuminanceReduceRasterMode mode = final ? LUMINANCE_REDUCE_FRAGMENT_FINAL : (i == 0 ? LUMINANCE_REDUCE_FRAGMENT_FIRST : LUMINANCE_REDUCE_FRAGMENT);
			RID shader = luminance_reduce_raster.shader.version_get_shader(luminance_reduce_raster.shader_version, mode);

			RID framebuffer = FramebufferCacheRD::get_singleton()->get_cache(p_luminance_buffers->reduce[i]);

			RD::Uniform u_source_texture(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, i == 0 ? p_source_texture : p_luminance_buffers->reduce[i - 1] }));

			RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(framebuffer);
			RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, luminance_reduce_raster.pipelines[mode].get_render_pipeline(RD::INVALID_ID, RD::get_singleton()->framebuffer_get_format(framebuffer)));
			RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache(shader, 0, u_source_texture), 0);
			if (final) {
				RD::Uniform u_current_texture(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, p_luminance_buffers->current }));
				RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache(shader, 1, u_current_texture), 1);
			}

			RD::get_singleton()->draw_list_set_push_constant(draw_list, &push_constant, sizeof(LuminanceReduceRasterPushConstant));

			RD::get_singleton()->draw_list_draw(draw_list, false, 1u, 3u);
			RD::get_singleton()->draw_list_end();
		}

		SWAP(p_luminance_buffers->current, p_luminance_buffers->reduce.write[p_luminance_buffers->reduce.size() - 1]);
		return;
	}

	ERR_FAIL_COND(p_luminance_buffers->reduce.size() != 1 || !p_luminance_buffers->tiles.is_valid());

	LuminanceReducePushConstant push_constant;
	memset(&push_constant, 0, sizeof(LuminanceReducePushConstant));
	push_constant.source_size[0] = p_source_size.x;
	push_constant.source_size[1] = p_source_size.y;
	push_constant.tiles[0] = p_luminance_buffers->tiles_x;
	push_constant.tiles[1] = p_luminance_buffers->tiles_y;
	push_constant.max_luminance = p_max_luminance;
	push_constant.min_luminance = p_min_luminance;
	push_constant.adapt_up = p_set ? 1.0f : CLAMP(p_adapt_up, 0.0f, 1.0f);
	push_constant.adapt_down = p_set ? 1.0f : CLAMP(p_adapt_down, 0.0f, 1.0f);
	push_constant.history_scale = p_history_scale > 0.0f ? p_history_scale : 1.0f; // THE METER'S HISTORY ACROSS THE LIFT (patch 17)
	push_constant.window_low = CLAMP(p_window_low, 0.0f, 1.0f);
	push_constant.window_high = CLAMP(p_window_high, push_constant.window_low, 1.0f);
	push_constant.min_weight = EYE_SUBJECT_MIN_SHARE * eye_mask_integral(p_source_size);
	push_constant.has_excluded = p_excluded_texture.is_valid() ? 1.0f : 0.0f;

	RID histogram_shader = luminance_reduce.shader.version_get_shader(luminance_reduce.shader_version, LUMINANCE_HISTOGRAM);
	RID resolve_shader = luminance_reduce.shader.version_get_shader(luminance_reduce.shader_version, LUMINANCE_RESOLVE);

	RD::Uniform u_source_texture(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, p_source_texture }));
	RD::Uniform u_tiles(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_luminance_buffers->tiles);
	// THE PICTURE DECLARES ITSELF TO THE METER: the declared luminance, or the default black (unread: has_excluded is nought then)
	const RID excluded = p_excluded_texture.is_valid() ? p_excluded_texture : TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	RD::Uniform u_excluded_texture(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, excluded }));
	RD::Uniform u_result(RD::UNIFORM_TYPE_IMAGE, 0, p_luminance_buffers->reduce[0]);
	RD::Uniform u_current_texture(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, p_luminance_buffers->current }));

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

	// the histogram: one workgroup a tile
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, luminance_reduce.pipelines[LUMINANCE_HISTOGRAM]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(histogram_shader, 0, u_source_texture), 0);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(histogram_shader, 1, u_tiles), 1);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(histogram_shader, 2, u_excluded_texture), 2);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(LuminanceReducePushConstant));
	RD::get_singleton()->compute_list_dispatch(compute_list, p_luminance_buffers->tiles_x, p_luminance_buffers->tiles_y, 1);

	RD::get_singleton()->compute_list_add_barrier(compute_list);

	// the resolve: one workgroup
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, luminance_reduce.pipelines[LUMINANCE_RESOLVE]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(resolve_shader, 0, u_tiles), 0);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(resolve_shader, 1, u_result), 1);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(resolve_shader, 2, u_current_texture), 2);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(LuminanceReducePushConstant));
	RD::get_singleton()->compute_list_dispatch(compute_list, 1, 1, 1);

	RD::get_singleton()->compute_list_end();

	SWAP(p_luminance_buffers->current, p_luminance_buffers->reduce.write[0]);

	// THE READBACK: this frame's reading to the CPU, a frame or two on, with the exposure normalization this frame was metered under
	// (the renderer writes it to the buffers before the reduction: patch 17's history) so the fetch lands in the scene's units
	const float normalization = p_luminance_buffers->last_exposure_normalization > 0.0f ? p_luminance_buffers->last_exposure_normalization : 1.0f;
	Callable done = callable_mp_static(&Luminance::_eye_readback_done).bind(p_luminance_buffers->get_instance_id(), normalization);
	RD::get_singleton()->texture_get_data_async(p_luminance_buffers->current, 0, done);
}
