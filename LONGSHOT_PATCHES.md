# LONGSHOT_PATCHES.md - every change to Godot's core in this fork (docs/GODOT_REBUILD_PROGRAM.md section 2)

One row per patch. No patch without the number that justified it. Each is a separate `[patch]` commit on
`longshot-4.7` so the branch rebases onto upstream releases; anything general is sent upstream.

| # | Where | What | The number | Upstream |
|---|---|---|---|---|
| 1 | `scene/3d/node_3d.cpp` `Node3D::look_at_from_position` | the "same position" guard compares with an ABSOLUTE epsilon (`distance_squared_to < CMP_EPSILON^2`) instead of `Vector3::is_equal_approx`, whose tolerance is 1e-5 of the coordinate | the precision proof (2026-09-21): at the pad, 6,371,839 m from the Earth-centred origin, `look_at` refused a target 12 m away (tolerance 64 m); on the Moon, 384,400 km away, the tolerance would be 3.8 km | to send: a true-scale fix any large-world project needs |
