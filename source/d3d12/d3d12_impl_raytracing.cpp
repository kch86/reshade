#include "d3d12_impl_raytracing.hpp"
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

	D3D12_GPU_VIRTUAL_ADDRESS to_native_gpu(api::resource res)
	{
		ID3D12Resource *d3dres = reinterpret_cast<ID3D12Resource *>(res.handle);
		return d3dres->GetGPUVirtualAddress();
	}

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE to_native(api::rt_acceleration_structure_postbuild_info_type type)
	{
		return (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE)type;
	}
}

auto reshade::d3d12::convert_rt_build_inputs(const api::rt_build_acceleration_structure_inputs &input,
	span<D3D12_RAYTRACING_GEOMETRY_DESC> geom_desc_storage) -> D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS
{
	assert(geom_desc_storage.size() == input.NumDescs);
	for (uint32_t i = 0; i < input.NumDescs; i++)
	{
		const api::rt_geometry_desc &desc = input.pGeometryDescs[i];

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
			geomDesc.Triangles.VertexBuffer.StartAddress = to_native_gpu(triangle.VertexBuffer.buffer) + triangle.VertexBuffer.offset;
			geomDesc.Triangles.VertexBuffer.StrideInBytes = triangle.VertexBuffer.stride;
			geomDesc.Triangles.VertexFormat = to_native(triangle.VertexFormat);
			geomDesc.Triangles.VertexCount = triangle.VertexCount;
			geomDesc.Triangles.IndexBuffer = to_native_gpu(triangle.IndexBuffer.buffer) + triangle.IndexBuffer.offset;
			geomDesc.Triangles.IndexCount = triangle.IndexCount;
			geomDesc.Triangles.IndexFormat = to_native(triangle.IndexFormat);
			geomDesc.Triangles.Transform3x4 = to_native_gpu(triangle.Transform3x4.buffer) + triangle.Transform3x4.offset;
		}

		geomDesc.Type = to_native(desc.Type);
		geomDesc.Flags = to_native(desc.Flags);

		geom_desc_storage[i] = geomDesc;
	}

	// todo: support different layouts?
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.DescsLayout = to_native(input.DescsLayout);
	inputs.Flags = to_native(input.Flags);
	inputs.NumDescs = input.NumDescs;
	inputs.pGeometryDescs = geom_desc_storage.data();
	inputs.Type = to_native(input.Type);

	return inputs;
}

auto reshade::d3d12::convert_rt_build_desc(const api::rt_build_acceleration_structure_desc &desc, span<D3D12_RAYTRACING_GEOMETRY_DESC> geom_desc_storage) -> D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC
{
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = convert_rt_build_inputs(desc.Inputs, geom_desc_storage);

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
