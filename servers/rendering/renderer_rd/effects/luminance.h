/**************************************************************************/
/*  luminance.h                                                           */
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

#pragma once

#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/shaders/effects/luminance_reduce.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/luminance_reduce_raster.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

#define RB_LUMINANCE_BUFFERS SNAME("luminance_buffers")

namespace RendererRD {

// THE EYE IS A CAMERA (PROJECT LONGSHOT, the fork's patch 57, 10-24; docs/manual/sky.md THE EYE IS A CAMERA): the auto exposure's meter
// is a histogram of the lit pixels' log luminance read between two percentiles (luminance_reduce.glsl), adapted in log space at two
// speeds, and its reading is READ BACK to the CPU every frame so the game reads the eye's actual state (RenderSceneBuffersRD::
// get_eye_luminance, RenderingServer::viewport_get_eye_luminance). The mobile renderer's raster chain keeps the stock log-mean.
class Luminance {
private:
	bool prefer_raster_effects;

	enum LuminanceReduceMode {
		LUMINANCE_HISTOGRAM,
		LUMINANCE_RESOLVE,
		LUMINANCE_REDUCE_MAX
	};

	struct LuminanceReducePushConstant {
		int32_t source_size[2];
		int32_t tiles[2];
		float max_luminance;
		float min_luminance;
		float adapt_up;
		float adapt_down;
		float history_scale;
		float window_low;
		float window_high;
		float min_weight;
		float has_excluded;
	};

	struct LuminanceReduce {
		LuminanceReduceShaderRD shader;
		RID shader_version;
		RID pipelines[LUMINANCE_REDUCE_MAX];
	} luminance_reduce;

	enum LuminanceReduceRasterMode {
		LUMINANCE_REDUCE_FRAGMENT_FIRST,
		LUMINANCE_REDUCE_FRAGMENT,
		LUMINANCE_REDUCE_FRAGMENT_FINAL,
		LUMINANCE_REDUCE_FRAGMENT_MAX
	};

	struct LuminanceReduceRasterPushConstant {
		int32_t source_size[2];
		int32_t dest_size[2];
		float exposure_adjust;
		float min_luminance;
		float max_luminance;
		float history_scale; // THE METER'S HISTORY ACROSS THE LIFT (patch 17)
	};

	struct LuminanceReduceFragment {
		LuminanceReduceRasterShaderRD shader;
		RID shader_version;
		PipelineCacheRD pipelines[LUMINANCE_REDUCE_FRAGMENT_MAX];
	} luminance_reduce_raster;

	static void _eye_readback_done(const PackedByteArray &p_data, ObjectID p_buffers, float p_exposure_normalization);

public:
	class LuminanceBuffers : public RenderBufferCustomDataRD {
		GDCLASS(LuminanceBuffers, RenderBufferCustomDataRD);

	private:
		bool prefer_raster_effects;

	public:
		Vector<RID> reduce; // the raster chain's intermediates (the mobile renderer); the compute path's one result texel
		RID current;
		RID tiles; // the compute path's tile histograms: tiles_x * tiles_y * 64 bins * (weight, weighted log)
		int tiles_x = 0;
		int tiles_y = 0;
		float last_exposure_normalization = -1.0f; // THE METER'S HISTORY ACROSS THE LIFT (patch 17): the multiplier the history was metered under
		float measured = 0.0f; // THE READBACK: the last reading fetched to the CPU, in the SCENE's units - the frame's exposure normalization (the pre-exposure) divided out at the fetch, so a reading a frame or two late is still true (nought until the first lands)

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override;
		virtual void free_data() override;

		void set_prefer_raster_effects(bool p_prefer_raster_effects);
	};

	Ref<LuminanceBuffers> get_luminance_buffers(Ref<RenderSceneBuffersRD> p_render_buffers);
	RID get_current_luminance_buffer(Ref<RenderSceneBuffersRD> p_render_buffers);
	void luminance_reduction(RID p_source_texture, RID p_excluded_texture, const Size2i p_source_size, Ref<LuminanceBuffers> p_luminance_buffers, float p_min_luminance, float p_max_luminance, float p_adapt_up, float p_adapt_down, bool p_set = false, float p_history_scale = 1.0, float p_window_low = 0.8, float p_window_high = 0.983);

	Luminance(bool p_prefer_raster_effects);
	~Luminance();
};

} // namespace RendererRD
