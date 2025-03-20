#pragma once

#include <stdint.h>

enum MaterialType : unsigned int;

namespace mtrldb
{
	// types
	struct MaterialMapping
	{
		// offsets in float4 slots as in the shader disasm
		int diffuse_offset = -1;
		int specular_offset = -1;
		int specular_power_offset = -1;
		int env_power_offset = -1;
		int min_spec_offset = -1;
	};

	void load_db(const char *file);

	MaterialType get_material_type(uint64_t vshash, uint64_t pshash);
	int get_wvp_offset(uint64_t vshash);
	int get_albedo_tex_slot(uint64_t pshash);
	MaterialMapping &get_mtrl_constant_offsets(uint64_t vshash);
}
