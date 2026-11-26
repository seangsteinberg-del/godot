/**************************************************************************/
/*  draw_report.cpp                                                       */
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

// LONGSHOT patch #121 (THE DRAW REPORT): see draw_report.h.

#include "draw_report.h"

#include "servers/rendering/renderer_geometry_instance.h"

Mutex DrawReport::mutex;
DrawReport::State DrawReport::state = DrawReport::STATE_IDLE;
bool DrawReport::collecting = false;
uint64_t DrawReport::frame = 0;
uint16_t DrawReport::view = 0;
uint8_t DrawReport::pass = DrawReport::PASS_OTHER;
uint32_t DrawReport::pass_count[DrawReport::PASS_MAX] = {};
LocalVector<DrawReport::Row> DrawReport::rows;
LocalVector<Size2i> DrawReport::view_sizes;
HashMap<const RenderGeometryInstanceBase *, uint32_t> DrawReport::pass_rows;

DrawReport::Updates DrawReport::updates;

const char *DrawReport::dirty_name(Dirty p_cause) {
	static const char *names[DIRTY_MAX] = { "skeleton", "material_override", "material_overlay", "surface_materials", "mesh_instance", "baked_light", "dynamic_gi", "ignore_gi", "shader_uniforms", "double_sided_shadows", "dependency_changed", "dependency_deleted", "created", "lightmap", "lightmap_capture", "softshadow_projector_pairing" };
	return p_cause < DIRTY_MAX ? names[p_cause] : "?";
}

const char *DrawReport::stale_name(Stale p_cause) {
	static const char *names[STALE_MAX] = { "light_changed", "caster_changed", "caster_moved_light_stood", "light_moved_caster_stood", "moved_apart", "caster_came", "caster_left" };
	return p_cause < STALE_MAX ? names[p_cause] : "?";
}

void DrawReport::take_updates(Updates &r_updates, uint64_t p_frame_now, uint64_t &r_frames) {
	MutexLock lock(mutex);
	r_updates = updates;
	r_frames = p_frame_now > updates.from_frame ? p_frame_now - updates.from_frame : 1;
	updates = Updates();
	updates.from_frame = p_frame_now;
}

void DrawReport::request() {
	MutexLock lock(mutex);
	if (state == STATE_COLLECTING) {
		return; // (a frame is being read: it is the asked one)
	}
	rows.clear();
	pass_rows.clear();
	state = STATE_ASKED;
}

bool DrawReport::ready() {
	MutexLock lock(mutex);
	return state == STATE_READY;
}

bool DrawReport::take(LocalVector<Row> &r_rows, uint32_t r_pass_count[PASS_MAX], LocalVector<Size2i> &r_view_sizes, uint64_t &r_frame) {
	MutexLock lock(mutex);
	if (state != STATE_READY) {
		return false;
	}
	r_rows = rows;
	for (uint32_t i = 0; i < PASS_MAX; i++) {
		r_pass_count[i] = pass_count[i];
	}
	r_view_sizes = view_sizes;
	r_frame = frame;
	rows.clear();
	pass_rows.clear();
	state = STATE_IDLE;
	return true;
}

void DrawReport::view_begin(uint64_t p_frame, const Size2i &p_size) {
	if (state == STATE_IDLE || state == STATE_READY) {
		return;
	}
	MutexLock lock(mutex);
	if (state == STATE_ASKED) {
		state = STATE_COLLECTING;
		collecting = true;
		frame = p_frame;
		view = 0;
		view_sizes.clear();
		for (uint32_t i = 0; i < PASS_MAX; i++) {
			pass_count[i] = 0;
		}
		rows.clear();
	} else if (state == STATE_COLLECTING && p_frame != frame) {
		state = STATE_READY;
		collecting = false;
		return;
	}
	if (state == STATE_COLLECTING) {
		view++;
		view_sizes.push_back(p_size);
		pass = PASS_OTHER;
		pass_rows.clear();
	}
}

void DrawReport::pass_begin(Pass p_pass) {
	pass = p_pass;
	pass_count[p_pass]++;
	pass_rows.clear();
}

DrawReport::Row &DrawReport::_row(const RenderGeometryInstanceBase *p_inst, uint32_t p_copies, bool p_indirect) {
	const uint32_t *at = pass_rows.getptr(p_inst);
	if (at) {
		return rows[*at];
	}
	Row r;
	r.view = view;
	r.pass = pass;
	r.pass_serial = pass_count[pass];
	r.copies = p_copies;
	r.indirect = p_indirect;
	if (p_inst->data) {
		r.object = p_inst->data->owner_object;
		r.instance = p_inst->data->owner_instance;
		r.base = p_inst->data->base.get_id();
		r.base_type = uint8_t(p_inst->data->base_type);
	}
	pass_rows[p_inst] = rows.size();
	rows.push_back(r);
	return rows[rows.size() - 1];
}

void DrawReport::primitives(const RenderGeometryInstanceBase *p_inst, uint32_t p_copies, bool p_indirect, uint64_t p_primitives) {
	_row(p_inst, p_copies, p_indirect).primitives += p_primitives;
}

void DrawReport::surface(const RenderGeometryInstanceBase *p_inst, uint32_t p_copies, bool p_indirect, bool p_alpha) {
	Row &r = _row(p_inst, p_copies, p_indirect);
	r.surfaces++;
	if (p_alpha) {
		r.alpha_surfaces++;
	}
}

void DrawReport::draw(const RenderGeometryInstanceBase *p_inst, uint32_t p_copies, bool p_indirect) {
	_row(p_inst, p_copies, p_indirect).draws++;
}
