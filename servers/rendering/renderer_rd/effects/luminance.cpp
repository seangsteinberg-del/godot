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

#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

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
		// Initialize luminance_reduce
		Vector<String> luminance_reduce_modes;
		luminance_reduce_modes.push_back("\n#define READ_TEXTURE\n");
		luminance_reduce_modes.push_back("\n");
		luminance_reduce_modes.push_back("\n#define WRITE_LUMINANCE\n");

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

	while (true) {
		w = MAX(w / 8, 1);
		h = MAX(h / 8, 1);

		RD::TextureFormat tf;
		bool final = w == 1 && h == 1;
		// THE METER READS THE SUBJECT (patch 20, Longshot): the intermediates carry the weighted log's mean and the weight's
		// mean (rg) and THE HIGHLIGHT (patch 21: the blocks' capped mean, then their max, b); the final the effective luminance
		// alone (r) as every reader expects
		tf.format = final ? RD::DATA_FORMAT_R32_SFLOAT : RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		tf.width = w;
		tf.height = h;

		if (prefer_raster_effects) {
			tf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		} else {
			tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT;
		}

		if (final) {
			tf.usage_bits |= RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		}

		RID texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
		reduce.push_back(texture);

		if (final) {
			current = RD::get_singleton()->texture_create(tf, RD::TextureView());
			RD::get_singleton()->texture_clear(current, Color(0.0, 0.0, 0.0), 0u, 1u, 0u, 1u);
			break;
		}
	}
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

// THE SUBJECT IS A SURFACE (patch 47, Longshot): the centre mask's integral over the frame (luminance_reduce.glsl's wc, a function of
// the normalized place alone: one mean for every aspect), so a share of the frame's weight is a number of the mask's units
static float subject_mask_integral(const Size2i &p_size) {
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

void Luminance::luminance_reduction(RID p_source_texture, const Size2i p_source_size, Ref<LuminanceBuffers> p_luminance_buffers, float p_min_luminance, float p_max_luminance, float p_adjust, bool p_set, float p_key_ratio, float p_scotopic_luminance, float p_photopic_luminance, float p_history_scale, float p_highlight_hold, float p_highlight_cap, float p_light_hold, float p_subject_floor, float p_subject_min_share) {
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
		push_constant.exposure_adjust = p_set ? 1.0f : p_adjust; // THE LOG-MEAN METER (patch 13): the immediate frame is the blend at one
		push_constant.history_scale = p_history_scale > 0.0f ? p_history_scale : 1.0f; // THE METER'S HISTORY ACROSS THE LIFT (patch 17)

		for (int i = 0; i < p_luminance_buffers->reduce.size(); i++) {
			push_constant.source_size[0] = i == 0 ? p_source_size.x : push_constant.dest_size[0];
			push_constant.source_size[1] = i == 0 ? p_source_size.y : push_constant.dest_size[1];
			push_constant.dest_size[0] = MAX(push_constant.source_size[0] / 8, 1);
			push_constant.dest_size[1] = MAX(push_constant.source_size[1] / 8, 1);

			bool final = (push_constant.dest_size[0] == 1) && (push_constant.dest_size[1] == 1); // the exponential's pass, set or not (patch 13)
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
	} else {
		LuminanceReducePushConstant push_constant;
		memset(&push_constant, 0, sizeof(LuminanceReducePushConstant));

		push_constant.source_size[0] = p_source_size.x;
		push_constant.source_size[1] = p_source_size.y;
		push_constant.max_luminance = p_max_luminance;
		push_constant.min_luminance = p_min_luminance;
		push_constant.exposure_adjust = p_set ? 1.0f : p_adjust; // THE LOG-MEAN METER (patch 13): the immediate frame is the blend at one
		// THE KEY FOLLOWS THE ADAPTATION (patch 13): the law's three numbers, or none (a ratio of one, an empty range)
		const bool key_law = p_key_ratio > 1.0f && p_scotopic_luminance > 0.0f && p_photopic_luminance > p_scotopic_luminance;
		push_constant.key_ratio = key_law ? p_key_ratio : 1.0f;
		push_constant.ln_scotopic = key_law ? Math::log(p_scotopic_luminance) : 0.0f;
		push_constant.ln_photopic = key_law ? Math::log(p_photopic_luminance) : 0.0f;
		push_constant.history_scale = p_history_scale > 0.0f ? p_history_scale : 1.0f; // THE METER'S HISTORY ACROSS THE LIFT (patch 17)
		push_constant.highlight_hold = MAX(p_highlight_hold, 0.0f); // THE HIGHLIGHT HOLDS THE WINDOW (patch 21)
		push_constant.highlight_cap = p_highlight_cap > 0.0f ? p_highlight_cap : 65504.0f;
		push_constant.light_hold = MAX(p_light_hold, 0.0f); // THE FILLED LIGHT (patch 22)
		push_constant.subject_floor = MAX(p_subject_floor, 0.0f); // THE SUBJECT STANDS OVER THE BACKDROP (patch 43)
		push_constant.subject_min_weight = p_subject_min_share > 0.0f ? p_subject_min_share * subject_mask_integral(p_source_size) : 0.0f; // THE SUBJECT IS A SURFACE (patch 47)
		push_constant.level = 0;

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

		for (int i = 0; i < p_luminance_buffers->reduce.size(); i++) {
			RID shader;

			if (i == 0) {
				shader = luminance_reduce.shader.version_get_shader(luminance_reduce.shader_version, LUMINANCE_REDUCE_READ);
				RD::Uniform u_source_texture(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, p_source_texture }));

				RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, luminance_reduce.pipelines[LUMINANCE_REDUCE_READ]);
				RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_source_texture), 0);
			} else {
				RD::get_singleton()->compute_list_add_barrier(compute_list); //needs barrier, wait until previous is done

				if (i == p_luminance_buffers->reduce.size() - 1) { // the exponential's pass, set or not (patch 13)
					shader = luminance_reduce.shader.version_get_shader(luminance_reduce.shader_version, LUMINANCE_REDUCE_WRITE);
					RD::Uniform u_current_texture(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, p_luminance_buffers->current }));

					RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, luminance_reduce.pipelines[LUMINANCE_REDUCE_WRITE]);
					RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 2, u_current_texture), 2);
				} else {
					shader = luminance_reduce.shader.version_get_shader(luminance_reduce.shader_version, LUMINANCE_REDUCE);
					RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, luminance_reduce.pipelines[LUMINANCE_REDUCE]);
				}

				RD::Uniform u_source_texture(RD::UNIFORM_TYPE_IMAGE, 0, p_luminance_buffers->reduce[i - 1]);
				RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_source_texture), 0);
			}

			RD::Uniform u_reduce_texture(RD::UNIFORM_TYPE_IMAGE, 0, p_luminance_buffers->reduce[i]);
			RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_reduce_texture), 1);

			push_constant.level = i; // THE HIGHLIGHT HOLDS THE WINDOW (patch 21): the pass's rung
			RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(LuminanceReducePushConstant));

			RD::get_singleton()->compute_list_dispatch_threads(compute_list, push_constant.source_size[0], push_constant.source_size[1], 1);

			push_constant.source_size[0] = MAX(push_constant.source_size[0] / 8, 1);
			push_constant.source_size[1] = MAX(push_constant.source_size[1] / 8, 1);
		}

		RD::get_singleton()->compute_list_end();
	}

	SWAP(p_luminance_buffers->current, p_luminance_buffers->reduce.write[p_luminance_buffers->reduce.size() - 1]);
}
