#include "dxhelpers.h"

#include <shared_mutex>
#include <reshade.hpp>
#include <d3d9/d3d9_device.hpp>
#include <d3d9/d3d9on12_device.hpp>

#include "Shaders\RaytracingHlslCompat.h"
#include "CompiledShaders\Raytracing.hlsl.h"
#include "raytracing.h"

using namespace reshade::api;

struct DeferDeleteData
{
	std::vector<std::pair<device*, uint64_t>> todelete;
};

constexpr uint32_t MaxDeferredFrames = 4;
constexpr uint32_t HandleTypeCount = 2;
static DeferDeleteData s_frameDeleteData[MaxDeferredFrames][HandleTypeCount];
static uint32_t s_frameIndex = 0;
static std::shared_mutex s_mutex;

void doDeferredDeletes(uint32_t deleteIndex, uint32_t type_index)
{
	const int i = type_index;
	for (auto &pair : s_frameDeleteData[deleteIndex][i].todelete)
	{
		device *device = pair.first;
		if (i == 0)
		{
			device->destroy_resource_view((resource_view)pair.second);
		}
		else if (i == 1)
		{
			device->destroy_resource((resource)pair.second);
		}
		else
		{
			assert(false);
		}
	}
	s_frameDeleteData[deleteIndex][i].todelete.clear();
}

void doDeferredDeletes()
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	// delete oldest frame
	const uint32_t index = s_frameIndex % MaxDeferredFrames;
	const uint32_t deleteIndex = (index + 1) % MaxDeferredFrames;

	doDeferredDeletes(deleteIndex, 0);
	doDeferredDeletes(deleteIndex, 1);

	s_frameIndex++;
}

void doDeferredDeletesAll()
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	for(uint32_t index = 0; index < MaxDeferredFrames; index++)
		doDeferredDeletes(index, 0);

	for (uint32_t index = 0; index < MaxDeferredFrames; index++)
		doDeferredDeletes(index, 1);
}

void deferDestroyHandle(device* device, resource res)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	const uint32_t index = s_frameIndex % MaxDeferredFrames;

	auto iter = std::find_if(
		s_frameDeleteData[index][1].todelete.begin(),
		s_frameDeleteData[index][1].todelete.end(), [&](auto &handle) {
			return handle.second == res.handle;
	});

	if (iter == s_frameDeleteData[index][1].todelete.end())
	{
		s_frameDeleteData[index][1].todelete.push_back({ device,res.handle });
	}	
}

void deferDestroyHandle(device *device, resource_view view)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	const uint32_t index = s_frameIndex % MaxDeferredFrames;

	auto iter = std::find_if(
		s_frameDeleteData[index][0].todelete.begin(),
		s_frameDeleteData[index][0].todelete.end(), [&](auto &handle) {
			return handle.second == view.handle;
	});

	if (iter == s_frameDeleteData[index][0].todelete.end())
	{
		s_frameDeleteData[index][0].todelete.push_back({ device,view.handle });
	}
}

resource getd3d12resource(Direct3DDevice9On12 *device, command_queue* cmdqueue, resource res)
{
	IDirect3DResource9 *d3d9res = reinterpret_cast<IDirect3DResource9 *>(res.handle);
	ID3D12CommandQueue *d3d12queue = reinterpret_cast<ID3D12CommandQueue *>(cmdqueue->get_native());
	//ID3D12CommandQueue *d3d12queue =(D3D12CommandQueue*)cmdqueue;// reinterpret_cast<ID3D12CommandQueue *>(cmdqueue->get_native());

	ID3D12Resource *d3d12res = nullptr;
	//device->UnwrapUnderlyingResource(d3d9res, d3d12queue, IID_PPV_ARGS(&d3d12res));

	IDirect3DDevice9On12 *d3d9on12 = device->_orig;
	ThrowIfFailed(d3d9on12->UnwrapUnderlyingResource(d3d9res, d3d12queue, IID_PPV_ARGS(&d3d12res)));

	return { uint64_t(d3d12res) };
}

void returnd3d12resource(Direct3DDevice9On12 *device, uint64_t signal, ID3D12Fence *fence, resource res)
{
	IDirect3DResource9 *d3d9res = reinterpret_cast<IDirect3DResource9 *>(res.handle);

	IDirect3DDevice9On12 *d3d9on12 = device->_orig;

	uint32_t signal_count = 0;
	if (fence)
	{
		signal_count = 1;
	}
	ThrowIfFailed(d3d9on12->ReturnUnderlyingResource(d3d9res, signal_count, &signal, &fence));
}

struct AccelerationStructureBuffers
{
	ID3D12Resource* pScratch;
	ID3D12Resource* pResult;
	ID3D12Resource* pInstanceDesc;    // Used only for top-level AS
};

ID3D12Resource* createBuffer(ID3D12Device5* pDevice, uint64_t size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initState)
{
	static const D3D12_HEAP_PROPERTIES kDefaultHeapProps =
	{
		D3D12_HEAP_TYPE_DEFAULT,
		D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
		D3D12_MEMORY_POOL_UNKNOWN,
		0,
		0
	};

	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Alignment = 0;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Flags = flags;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.Height = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	bufDesc.MipLevels = 1;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.SampleDesc.Quality = 0;
	bufDesc.Width = size;

	ID3D12Resource *pBuffer;
	ThrowIfFailed(pDevice->CreateCommittedResource(&kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, initState, nullptr, IID_PPV_ARGS(&pBuffer)));
	return pBuffer;
}

inline scopedresource allocateUAVBuffer(device *d, uint64_t bufferSize, resource_usage usage, const wchar_t *resourceName = nullptr)
{
	resource_desc desc(bufferSize, memory_heap::gpu_only, usage);

	resource res;
	ThrowIfFailed(d->create_resource(desc, nullptr, usage, &res));

	return scopedresource(d, res);
}

AccelerationStructureBuffers build_bvh_native(ID3D12Device5* pDevice, ID3D12GraphicsCommandList4* pCmdList, const BlasBuildDesc &desc, ID3D12Resource* vb, ID3D12Resource* ib)
{
	D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
	geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geomDesc.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress() + desc.vb.offset;
	geomDesc.Triangles.VertexBuffer.StrideInBytes = desc.vb.stride;
	geomDesc.Triangles.VertexFormat = to_native_d3d12(desc.vb.fmt);
	geomDesc.Triangles.VertexCount = desc.vb.count;
	geomDesc.Triangles.IndexBuffer = ib->GetGPUVirtualAddress() + desc.ib.offset;
	geomDesc.Triangles.IndexCount = desc.ib.count;
	geomDesc.Triangles.IndexFormat = to_native_d3d12(desc.ib.fmt);
	geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

	// Get the size requirements for the scratch and AS buffers
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
	inputs.NumDescs = 1;
	inputs.pGeometryDescs = &geomDesc;
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
	pDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

	// Create the buffers. They need to support UAV, and since we are going to immediately use them, we create them with an unordered-access state
	AccelerationStructureBuffers buffers;
	buffers.pScratch = createBuffer(pDevice, info.ScratchDataSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	buffers.pResult = createBuffer(pDevice, info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

	// Create the bottom-level AS
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
	asDesc.Inputs = inputs;
	asDesc.DestAccelerationStructureData = buffers.pResult->GetGPUVirtualAddress();
	asDesc.ScratchAccelerationStructureData = buffers.pScratch->GetGPUVirtualAddress();

	pCmdList->BuildRaytracingAccelerationStructure(&asDesc, 0, nullptr);

	// We need to insert a UAV barrier before using the acceleration structures in a raytracing operation
	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = buffers.pResult;
	pCmdList->ResourceBarrier(1, &uavBarrier);

	return buffers;
}

scopedresource buildBlas(device* device9,
				   command_list *cmdlist,
				   command_queue *cmdqueue,
				   const BlasBuildDesc &desc)
{
	Direct3DDevice9On12 *d3d9on12 = ((Direct3DDevice9 *)device9)->_d3d9on12_device;// ->_orig;
	assert(d3d9on12);

	device *device12 = cmdlist->get_device();

#if 0
	//ID3D12Resource* vb = reinterpret_cast<ID3D12Resource*>(getd3d12resource(d3d9on12, cmdqueue, (resource)desc.vb.res).handle);
	//ID3D12Resource* ib = reinterpret_cast<ID3D12Resource*>(getd3d12resource(d3d9on12, cmdqueue, (resource)desc.ib.res).handle);
	ID3D12Resource *vb = reinterpret_cast<ID3D12Resource *>(desc.vb.res);
	ID3D12Resource *ib = reinterpret_cast<ID3D12Resource *>(desc.ib.res);

	ID3D12Device *nativeDevice = reinterpret_cast<ID3D12Device *>(device12->get_native());
	ID3D12GraphicsCommandList *nativeCmdlist = reinterpret_cast<ID3D12GraphicsCommandList *>(cmdlist->get_native());

	ID3D12Device5 *nativeDevice5 = nullptr;
	nativeDevice->QueryInterface(IID_PPV_ARGS(&nativeDevice5));

	ID3D12GraphicsCommandList4 *nativeCmdlist4 = nullptr;
	nativeCmdlist->QueryInterface(IID_PPV_ARGS(&nativeCmdlist4));

	ID3D12Resource* nativebvh = build_bvh_native(nativeDevice5, nativeCmdlist4, desc, vb, ib).pResult;

	cmdqueue->flush_immediate_command_list();

	scopedresource bvh(device12, (resource)uint64_t(nativebvh));
#else
	rt_geometry_desc geomDesc = {};
	geomDesc.type = rt_geometry_type::triangles;
	geomDesc.triangle_geo_descs.vertex_buffer = {
		.buffer = desc.vb.res,
		.offset = desc.vb.offset,
		.stride = desc.vb.stride,
	};
	geomDesc.triangle_geo_descs.vertex_format = desc.vb.fmt;
	geomDesc.triangle_geo_descs.vertex_count = desc.vb.count;
	geomDesc.triangle_geo_descs.index_buffer = {
		.buffer = desc.ib.res,
		.offset = desc.ib.offset,
	};
	geomDesc.triangle_geo_descs.index_count = desc.ib.count;
	geomDesc.triangle_geo_descs.index_format = desc.ib.fmt;
	geomDesc.flags = desc.opaque ? rt_geometry_flags::opaque : rt_geometry_flags::none;

	// Get the size requirements for the scratch and AS buffers
	rt_build_acceleration_structure_inputs inputs = {};
	inputs.descs_layout = rt_elements_layout::array;
	inputs.flags = rt_acceleration_structure_build_flags::prefer_fast_trace;
	inputs.desc_count = 1;
	inputs.geometry_desc_array = &geomDesc;
	inputs.type = rt_acceleration_structure_type::bottom_level;

	rt_acceleration_structure_prebuild_info info = {};
	device12->get_rt_acceleration_structure_prebuild_info(&inputs, &info);

	// Create the buffers. They need to support UAV, and since we are going to immediately use them, we create them with an unordered-access state
	scopedresource scratch = allocateUAVBuffer(device12, info.scratch_data_size_in_bytes, resource_usage::unordered_access);
	scopedresource bvh = allocateUAVBuffer(device12, info.result_data_max_size_in_bytes, resource_usage::acceleration_structure);

	// Create the bottom-level AS
	rt_build_acceleration_structure_desc asDesc = {};
	asDesc.inputs = inputs;
	asDesc.dest_data = { .buffer = bvh.handle()};
	asDesc.scratch_data = { .buffer = scratch.handle()};

	cmdlist->build_acceleration_structure(&asDesc, 0, nullptr);

	// We need to insert a UAV barrier before using the acceleration structures in a raytracing operation
	cmdlist->barrier(bvh.handle(), resource_usage::unordered_access, resource_usage::unordered_access);

#endif

	return bvh;
}

scopedresource buildTlas(reshade::api::command_list *cmdlist, reshade::api::command_queue *cmdqueue, const TlasBuildDesc &desc)
{
	device *device12 = cmdlist->get_device();

	rt_build_acceleration_structure_inputs inputs = {};
	inputs.descs_layout = rt_elements_layout::array;
	inputs.flags = rt_acceleration_structure_build_flags::prefer_fast_trace;
	inputs.desc_count = desc.instances.size();
	inputs.type = rt_acceleration_structure_type::top_level;

	rt_acceleration_structure_prebuild_info info = {};
	device12->get_rt_acceleration_structure_prebuild_info(&inputs, &info);

	// Create the buffers. They need to support UAV, and since we are going to immediately use them, we create them with an unordered-access state
	scopedresource scratch = allocateUAVBuffer(device12, info.scratch_data_size_in_bytes, resource_usage::unordered_access);
	scopedresource bvh = allocateUAVBuffer(device12, info.result_data_max_size_in_bytes, resource_usage::acceleration_structure);

	const uint32_t instancesSizeBytes = sizeof(rt_instance_desc) * desc.instances.size();
	resource instances;
	device12->create_resource(
		resource_desc(instancesSizeBytes, memory_heap::cpu_to_gpu, resource_usage::shader_resource),
		nullptr, resource_usage::cpu_access, &instances);

	// schedule buffer for deletion
	scopedresource instancelifetime(device12, instances);

	rt_build_acceleration_structure_desc asDesc = {};
	asDesc.inputs = inputs;
	asDesc.inputs.instances.instance_descs = desc.instances.data();
	asDesc.inputs.instances.instances_buffer = { .buffer = instances };
	asDesc.dest_data = { .buffer = bvh.handle()};
	asDesc.scratch_data = { .buffer = scratch.handle()};

	cmdlist->build_acceleration_structure(&asDesc, 0, nullptr);

	// We need to insert a UAV barrier before using the acceleration structures in a raytracing operation
	cmdlist->barrier(bvh.handle(), resource_usage::unordered_access, resource_usage::unordered_access);

	return bvh;
}

resource lock_resource(reshade::api::device *device9, reshade::api::command_queue *cmdqueue12, reshade::api::resource d3d9resource)
{
	Direct3DDevice9On12 *d3d9on12 = ((Direct3DDevice9 *)device9)->_d3d9on12_device;// ->_orig;
	return getd3d12resource(d3d9on12, cmdqueue12, d3d9resource);
}

void unlock_resource(reshade::api::device *device9, uint64_t signal, ID3D12Fence *fence, reshade::api::resource d3d9resource)
{
	Direct3DDevice9On12 *d3d9on12 = ((Direct3DDevice9 *)device9)->_d3d9on12_device;// ->_orig;
	returnd3d12resource(d3d9on12, signal, fence, d3d9resource);
}

