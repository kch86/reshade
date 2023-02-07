
#include "bvh_manager.h"
#include "raytracing.h"
#include "Shaders/RtShared.h"

#include <reshade.hpp>
#include "hash.h"

using namespace reshade::api;
using namespace DirectX;

void bvh_manager::update()
{
	m_per_frame_instance_counts.clear();
	m_frame_id++;
}

void bvh_manager::destroy()
{
	m_geometry.clear();
	m_geo_state.clear();
	m_bvhs.clear();
	m_instances.clear();
	m_attachments.clear();
	m_instances_flat.clear();
	m_attachments_flat.clear();
	m_per_frame_instance_counts.clear();
}

void bvh_manager::update_vbs(std::span<const resource> buffers)
{
	m_current_draw_stream_hash = XXH3_64bits(buffers.data(), buffers.size_bytes());
}

void bvh_manager::prune_stale_geo()
{
	// currently unused, leaving here for future use if I need it
	// erase from our geometry and bvh list
	uint32_t count = m_geometry.size();
	for (uint32_t i = 0; i < count; i++)
	{
		if (m_geo_state[i].needs_rebuild && (m_frame_id - m_geo_state[i].last_rebuild) > 100)
		{
			m_geometry[i] = m_geometry[count - 1];

			m_bvhs[i].free();
			m_bvhs[i] = std::move(m_bvhs[count - 1]);

			m_instances[i] = m_instances[count - 1];

			m_attachments[i].data.clear();
			m_attachments[i].data = std::move(m_attachments[count - 1].data);

			m_geo_state[i] = m_geo_state[count - 1];

			//since we moved the last one here, we need to check i again
			--i;

			// decrement count to match erasing 1 element
			--count;
		}
	}
	m_geometry.resize(count);
	m_bvhs.resize(count);
	m_instances.resize(count);
	m_attachments.resize(count);
	m_geo_state.resize(count);
}

void bvh_manager::on_geo_updated(resource res)
{
	// schedule a rebuild when geo is updated
	uint32_t count = m_geometry.size();
	for (uint32_t i = 0; i < count; i++)
	{
		if (m_geometry[i].vb.res == res.handle)
		{
			m_geo_state[i].needs_rebuild = true;
		}
	}
}

void bvh_manager::on_geo_draw(DrawDesc& desc)
{
	if (desc.dynamic)
		return;
	struct
	{
		BlasBuildDesc desc;
		uint64_t stream_hash;
	} combinedHashData = {
			.desc = desc.blas_desc,
			.stream_hash = m_current_draw_stream_hash
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
		m_per_frame_instance_counts[combined_hash] = 1;
	}

	auto result = std::find_if(m_geometry.begin(), m_geometry.end(), [&](const BlasBuildDesc &d) {
		if (desc.dynamic)
		{
			return d.vb.res == desc.blas_desc.vb.res && d.ib.res == desc.blas_desc.ib.res;
		}
		return 
			(d.vb.count == desc.blas_desc.vb.count &&
			d.vb.offset == desc.blas_desc.vb.offset &&
			d.vb.res == desc.blas_desc.vb.res &&
			d.ib.count == desc.blas_desc.ib.count &&
			d.ib.offset == desc.blas_desc.ib.offset &&
			d.ib.res == desc.blas_desc.ib.res);
		});

	if (result == m_geometry.end())
	{
		scopedresource bvh = buildBlas(desc.d3d9device, desc.cmd_list, desc.cmd_queue, desc.blas_desc);

		assert(instanceIndex == 0);
		m_bvhs.push_back(std::move(bvh));
		m_instances.push_back({});
		m_instances.back().push_back({
			.transform = desc.transform,
			.prev_transform = desc.transform,
			.mtrl = desc.material
		});
		m_geometry.push_back(desc.blas_desc);
		m_geo_state.push_back({
			.last_visible = m_frame_id,
			.last_rebuild = m_frame_id,
			.needs_rebuild = false,
			.dynamic = !desc.is_static
		});

		ScopedAttachment gpuattach;
		for (const AttachmentDesc &attachment : desc.attachments)
		{
			resource_view_flags flags = resource_view_flags::shader_visible;
			if (attachment.type == resource_type::buffer)
			{
				format fmt = attachment.fmt;
				uint32_t offset = attachment.offset;
				uint32_t count = attachment.count;
				uint32_t stride = attachment.stride;
				if (attachment.view_as_raw)
				{
					fmt = format::r32_uint;
					flags |= resource_view_flags::raw;
					assert((stride % sizeof(uint32_t)) == 0);
					offset *= stride / sizeof(uint32_t);
					count *= stride / sizeof(uint32_t);
				}
				resource_view_desc view_desc(fmt, offset, count);
				view_desc.flags = flags;

				resource_view srv{};
				if (attachment.res.handle != 0)
					desc.cmd_list->get_device()->create_resource_view(attachment.res, resource_usage::shader_resource, view_desc, &srv);

				ScopedAttachment::Elem data;
				data.srv = std::move(scopedresourceview(desc.cmd_list->get_device(), srv));
				data.offset = attachment.elem_offset;
				data.stride = stride;
				data.fmt = (uint32_t)attachment.fmt;
				gpuattach.data.push_back(std::move(data));
			}
			else if (attachment.type == resource_type::texture_2d)
			{
				resource_view srv{};
				if (attachment.res.handle != 0)
				{
					resource_desc res_desc = desc.cmd_list->get_device()->get_resource_desc(attachment.res);

					resource_view_desc view_desc(res_desc.texture.format);
					view_desc.flags = flags;

					desc.cmd_list->get_device()->create_resource_view(attachment.res, resource_usage::shader_resource, view_desc, &srv);
				}

				ScopedAttachment::Elem data;
				data.srv = std::move(scopedresourceview(desc.cmd_list->get_device(), srv));
				data.offset = 0;
				data.stride = 0;
				data.fmt = 0;
				gpuattach.data.push_back(std::move(data));
			}			
		}
		m_attachments.push_back(std::move(gpuattach));
	}
	else
	{
		const uint32_t index = result - m_geometry.begin();

		GeometryState &geostate = m_geo_state[index];
		geostate.last_visible = m_frame_id;

		if (geostate.needs_rebuild)
		{
			m_bvhs[index].free();
			m_bvhs[index] = std::move(buildBlas(desc.d3d9device, desc.cmd_list, desc.cmd_queue, desc.blas_desc));
			geostate.needs_rebuild = false;
			geostate.last_rebuild = m_frame_id;
		}
		if (instanceIndex < m_instances[index].size())
		{
			m_instances[index][instanceIndex].prev_transform = m_instances[index][instanceIndex].transform;
			m_instances[index][instanceIndex].transform = desc.transform;
			m_instances[index][instanceIndex].mtrl = desc.material;
		}
		else
		{
			m_instances[index].push_back({
				.transform = desc.transform,
				.mtrl = desc.material
			});
		}
	}
}

scopedresource bvh_manager::build_tlas(XMMATRIX* base_transform, command_list* cmd_list, command_queue* cmd_queue)
{
	if (m_bvhs.size() > 0)
	{
		std::vector<rt_instance_desc> instances;
		instances.reserve(m_bvhs.size());

		std::vector<Attachment> attachments;
		attachments.reserve(m_bvhs.size());

		std::vector<RtInstanceData> instance_data;
		instance_data.reserve(m_bvhs.size());

		assert(m_bvhs.size() == m_instances.size());

		int totalInstanceCount = 0;
		for (size_t i = 0; i < m_instances.size(); i++)
		{
			assert(m_bvhs[i].handle().handle != 0);

			const GeometryState &geostate = m_geo_state[i];
			if (geostate.needs_rebuild || (geostate.dynamic && geostate.last_visible != m_frame_id))
				continue;

			const BlasBuildDesc &blas_desc = m_geometry[i];

			rt_instance_desc instance{};
			instance.acceleration_structure = { .buffer = m_bvhs[i].handle() };
			//instance.instance_mask = 0xff;
			instance.instance_mask = blas_desc.opaque ? 1 : 2;
			instance.flags = rt_instance_flags::none;

			Attachment attachment;
			for (ScopedAttachment::Elem &elem : m_attachments[i].data)
			{
				attachment.data.push_back(Attachment::Elem{
					.srv = elem.srv.handle(),
					.offset = elem.offset,
					.stride = elem.stride,
					.fmt = elem.fmt
				});
			}

			auto &instanceDatas = m_instances[i];
			for (auto &instanceData : instanceDatas)
			{
				XMFLOAT3X4 toPrevWorldTransform;
				XMMATRIX toPrevWorldTransform4x4 = XMMatrixIdentity();
				if (base_transform)
				{
					//matrices are row major, so mult happens right to left
					XMMATRIX inv_viewproj = XMMatrixInverse(nullptr, *base_transform);
					XMMATRIX wvp = instanceData.transform;
					XMMATRIX world = inv_viewproj * wvp;

					// hack: update the instance's transform with this actual world transform
					// when the object is drawn again, we'll copy this transform  into prev_transform
					instanceData.transform = world;

					memcpy(instance.transform, &world, sizeof(instance.transform));

					// multiply by this frame's world inverse by the previous frame's world matrix
					toPrevWorldTransform4x4 = XMMatrixInverse(nullptr, world) * instanceData.prev_transform;
				}
				else
				{
					instance.transform[0][0] = instance.transform[1][1] = instance.transform[2][2] = 1.0f;
				}
				memcpy(&toPrevWorldTransform, &toPrevWorldTransform4x4, sizeof(XMFLOAT3X4));

				instance.instance_id = totalInstanceCount;
				totalInstanceCount++;

				instances.push_back(instance);				
				attachments.push_back(attachment);

				RtInstanceData rt_instance_data;
				rt_instance_data.diffuse = instanceData.mtrl.diffuse;
				rt_instance_data.specular = instanceData.mtrl.specular;
				rt_instance_data.roughness = instanceData.mtrl.roughness;
				rt_instance_data.toWorldPrevT = toPrevWorldTransform;
				rt_instance_data.flags = blas_desc.opaque ? 1 : 0;
				instance_data.push_back(rt_instance_data);
			}
		}

		m_instances_flat = instances;
		m_attachments_flat = attachments;
		m_instance_data_flat = instance_data;

		TlasBuildDesc desc = {
			.instances = {m_instances_flat.data(), m_instances_flat.size() }
		};
		return buildTlas(cmd_list, cmd_queue, desc);
	}

	return {};
}

std::pair<scopedresource, scopedresourceview> bvh_manager::build_attachments(reshade::api::command_list *cmd_list)
{
	if (m_attachments_flat.size() > 0)
	{
		device *d = cmd_list->get_device();

		// 1 "element" is however many attachments we have
		const uint32_t elem_byte_count = sizeof(RtInstanceAttachElem);
		const uint32_t attachment_count = m_attachments_flat[0].data.size();
		const uint32_t attachment_byte_count = elem_byte_count * attachment_count;
		const uint32_t total_count = m_attachments_flat.size();
		const uint32_t total_byte_count = total_count * attachment_byte_count;

		//size for the resource is in bytes
		resource_desc res_desc = resource_desc(total_byte_count, memory_heap::cpu_to_gpu, resource_usage::shader_resource);
		res_desc.buffer.stride = attachment_byte_count;
		res_desc.flags = resource_flags::structured;

		resource d3d12res;
		d->create_resource(
			res_desc,
			nullptr,
			resource_usage::cpu_access,
			&d3d12res);

		void *ptr;
		d->map_buffer_region(d3d12res, 0, total_byte_count, map_access::write_only, &ptr);

		RtInstanceAttachElem *data = (RtInstanceAttachElem *)ptr;
		for (uint32_t i = 0; i < m_attachments_flat.size(); i++)
		{
			for (uint32_t att = 0; att < attachment_count; att++)
			{
				const Attachment::Elem &elem = m_attachments_flat[i].data[att];
				if (elem.srv.handle != 0)
				{
					const uint32_t index = d->get_resource_view_descriptor_index(elem.srv);
					assert(index < d->get_descriptor_count(true));
					data[att].id = index;
				}
				else
				{
					data[att].id = 0x7FFFFFFF;
				}					

				data[att].offset = elem.offset;
				data[att].stride = elem.stride;
				data[att].format = elem.fmt;
			}
			data += attachment_count;
		}
		d->unmap_buffer_region(d3d12res);

		// size in element count not bytes
		resource_view_desc view_desc(format::unknown, 0, m_attachments_flat.size());
		view_desc.flags = resource_view_flags::structured;
		view_desc.buffer.stride = attachment_byte_count;

		resource_view srv;
		d->create_resource_view(d3d12res, resource_usage::shader_resource, view_desc, &srv);

		return { scopedresource(d, d3d12res), scopedresourceview(d, srv) };
	}
	return { scopedresource(), scopedresourceview() };
}

std::pair<scopedresource, scopedresourceview> bvh_manager::build_instance_data(reshade::api::command_list *cmd_list)
{
	if (m_instance_data_flat.size() > 0)
	{
		device *d = cmd_list->get_device();

		// 1 "element" is however many attachments we have
		const uint32_t elem_byte_count = sizeof(RtInstanceData);
		const uint32_t total_count = m_instance_data_flat.size();
		const uint32_t total_byte_count = total_count * elem_byte_count;

		//size for the resource is in bytes
		resource_desc res_desc = resource_desc(total_byte_count, memory_heap::cpu_to_gpu, resource_usage::shader_resource);
		res_desc.buffer.stride = elem_byte_count;
		res_desc.flags = resource_flags::structured;

		resource d3d12res;
		d->create_resource(
			res_desc,
			nullptr,
			resource_usage::cpu_access,
			&d3d12res);

		void *ptr;
		d->map_buffer_region(d3d12res, 0, total_byte_count, map_access::write_only, &ptr);
		memcpy(ptr, m_instance_data_flat.data(), total_byte_count);		
		d->unmap_buffer_region(d3d12res);

		// size in element count not bytes
		resource_view_desc view_desc(format::unknown, 0, m_attachments_flat.size());
		view_desc.flags = resource_view_flags::structured;
		view_desc.buffer.stride = elem_byte_count;

		resource_view srv;
		d->create_resource_view(d3d12res, resource_usage::shader_resource, view_desc, &srv);

		return { scopedresource(d, d3d12res), scopedresourceview(d, srv) };
	}
	return { scopedresource(), scopedresourceview() };
}
