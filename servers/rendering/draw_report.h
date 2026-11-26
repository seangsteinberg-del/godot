/**************************************************************************/
/*  draw_report.h                                                         */
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

// LONGSHOT patch #121 (THE DRAW REPORT): the renderer's own account of what it drew, by instance and by pass, FOR ONE ASKED
// FRAME. The engine's counters (Viewport.get_render_info) say how many objects, draw calls and primitives a frame's picture and
// its shadows held - never whose. This collector is filled beside those counters, at the same lines, with one row for every
// instance in every pass's list of every view the asked frame drew: the owner's object id and the scene instance (given to a
// geometry instance by the cull at its making), its base, the surfaces listed, the draw calls begun, the primitives as the
// engine counts them. A game names the owners; the engine names none. Unasked, a list costs one test of a flag.
//
// A view's rows carry which view of the frame they are (1 the first drawn), and the report its views' sizes (patch #123): the
// window's own view is told from a seat's lens or a probe by it.
// The asked frame: request() asks; the next view that begins opens the collection and names its frame; the first view of a
// LATER frame closes it (ready() is then true and take() hands the rows). A frame that draws no 3D view leaves it open.

#include "core/math/vector2i.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

class RenderGeometryInstanceBase;

class DrawReport {
public:
	enum Pass : uint8_t {
		PASS_PICTURE, // the view's own lists: the opaque, the motion's and the transparent
		PASS_SUN_1, // a directional light's cascades, the nearest first
		PASS_SUN_2,
		PASS_SUN_3,
		PASS_SUN_4,
		PASS_LIGHT, // a local light's shadow map (a spot's one, an omni's each side)
		PASS_SDFGI,
		PASS_OTHER, // a material's capture, a lightmap's, a collider's height field
		PASS_MAX,
	};

	struct Row {
		uint64_t object = 0; // the owner's object id (0: the instance's maker attached none)
		uint64_t instance = 0; // the scene instance's RID id
		uint64_t base = 0; // the base's RID id: a mesh, a multimesh, particles
		uint32_t pass_serial = 0; // which of the frame's passes of this kind (the nth shadow map drawn)
		uint32_t surfaces = 0; // the surfaces the pass's lists hold of it ...
		uint32_t alpha_surfaces = 0; // ... those of them in the transparent list
		uint32_t draws = 0; // the draw calls it begins (a repeat of the surface before it begins none)
		uint32_t copies = 0; // a multimesh's instances as the renderer holds them (1: one mesh)
		uint64_t primitives = 0; // as the engine's own counter counts them (an indirect multimesh's are the device's: 0 here)
		uint16_t view = 0; // which of the frame's 3D views
		uint8_t pass = 0;
		uint8_t base_type = 0; // RSE::InstanceType
		bool indirect = false; // drawn from a command buffer: its counts are its maker's
	};

	// LONGSHOT patch #123 (THE REPORT'S UPDATES): what the scene and the renderer re-make a frame, and why - counted ALWAYS (an
	// increment each) and handed out as sums since the last hand-out, with the frames they span:
	//   the scene instances updated (RendererSceneCull::_update_dirty_instance: moved, shown, re-based), by kind;
	//   the pairs made and broken (an instance with a light, a probe, a decal);
	//   the geometry instances whose draw data was rebuilt (the forward renderer's _geometry_instance_update);
	//   every mark for such a rebuild, by its cause.
	enum Dirty : uint8_t {
		DIRTY_SKELETON,
		DIRTY_MATERIAL_OVERRIDE,
		DIRTY_MATERIAL_OVERLAY,
		DIRTY_SURFACE_MATERIALS,
		DIRTY_MESH_INSTANCE,
		DIRTY_BAKED_LIGHT,
		DIRTY_DYNAMIC_GI,
		DIRTY_IGNORE_GI,
		DIRTY_SHADER_UNIFORMS,
		DIRTY_DOUBLE_SIDED,
		DIRTY_DEPENDENCY_CHANGED,
		DIRTY_DEPENDENCY_DELETED,
		DIRTY_CREATED,
		DIRTY_LIGHTMAP,
		DIRTY_LIGHTMAP_CAPTURE,
		DIRTY_PAIRING,
		DIRTY_MAX,
	};
	enum Updated : uint8_t {
		UPDATED_GEOMETRY,
		UPDATED_LIGHT,
		UPDATED_OTHER,
		UPDATED_MAX,
	};
	struct Updates {
		uint64_t marks[DIRTY_MAX] = {};
		uint64_t updated[UPDATED_MAX] = {};
		uint64_t pairs = 0;
		uint64_t unpairs = 0;
		uint64_t rebuilt = 0;
		uint64_t from_frame = 0; // the frame the sums began at
	};
	static Updates updates;
	_FORCE_INLINE_ static void dirty(Dirty p_cause) { updates.marks[p_cause]++; }
	static const char *dirty_name(Dirty p_cause);
	// the sums since the last hand-out and the frames they span; the sums begin again
	static void take_updates(Updates &r_updates, uint64_t p_frame_now, uint64_t &r_frames);

private:
	enum State : uint8_t {
		STATE_IDLE,
		STATE_ASKED,
		STATE_COLLECTING,
		STATE_READY,
	};

	static Mutex mutex;
	static State state;
	static bool collecting; // (the hooks' one test)
	static uint64_t frame;
	static uint16_t view;
	static uint8_t pass;
	static uint32_t pass_count[PASS_MAX];
	static LocalVector<Row> rows;
	static LocalVector<Size2i> view_sizes; // LONGSHOT patch #123: each view's size in pixels (its internal one), the first view's first
	static HashMap<const RenderGeometryInstanceBase *, uint32_t> pass_rows; // the pass in hand's rows, by instance

	static Row &_row(const RenderGeometryInstanceBase *p_inst, uint32_t p_copies, bool p_indirect);

public:
	// the asker's side
	static void request();
	static bool ready();
	// the rows, the passes drawn of each kind, the views and the frame's number; false while none stands ready
	static bool take(LocalVector<Row> &r_rows, uint32_t r_pass_count[PASS_MAX], LocalVector<Size2i> &r_view_sizes, uint64_t &r_frame);

	// the renderer's side
	_FORCE_INLINE_ static bool on() { return collecting; }
	static void view_begin(uint64_t p_frame, const Size2i &p_size); // every 3D view's begin, asked or not
	static void pass_begin(Pass p_pass);
	static void primitives(const RenderGeometryInstanceBase *p_inst, uint32_t p_copies, bool p_indirect, uint64_t p_primitives);
	static void surface(const RenderGeometryInstanceBase *p_inst, uint32_t p_copies, bool p_indirect, bool p_alpha);
	static void draw(const RenderGeometryInstanceBase *p_inst, uint32_t p_copies, bool p_indirect);
};
