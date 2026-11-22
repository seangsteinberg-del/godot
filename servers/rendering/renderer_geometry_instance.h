/**************************************************************************/
/*  renderer_geometry_instance.h                                          */
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

#include "core/math/color.h"
#include "core/math/rect2.h"
#include "core/math/transform_3d.h"
#include "core/templates/rid.h"
#include "servers/rendering/storage/utilities.h"

// API definition for our RenderGeometryInstance class so we can expose this through GDExtension in the near future
class RenderGeometryInstance {
public:
	virtual ~RenderGeometryInstance() {}

	virtual void _mark_dirty() = 0;

	virtual void set_skeleton(RID p_skeleton) = 0;
	virtual void set_material_override(RID p_override) = 0;
	virtual void set_material_overlay(RID p_overlay) = 0;
	virtual void set_surface_materials(const Vector<RID> &p_materials) = 0;
	virtual void set_mesh_instance(RID p_mesh_instance) = 0;
	virtual void set_transform(const Transform3D &p_transform, const AABB &p_aabb, const AABB &p_transformed_aabb) = 0;
	virtual void set_pivot_data(float p_sorting_offset, bool p_use_aabb_center) = 0;
	virtual void set_lod_bias(float p_lod_bias) = 0;
	virtual void set_layer_mask(uint32_t p_layer_mask) = 0;
	virtual void set_fade_range(bool p_enable_near, float p_near_begin, float p_near_end, bool p_enable_far, float p_far_begin, float p_far_end) = 0;
	virtual void set_parent_fade_alpha(float p_alpha) = 0;
	virtual void set_transparency(float p_transparency) = 0;
	virtual void set_use_baked_light(bool p_enable) = 0;
	virtual void set_use_dynamic_gi(bool p_enable) = 0;
	virtual void set_ignore_gi(bool p_enable) = 0; // LONGSHOT patch #4
	virtual void set_use_lightmap(RID p_lightmap_instance, const Rect2 &p_lightmap_uv_scale, int p_lightmap_slice_index) = 0;
	virtual void set_lightmap_capture(const Color *p_sh9) = 0;
	virtual void set_instance_shader_uniforms_offset(int32_t p_offset) = 0;
	virtual void set_cast_double_sided_shadows(bool p_enable) = 0;

	virtual void reset_motion_vectors() = 0;

	virtual Transform3D get_transform() = 0;
	virtual AABB get_aabb() = 0;

	virtual void clear_light_instances() = 0;
	virtual void pair_light_instance(const RID p_light_instance, RSE::LightType light_type, uint32_t placement_idx) = 0;
	virtual void pair_reflection_probe_instances(const RID *p_reflection_probe_instances, uint32_t p_reflection_probe_instance_count) = 0;
	virtual void pair_decal_instances(const RID *p_decal_instances, uint32_t p_decal_instance_count) = 0;
	virtual void pair_voxel_gi_instances(const RID *p_voxel_gi_instances, uint32_t p_voxel_gi_instance_count) = 0;

	virtual void set_softshadow_projector_pairing(bool p_softshadow, bool p_projector) = 0;
};

// Base implementation of RenderGeometryInstance shared by internal renderers.
class RenderGeometryInstanceBase : public RenderGeometryInstance {
public:
	// setup
	uint32_t base_flags = 0;
	uint32_t flags_cache = 0;

	// used during rendering
	float depth = 0;

	RID mesh_instance;

	Transform3D transform;
	// LONGSHOT patch #116 (THE PREVIOUS PLACE IS WHERE IT WAS LAST DRAWN): an instance's motion runs from the place the LAST FRAME LAID IT
	// AT to the place this frame lays it at. The engine told it from the moment the transform was SET: the previous place was
	// latched in set_transform by the frame's number, and an instance whose last set bore an earlier number than the frame
	// drawn was taken as not having moved. The number advances when the draw begins - so a transform that reached the instance
	// BEFORE the draw began (anything that updates the dirty instances early does it: a cull asked of the server, an instance
	// freed - RendererSceneCull::instances_cull_*, ::free) bore the last frame's number, and the instance was drawn as standing
	// still in the world. Under a camera that travels with it - a craft in flight, 190 m a frame in orbit - that wrote the
	// camera's whole step as the craft's motion: the temporal filter looked its history up off the screen and kept none, and
	// every thin thing of the hull flickered with the jitter (measured 11-22, game/tests/probe_hull_still.gd: 0.06 % of a
	// lander's pixels moved between two frames on the pad, 7.2 % in orbit; probe_motion_latch.gd: a box riding with the camera
	// drew no motion as the game ran and the craft's own motion once the dirty instances were flushed early).
	// THE LAW: `motion_latch`, called where a frame first lays the instance in a render list (after every transform of the frame
	// has reached it, whenever it did): the previous place is the place the last frame laid it at; an instance the last frame
	// did not lay, or whose motion was reset since (reset_motion_vectors: a teleport), has no motion of its own this frame.
	Transform3D prev_transform; // where the last frame laid it (this frame's place where it has no motion of its own)
	Transform3D laid_transform; // where it is laid in `laid_frame`
	uint64_t laid_frame = 0;
	bool laid_before = false;
	bool moved = false; // it stands elsewhere than the last frame laid it: it writes a motion of its own
	bool motion_reset = false; // reset_motion_vectors since it was last laid
	_FORCE_INLINE_ void motion_latch(uint64_t p_frame) {
		if (likely(laid_before && laid_frame == p_frame)) {
			return; // (laid already this frame: another pass, another viewport)
		}
		const bool follows = laid_before && laid_frame + 1 == p_frame && !motion_reset;
		prev_transform = follows ? laid_transform : transform;
		moved = follows && prev_transform != transform;
		laid_transform = transform;
		laid_frame = p_frame;
		laid_before = true;
		motion_reset = false;
	}
	bool mirror = false;
	AABB transformed_aabb;
	bool non_uniform_scale = false;
	float lod_model_scale = 1.0;
	float lod_bias = 0.0;
	float sorting_offset = 0.0;
	bool use_aabb_center = true;

	uint32_t layer_mask = 1;

	bool fade_near = false;
	float fade_near_begin = 0;
	float fade_near_end = 0;
	bool fade_far = false;
	float fade_far_begin = 0;
	float fade_far_end = 0;

	float parent_fade_alpha = 1.0;
	float force_alpha = 1.0;

	int32_t shader_uniforms_offset = -1;

	struct Data {
		//data used less often goes into regular heap
		RID base;
		RSE::InstanceType base_type;

		RID skeleton;
		Vector<RID> surface_materials;
		RID material_override;
		RID material_overlay;
		AABB aabb;

		bool use_baked_light = false;
		bool use_dynamic_gi = false;
		bool ignore_gi = false; // LONGSHOT patch #4: no SDFGI, no VoxelGI, no GI buffers on this instance
		bool cast_double_sided_shadows = false;
		bool dirty_dependencies = false;

		DependencyTracker dependency_tracker;
	};

	Data *data = nullptr;

	virtual void set_skeleton(RID p_skeleton) override;
	virtual void set_material_override(RID p_override) override;
	virtual void set_material_overlay(RID p_overlay) override;
	virtual void set_surface_materials(const Vector<RID> &p_materials) override;
	virtual void set_mesh_instance(RID p_mesh_instance) override;
	virtual void set_transform(const Transform3D &p_transform, const AABB &p_aabb, const AABB &p_transformed_aabb) override;
	virtual void set_pivot_data(float p_sorting_offset, bool p_use_aabb_center) override;
	virtual void set_lod_bias(float p_lod_bias) override;
	virtual void set_layer_mask(uint32_t p_layer_mask) override;
	virtual void set_fade_range(bool p_enable_near, float p_near_begin, float p_near_end, bool p_enable_far, float p_far_begin, float p_far_end) override;
	virtual void set_parent_fade_alpha(float p_alpha) override;
	virtual void set_transparency(float p_transparency) override;
	virtual void set_use_baked_light(bool p_enable) override;
	virtual void set_use_dynamic_gi(bool p_enable) override;
	virtual void set_ignore_gi(bool p_enable) override;
	virtual void set_instance_shader_uniforms_offset(int32_t p_offset) override;
	virtual void set_cast_double_sided_shadows(bool p_enable) override;

	virtual void reset_motion_vectors() override;

	virtual Transform3D get_transform() override;
	virtual AABB get_aabb() override;
};
