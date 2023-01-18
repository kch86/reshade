
#include "bvh_manager.h"
#include "raytracing.h"

#include <reshade.hpp>

#include "hash.h"

using namespace reshade::api;
using namespace DirectX;

void bvh_manager::update()
{
	m_per_frame_instance_counts.clear();
}

void bvh_manager::destroy()
{
	m_geometry.clear();
	m_bvhs.clear();
	m_instances.clear();
}

void bvh_manager::update_vbs(std::span<const resource> buffers)
{
	s_current_draw_stream_hash = XXH3_64bits(buffers.data(), buffers.size_bytes());
}

void bvh_manager::on_geo_updated(resource res)
{
	//erase from our geometry and bvh list
	uint32_t count = m_geometry.size();
	for (uint32_t i = 0; i < count; i++)
	{
		if (m_geometry[i].vb.res == res.handle)
		{
			m_geometry[i] = m_geometry[count - 1];

			m_bvhs[i].free();
			m_bvhs[i] = std::move(m_bvhs[count - 1]);

			m_instances[i] = m_instances[count - 1];

			//since we moved the last one here, we need to check i again
			--i;

			// decrement count to match erasing 1 element
			--count;
		}
	}
	m_geometry.resize(count);
	m_bvhs.resize(count);
	m_instances.resize(count);
}

void bvh_manager::on_geo_draw(DrawDesc& desc)
{
	struct
	{
		BlasBuildDesc desc;
		uint64_t stream_hash;
	} combinedHashData = {
			.desc = desc.blas_desc,
			.stream_hash = s_current_draw_stream_hash
	};

	// combine the stream and draw data into one hash
	// use this to track instance count per frame
	XXH64_hash_t combined_hash = XXH3_64bits(&combinedHashData, sizeof(combinedHashData));
	uint32_t instanceIndex = 0;
	if (auto iter = m_per_frame_instance_counts.find(combined_hash); iter != m_per_frame_instance_counts.end())
	{
		instanceIndex = iter->second;
		iter->second++;
	}
	else
	{
		instanceIndex = 0;
		m_per_frame_instance_counts[combined_hash] = 0;
	}

	auto result = std::find_if(m_geometry.begin(), m_geometry.end(), [&](const BlasBuildDesc &d) {
		return
		d.vb.count == desc.blas_desc.vb.count &&
				d.vb.offset == desc.blas_desc.vb.offset &&
				d.vb.res == desc.blas_desc.vb.res &&
				d.ib.count == desc.blas_desc.ib.count &&
				d.ib.offset == desc.blas_desc.ib.offset &&
				d.ib.res == desc.blas_desc.ib.res;
		});
	if (result == m_geometry.end())
	{
		scopedresource bvh = buildBlas(desc.d3d9device, desc.cmd_list, desc.cmd_queue, desc.blas_desc);

		m_bvhs.push_back(std::move(bvh));
		m_instances.push_back({});
		m_instances.back().push_back(desc.transform); assert(instanceIndex == 0);
		m_geometry.push_back(desc.blas_desc);
	}
	else
	{
		uint32_t index = result - m_geometry.begin();
		if (instanceIndex < m_instances[index].size())
		{
			m_instances[index][instanceIndex] = desc.transform;
		}
		else
		{
			m_instances[index].push_back(desc.transform);
		}
	}
}

scopedresource bvh_manager::build_tlas(XMMATRIX* base_transform, command_list* cmd_list, command_queue* cmd_queue)
{
	if (m_bvhs.size() > 0)
	{
		std::vector<rt_instance_desc> instances;
		instances.reserve(m_bvhs.size());

		assert(m_bvhs.size() == m_instances.size());
		int totalInstanceCount = 0;
		for (size_t i = 0; i < m_instances.size(); i++)
		{
			assert(m_bvhs[i].handle().handle != 0);

			rt_instance_desc instance{};
			instance.acceleration_structure = { .buffer = m_bvhs[i].handle() };
			instance.instance_mask = 0xff;
			instance.flags = rt_instance_flags::none;

			const auto &instanceDatas = m_instances[i];
			for (const auto &instanceData : instanceDatas)
			{
				if (base_transform)
				{
					XMMATRIX inv_viewproj = XMMatrixInverse(nullptr, *base_transform);
					XMMATRIX wvp = instanceData;
					XMMATRIX world = inv_viewproj * wvp;

					memcpy(instance.transform, &world, sizeof(instance.transform));
				}
				else
				{
					instance.transform[0][0] = instance.transform[1][1] = instance.transform[2][2] = 1.0f;
				}
				instance.instance_id = totalInstanceCount;
				totalInstanceCount++;

				instances.push_back(instance);
			}
		}

		m_instances_flat = instances;

		TlasBuildDesc desc = {
			.instances = {m_instances_flat.data(), m_instances_flat.size() }
		};
		return buildTlas(cmd_list, cmd_queue, desc);
	}

	return {};
}
