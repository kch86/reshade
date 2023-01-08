#include "d3d12_impl_raytracing.hpp"
#include "d3d12_impl_device.hpp"
#include "d3d12_impl_type_convert.hpp"
#include "addon.hpp"

namespace reshade::d3d12
{
	D3D12_ELEMENTS_LAYOUT to_native(api::rt_elements_layout layout)
	{
		return (D3D12_ELEMENTS_LAYOUT)layout;
	}

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS to_native(api::rt_acceleration_structure_build_flags flags)
	{
		return (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS)flags;
	}

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE to_native(api::rt_acceleration_structure_type type)
	{
		return (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE)type;
	}

	DXGI_FORMAT to_native(reshade::api::format value)
	{
		return static_cast<DXGI_FORMAT>(value);
	}

	D3D12_RAYTRACING_GEOMETRY_TYPE to_native(api::rt_geometry_type type)
	{
		return (D3D12_RAYTRACING_GEOMETRY_TYPE)type;
	}

	D3D12_RAYTRACING_GEOMETRY_FLAGS to_native(api::rt_geometry_flags flags)
	{
		return (D3D12_RAYTRACING_GEOMETRY_FLAGS)flags;
	}

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE to_native(api::rt_acceleration_structure_postbuild_info_type type)
	{
		return (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE)type;
	}

	D3D12_RAYTRACING_INSTANCE_FLAGS to_native(api::rt_instance_flags flags)
	{
		return (D3D12_RAYTRACING_INSTANCE_FLAGS)flags;
	}
}

auto reshade::d3d12::convert_rt_build_inputs(
	reshade::d3d12::device_impl* device,
	const api::rt_build_acceleration_structure_inputs &input,
	span<D3D12_RAYTRACING_GEOMETRY_DESC> geom_desc_storage) -> D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS
{
	// todo: support different layouts?
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.DescsLayout = to_native(input.descs_layout);
	inputs.Flags = to_native(input.flags);
	inputs.NumDescs = input.desc_count;
	inputs.Type = to_native(input.type);

	if (input.type == api::rt_acceleration_structure_type::top_level)
	{
		if (input.instances.instances_buffer.buffer.handle != 0)
		{
			D3D12_RAYTRACING_INSTANCE_DESC *instancesPtr;
			uint64_t size_to_map = sizeof(api::rt_instance_desc) * input.desc_count;
			device->map_buffer_region(input.instances.instances_buffer.buffer, input.instances.instances_buffer.offset, size_to_map, api::map_access::write_only, (void **)&instancesPtr);

			for (uint32_t i = 0; i < input.desc_count; i++)
			{
				const api::rt_instance_desc &src = input.instances.instance_descs[i];

				D3D12_RAYTRACING_INSTANCE_DESC &dst = instancesPtr[i];
				dst = {};
				dst.AccelerationStructure = to_native_gpu(src.acceleration_structure.buffer) + src.acceleration_structure.offset;
				dst.InstanceContributionToHitGroupIndex = src.instance_contribution_to_hit_group_index;
				dst.Flags = to_native(src.flags);
				dst.InstanceID = src.instance_id;
				dst.InstanceMask = src.instance_mask;
				memcpy(dst.Transform, src.transform, sizeof(dst.Transform));
			}

			device->unmap_buffer_region(input.instances.instances_buffer.buffer);

			inputs.InstanceDescs = to_native_gpu(input.instances.instances_buffer.buffer) + input.instances.instances_buffer.offset;
		}		
	}
	else
	{
		assert(geom_desc_storage.size() == input.desc_count);
		for (uint32_t i = 0; i < input.desc_count; i++)
		{
			const api::rt_geometry_desc &desc = input.geometry_desc_array[i];

			D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
			if (desc.Type == api::rt_geometry_type::procedural)
			{
				const api::rt_geometry_aabb_desc &aabb = desc.AABBs;
				geomDesc.AABBs.AABBCount = aabb.AABBCount;
				geomDesc.AABBs.AABBs.StartAddress = to_native_gpu(aabb.AABBs.buffer) + aabb.AABBs.offset;
				geomDesc.AABBs.AABBs.StrideInBytes = aabb.AABBs.stride;
			}
			else
			{
				const api::rt_geometry_triangle_desc &triangle = desc.Triangles;
				geomDesc.Triangles.VertexBuffer.StartAddress = to_native_gpu(triangle.vertex_buffer.buffer) + triangle.vertex_buffer.offset;
				geomDesc.Triangles.VertexBuffer.StrideInBytes = triangle.vertex_buffer.stride;
				geomDesc.Triangles.VertexFormat = to_native(triangle.vertex_format);
				geomDesc.Triangles.VertexCount = triangle.vertex_count;
				geomDesc.Triangles.IndexBuffer = to_native_gpu(triangle.index_buffer.buffer) + triangle.index_buffer.offset;
				geomDesc.Triangles.IndexCount = triangle.index_count;
				geomDesc.Triangles.IndexFormat = to_native(triangle.index_format);
				geomDesc.Triangles.Transform3x4 = to_native_gpu(triangle.transform3x4_buffer.buffer) + triangle.transform3x4_buffer.offset;
			}

			geomDesc.Type = to_native(desc.Type);
			geomDesc.Flags = to_native(desc.Flags);

			geom_desc_storage[i] = geomDesc;
		}

		inputs.pGeometryDescs = geom_desc_storage.data();
	}

	return inputs;
}

auto reshade::d3d12::convert_rt_build_desc(
	reshade::d3d12::device_impl *device,
	const api::rt_build_acceleration_structure_desc &desc,
	span<D3D12_RAYTRACING_GEOMETRY_DESC> geom_desc_storage) -> D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC
{
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = convert_rt_build_inputs(device, desc.Inputs, geom_desc_storage);

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
	asDesc.Inputs = inputs;
	asDesc.DestAccelerationStructureData = to_native_gpu(desc.DestData.buffer) + desc.DestData.offset;
	asDesc.ScratchAccelerationStructureData = to_native_gpu(desc.ScratchData.buffer) + desc.ScratchData.offset;

	return asDesc;
}

void reshade::d3d12::convert_rt_post_build_info_array(span<api::rt_acceleration_structure_postbuild_info_desc> input,
	span<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC> output)
{
	assert(output.size() == input.size());

	for (uint32_t i = 0; i < input.size(); i++)
	{
		output[i].DestBuffer = to_native_gpu(input[i].DestBuffer.buffer) + input[i].DestBuffer.offset;
		output[i].InfoType = to_native(input[i].InfoType);
	}
}
