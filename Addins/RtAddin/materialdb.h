#pragma once

#include <stdint.h>

enum MaterialType : unsigned int;

namespace mtrldb
{
	static constexpr int InvalidOffset = -1;
	static constexpr int InvalidSlot = -1;

	// types
	struct MaterialMapping
	{
		// offsets in float4 slots as in the shader disasm
		int diffuse_offset = InvalidOffset;
		int specular_offset = InvalidOffset;
		int specular_power_offset = InvalidOffset;
		int env_power_offset = InvalidOffset;
		int min_spec_offset = InvalidOffset;

		bool operator ==(const MaterialMapping& other) const = default;

		static const MaterialMapping& invalid();
	};


	void load_db(const char *file);

	MaterialType get_material_type(uint64_t vshash, uint64_t pshash);
	MaterialType get_submesh_material(MaterialType basetype, uint64_t vbhash, uint64_t ibhash, int index_count, int index_offset);
	int get_wvp_offset(uint64_t vshash);
	int get_albedo_tex_slot(uint64_t pshash);
	const MaterialMapping& get_mtrl_constant_offsets(uint64_t vshash);
}
