layout(push_constant, std430) uniform PushConstant {
	ivec2 source_size;
	ivec2 dest_size;

	float exposure_adjust;
	float min_luminance;
	float max_luminance;
	float history_scale; // THE METER'S HISTORY ACROSS THE LIFT (patch 17)
}
settings;
