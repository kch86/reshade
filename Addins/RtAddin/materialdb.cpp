#include "materialdb.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>

#include <fstream>
#include <unordered_set>
#include <unordered_map>

#include "Shaders/RtShared.h"
#include "hash.h"

namespace mtrldb
{
	struct SubMeshInfo
	{
		uint64_t vb_hash;
		uint64_t ib_hash;
		int index_offset;
		int index_count;
	};

	std::unordered_map<uint64_t, uint32_t> s_vs_transform_map;
	std::unordered_map<uint64_t, int> s_ps_texbinding_map;
	std::unordered_map<uint64_t, MaterialMapping> s_vs_material_map;
	std::unordered_map<uint64_t, MaterialType> s_material_type_map;
	std::unordered_map<uint64_t, MaterialType> s_material_submesh_type_map;

	const MaterialMapping& MaterialMapping::invalid()
	{
		static MaterialMapping ret{};
		return ret;
	}

	MaterialType get_enum(const char *str)
	{
		if (_strcmpi(str, "coat") == 0)
		{
			return Material_Coat;
		}
		else if (_strcmpi(str, "emissive") == 0)
		{
			return Material_Emissive;
		}

		return Material_Standard;
	}

	uint64_t get_combined_hash(uint64_t vshash, uint64_t pshash)
	{
		uint64_t data[2] = { vshash, pshash };

		return hash::hash(data, sizeof(data));
	}

	void load_db(const char* path)
	{
		using namespace rapidjson;

		std::string contents;
		if (auto file = std::ifstream(path))
		{
			contents = std::string(std::istreambuf_iterator<char>(file), {});
		}

		Document json;
		json.Parse<kParseCommentsFlag|kParseTrailingCommasFlag>(contents.c_str());
		assert(!json.HasParseError());
		ParseErrorCode error = json.GetParseError();
		size_t offset = json.GetErrorOffset();
		std::string location = contents.substr(offset, 32);
		assert(json.IsArray());

		for( auto& mtrl : json.GetArray())
		{
			const uint64_t vsguid = mtrl["vs_guid"].GetUint64();
			const uint64_t psguid = mtrl["ps_guid"].GetUint64();
			const uint64_t vbguid = mtrl["vb_guid"].GetUint64();
			const uint64_t ibguid = mtrl["ib_guid"].GetUint64();
			const MaterialType mtrltype = get_enum(mtrl["material"].GetString());
			const int transformslot = mtrl["transform_slot"].GetInt();
			const int albedoslot = mtrl["albedo_tex_slot"].GetInt();

			const auto& mtrl_map_obj = mtrl["constant_data"];
			const MaterialMapping mtrlmap = {
				mtrl_map_obj["diffuse_offset"].GetInt(),
				mtrl_map_obj["specular_offset"].GetInt(),
				mtrl_map_obj["specular_power_offset"].GetInt(),
				mtrl_map_obj["env_power_offset"].GetInt(),
				mtrl_map_obj["min_spec_offset"].GetInt(),
			};

			s_vs_transform_map[vsguid] = transformslot;
			s_ps_texbinding_map[psguid] = albedoslot;
			s_vs_material_map[vsguid] = mtrlmap;
			s_material_type_map[get_combined_hash(vsguid, psguid)] = mtrltype;

			// add sub mesh info
			if (mtrl.HasMember("sub_meshes"))
			{
				const auto &subMeshes = mtrl["sub_meshes"];
				for (const auto &subMesh : subMeshes.GetArray())
				{
					SubMeshInfo info = {
						.vb_hash = vbguid,
						.ib_hash = ibguid,
						.index_offset = subMesh["index_offset"].GetInt(),
						.index_count = subMesh["index_count"].GetInt(),
					};
					s_material_submesh_type_map[hash::hash(info)] = mtrltype;
				}
			}
		}
	}

	MaterialType get_material_type(uint64_t vshash, uint64_t pshash)
	{
		const uint64_t hash = get_combined_hash(vshash, pshash);

		if (auto entry = s_material_type_map.find(hash); entry != s_material_type_map.end())
		{
			return entry->second;
		}

		return Material_Standard;
	}

	MaterialType get_submesh_material(MaterialType basetype, uint64_t vbhash, uint64_t ibhash, int index_count, int index_offset)
	{
		SubMeshInfo info = {
			.vb_hash = vbhash,
			.ib_hash = ibhash,
			.index_offset = index_offset,
			.index_count = index_count,
		};
		const uint64_t hash = hash::hash(info);

		if (auto entry = s_material_submesh_type_map.find(hash); entry != s_material_submesh_type_map.end())
		{
			return entry->second;
		}

		return basetype;
	}

	int get_wvp_offset(uint64_t vshash)
	{
		if (auto entry = s_vs_transform_map.find(vshash); entry != s_vs_transform_map.end())
		{
			return entry->second;
		}

		return InvalidOffset;
	}

	int get_albedo_tex_slot(uint64_t pshash)
	{
		if (auto entry = s_ps_texbinding_map.find(pshash); entry != s_ps_texbinding_map.end())
		{
			return entry->second;
		}

		return InvalidSlot;
	}

	const MaterialMapping& get_mtrl_constant_offsets(uint64_t vshash)
	{
		if (auto entry = s_vs_material_map.find(vshash); entry != s_vs_material_map.end())
		{
			return entry->second;
		}

		return MaterialMapping::invalid();
	}

}
