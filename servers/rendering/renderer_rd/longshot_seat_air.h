/**************************************************************************/
/*  longshot_seat_air.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

// LONGSHOT patch #130: THE AIR IN FRONT OF EVERY SURFACE - the contract between the renderer and whoever holds the law of the air
// about the eye (PROJECT LONGSHOT's sky module: modules/longshot_sky/sky_air_gd.h, the one text every reader of the seat's air
// pastes). The renderer knows NOTHING of the law: it includes a text by name, binds what the text declares, and calls one function.
//
// THE TEXT is registered with ShaderIncludeDB under INCLUDE before the renderer is made (a module's SERVERS level). With none
// registered the scene shader is upstream's own (no define, no stage). The text declares, in the render pass's set (SET):
//   bindings BINDING_FIRST .. BINDING_FIRST + TABLES - 1: the law's tables as combined samplers - the first TABLES_2D of them
//     sampler2D, the rest sampler3D (the renderer binds them with a linear sampler that clamps, and a stand-in where none is handed);
//   binding BINDING_BLOCK: one std140 block of BLOCK_BYTES bytes (the law's own numbers; the renderer never reads it).
// and defines the one function the scene shader's fragment calls:
//   vec3 longshot_seat_air(vec3 p_eye, vec3 p_dir, float p_dist, float p_lift, out vec3 r_pass)
//     p_eye   the eye in the world's frame, metres;
//     p_dir   the unit from the eye to the pixel's point, the world's axes;
//     p_dist  metres to it along p_dir (the pixel's own depth: 1e20 at the far plane);
//     p_lift  the frame's lift (scene_data.emissive_exposure_normalization: what the engine multiplied the surface's emission by);
//     r_pass  what the air passes of the surface's light;
//     returns the air's own light toward the eye, in the frame's units (lifted).
//
// A VIEW'S AIR is handed for a frame to its buffers (RenderSceneBuffersRD::set_longshot_air) before the opaque pass's set is made
// (a compositor effect's PRE_OPAQUE callback); a view handed none draws as upstream draws.
//
// WHO IS LEFT OUT: a material that disables fog (render_mode fog_disabled; BaseMaterial3D's disable_fog). The engine's fog stage
// is where the air is laid, and the switch that takes a material out of it is Godot's own.
namespace LongshotSeatAir {
static const char *const INCLUDE = "longshot/seat_air_inc.glsl";
enum {
	SET = 1,
	BINDING_FIRST = 42,
	TABLES = 7,
	TABLES_2D = 2,
	BINDING_BLOCK = BINDING_FIRST + TABLES,
	BLOCK_BYTES = 272,
};
} // namespace LongshotSeatAir
