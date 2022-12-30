#include "dxhelpers.h"

#include <shared_mutex>
#include <reshade.hpp>
#include <d3d9/d3d9_device.hpp>
#include <d3d9/d3d9on12_device.hpp>

#include "Shaders\RaytracingHlslCompat.h"
#include "CompiledShaders\Raytracing.hlsl.h"
#include "raytracing.h"

using namespace reshade::api;

// Pretty-print a state object tree.
inline void PrintStateObjectDesc(const D3D12_STATE_OBJECT_DESC *desc)
{
	std::wstringstream wstr;
	wstr << L"\n";
	wstr << L"--------------------------------------------------------------------\n";
	wstr << L"| D3D12 State Object 0x" << static_cast<const void *>(desc) << L": ";
	if (desc->Type == D3D12_STATE_OBJECT_TYPE_COLLECTION) wstr << L"Collection\n";
	if (desc->Type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE) wstr << L"Raytracing Pipeline\n";

	auto ExportTree = [](UINT depth, UINT numExports, const D3D12_EXPORT_DESC *exports)
	{
		std::wostringstream woss;
		for (UINT i = 0; i < numExports; i++)
		{
			woss << L"|";
			if (depth > 0)
			{
				for (UINT j = 0; j < 2 * depth - 1; j++) woss << L" ";
			}
			woss << L" [" << i << L"]: ";
			if (exports[i].ExportToRename) woss << exports[i].ExportToRename << L" --> ";
			woss << exports[i].Name << L"\n";
		}
		return woss.str();
	};

	for (UINT i = 0; i < desc->NumSubobjects; i++)
	{
		wstr << L"| [" << i << L"]: ";
		switch (desc->pSubobjects[i].Type)
		{
		case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
			wstr << L"Global Root Signature 0x" << desc->pSubobjects[i].pDesc << L"\n";
			break;
		case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
			wstr << L"Local Root Signature 0x" << desc->pSubobjects[i].pDesc << L"\n";
			break;
		case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
			wstr << L"Node Mask: 0x" << std::hex << std::setfill(L'0') << std::setw(8) << *static_cast<const UINT *>(desc->pSubobjects[i].pDesc) << std::setw(0) << std::dec << L"\n";
			break;
		case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY:
		{
			wstr << L"DXIL Library 0x";
			auto lib = static_cast<const D3D12_DXIL_LIBRARY_DESC *>(desc->pSubobjects[i].pDesc);
			wstr << lib->DXILLibrary.pShaderBytecode << L", " << lib->DXILLibrary.BytecodeLength << L" bytes\n";
			wstr << ExportTree(1, lib->NumExports, lib->pExports);
			break;
		}
		case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION:
		{
			wstr << L"Existing Library 0x";
			auto collection = static_cast<const D3D12_EXISTING_COLLECTION_DESC *>(desc->pSubobjects[i].pDesc);
			wstr << collection->pExistingCollection << L"\n";
			wstr << ExportTree(1, collection->NumExports, collection->pExports);
			break;
		}
		case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
		{
			wstr << L"Subobject to Exports Association (Subobject [";
			auto association = static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *>(desc->pSubobjects[i].pDesc);
			UINT index = static_cast<UINT>(association->pSubobjectToAssociate - desc->pSubobjects);
			wstr << index << L"])\n";
			for (UINT j = 0; j < association->NumExports; j++)
			{
				wstr << L"|  [" << j << L"]: " << association->pExports[j] << L"\n";
			}
			break;
		}
		case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
		{
			wstr << L"DXIL Subobjects to Exports Association (";
			auto association = static_cast<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION *>(desc->pSubobjects[i].pDesc);
			wstr << association->SubobjectToAssociate << L")\n";
			for (UINT j = 0; j < association->NumExports; j++)
			{
				wstr << L"|  [" << j << L"]: " << association->pExports[j] << L"\n";
			}
			break;
		}
		case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
		{
			wstr << L"Raytracing Shader Config\n";
			auto config = static_cast<const D3D12_RAYTRACING_SHADER_CONFIG *>(desc->pSubobjects[i].pDesc);
			wstr << L"|  [0]: Max Payload Size: " << config->MaxPayloadSizeInBytes << L" bytes\n";
			wstr << L"|  [1]: Max Attribute Size: " << config->MaxAttributeSizeInBytes << L" bytes\n";
			break;
		}
		case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
		{
			wstr << L"Raytracing Pipeline Config\n";
			auto config = static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG *>(desc->pSubobjects[i].pDesc);
			wstr << L"|  [0]: Max Recursion Depth: " << config->MaxTraceRecursionDepth << L"\n";
			break;
		}
		case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP:
		{
			wstr << L"Hit Group (";
			auto hitGroup = static_cast<const D3D12_HIT_GROUP_DESC *>(desc->pSubobjects[i].pDesc);
			wstr << (hitGroup->HitGroupExport ? hitGroup->HitGroupExport : L"[none]") << L")\n";
			wstr << L"|  [0]: Any Hit Import: " << (hitGroup->AnyHitShaderImport ? hitGroup->AnyHitShaderImport : L"[none]") << L"\n";
			wstr << L"|  [1]: Closest Hit Import: " << (hitGroup->ClosestHitShaderImport ? hitGroup->ClosestHitShaderImport : L"[none]") << L"\n";
			wstr << L"|  [2]: Intersection Import: " << (hitGroup->IntersectionShaderImport ? hitGroup->IntersectionShaderImport : L"[none]") << L"\n";
			break;
		}
		}
		wstr << L"|--------------------------------------------------------------------\n";
	}
	wstr << L"\n";
	OutputDebugStringW(wstr.str().c_str());
}

struct __declspec(uuid("A655CE6C-2404-4C64-80F9-A48BF3F1DE6C")) DxrDevicedata
{
	ComPtr<ID3D12Device5> dxrDevice;
};
bool g_createPso = true;
void createDxrDevice(device *device)
{
	if (device->get_api() != device_api::d3d12)
		return;

	ID3D12Device *d3d12_device = reinterpret_cast<ID3D12Device *>(device->get_native());

	ComPtr<ID3D12Device5> dxrDevice;

	ThrowIfFailed(d3d12_device->QueryInterface(IID_PPV_ARGS(&dxrDevice)), L"Couldn't get DirectX Raytracing interface for the device.\n");

	DxrDevicedata& data = device->create_private_data<DxrDevicedata>();
	data.dxrDevice = dxrDevice;
}

void testCompilePso(device *device)
{
	if (!g_createPso)
	{
		return;
	}

	const wchar_t *c_hitGroupName = L"MyHitGroup";
	const wchar_t *c_raygenShaderName = L"MyRaygenShader";
	const wchar_t *c_closestHitShaderName = L"MyClosestHitShader";
	const wchar_t *c_missShaderName = L"MyMissShader";

	// Library subobject names
	const wchar_t *c_globalRootSignatureName = L"MyGlobalRootSignature";
	const wchar_t *c_localRootSignatureName = L"MyLocalRootSignature";
	const wchar_t *c_localRootSignatureAssociationName = L"MyLocalRootSignatureAssociation";
	const wchar_t *c_shaderConfigName = L"MyShaderConfig";
	const wchar_t *c_pipelineConfigName = L"MyPipelineConfig";

	// Create library and use the library subobject defined in the library in the RTPSO:
	// Subobjects need to be associated with DXIL shaders exports either by way of default or explicit associations.
	// Default association applies to every exported shader entrypoint that doesn't have any of the same type of subobject associated with it.
	// This simple sample utilizes default shader association except for local root signature subobject
	// which has an explicit association specified purely for demonstration purposes.
	//
	// Following subobjects are defined with the library itself. We can export and rename the subobjects from the 
	// DXIL library in a similar way we export shaders. 
	// 1 - Triangle hit group
	// 1 - Shader config
	// 2 - Local root signature and association
	// 1 - Global root signature
	// 1 - Pipeline config
	// 1 - Subobject to export association
	CD3DX12_STATE_OBJECT_DESC raytracingPipeline{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE };


	// DXIL library
	// This contains the shaders and their entrypoints for the state object.
	// Since shaders are not considered a subobject, they need to be passed in via DXIL library subobjects.
	auto lib = raytracingPipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
	D3D12_SHADER_BYTECODE libdxil = CD3DX12_SHADER_BYTECODE((void *)g_pRaytracing, ARRAYSIZE(g_pRaytracing));
	lib->SetDXILLibrary(&libdxil);
	// Define which shader exports to surface from the library.
	// If no shader exports are defined for a DXIL library subobject, all shaders will be surfaced.
	// In this sample, this could be ommited for convenience since the sample uses all shaders in the library. 
	{
		lib->DefineExport(c_raygenShaderName);
		lib->DefineExport(c_closestHitShaderName);
		lib->DefineExport(c_missShaderName);
	}

	// Define which subobjects exports to use from the library.
	// If no exports are defined all subobjects are used. 
	{
		lib->DefineExport(c_globalRootSignatureName);
		lib->DefineExport(c_localRootSignatureName);
		lib->DefineExport(c_localRootSignatureAssociationName);
		lib->DefineExport(c_shaderConfigName);
		lib->DefineExport(c_pipelineConfigName);
		lib->DefineExport(c_hitGroupName);
	}

	PrintStateObjectDesc(raytracingPipeline);

	if (device->get_api() == device_api::d3d12)
	{
		DxrDevicedata &data = device->get_private_data<DxrDevicedata>();

		ComPtr<ID3D12Device5> dxrDevice = data.dxrDevice;

		ComPtr<ID3D12StateObject> m_dxrStateObject;
		ThrowIfFailed(dxrDevice->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_dxrStateObject)), L"Couldn't create DirectX Raytracing state object.\n");
	}

	g_createPso = false;
}

struct DeferDeleteData
{
	std::vector<std::pair<device*, resource>> todelete;
};

constexpr uint32_t MaxDeferredFrames = 4;
static DeferDeleteData s_frameDeleteData[MaxDeferredFrames];
static uint32_t s_frameIndex = 0;
static std::shared_mutex s_mutex;

void doDeferredDeletes()
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	// delete oldest frame
	const uint32_t index = s_frameIndex % MaxDeferredFrames;
	const uint32_t deleteIndex = (index + 1) % MaxDeferredFrames;

	for (auto &pair : s_frameDeleteData[deleteIndex].todelete)
	{
		device *device = pair.first;
		device->destroy_resource(pair.second);
	}
	s_frameDeleteData[deleteIndex].todelete.clear();

	s_frameIndex++;
}

void deferDestroyResource(device* device, resource res)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	const uint32_t index = s_frameIndex % MaxDeferredFrames;

	s_frameDeleteData[index].todelete.push_back({ device,res });
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

	// TODO: pass valid values for the signal/fences
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
	geomDesc.Type = rt_geometry_type::triangles;
	geomDesc.Triangles.vertex_buffer = {
		.buffer = desc.vb.res,
		.offset = desc.vb.offset,
		.stride = desc.vb.stride,
	};
	geomDesc.Triangles.vertex_format = desc.vb.fmt;
	geomDesc.Triangles.vertex_count = desc.vb.count;
	geomDesc.Triangles.index_buffer = {
		.buffer = desc.ib.res,
		.offset = desc.ib.offset,
	};
	geomDesc.Triangles.index_count = desc.ib.count;
	geomDesc.Triangles.index_format = desc.ib.fmt;
	geomDesc.Flags = rt_geometry_flags::opaque;

	// Get the size requirements for the scratch and AS buffers
	rt_build_acceleration_structure_inputs inputs = {};
	inputs.DescsLayout = rt_elements_layout::array;
	inputs.Flags = rt_acceleration_structure_build_flags::prefer_fast_trace;
	inputs.NumDescs = 1;
	inputs.pGeometryDescs = &geomDesc;
	inputs.Type = rt_acceleration_structure_type::bottom_level;

	rt_acceleration_structure_prebuild_info info = {};
	device12->get_rt_acceleration_structure_prebuild_info(&inputs, &info);

	// Create the buffers. They need to support UAV, and since we are going to immediately use them, we create them with an unordered-access state
	scopedresource scratch = allocateUAVBuffer(device12, info.ScratchDataSizeInBytes, resource_usage::unordered_access);
	scopedresource bvh = allocateUAVBuffer(device12, info.ResultDataMaxSizeInBytes, resource_usage::acceleration_structure);

	// Create the bottom-level AS
	rt_build_acceleration_structure_desc asDesc = {};
	asDesc.Inputs = inputs;
	asDesc.DestData = { .buffer = bvh };
	asDesc.ScratchData = { .buffer = scratch };

	cmdlist->build_acceleration_structure(&asDesc, 0, nullptr);

	// We need to insert a UAV barrier before using the acceleration structures in a raytracing operation
	cmdlist->barrier(bvh, resource_usage::unordered_access, resource_usage::unordered_access);

#endif

	return bvh;
}

scopedresource buildTlas(reshade::api::command_list *cmdlist, reshade::api::command_queue *cmdqueue, const TlasBuildDesc &desc)
{
	device *device12 = cmdlist->get_device();

	rt_build_acceleration_structure_inputs inputs = {};
	inputs.DescsLayout = rt_elements_layout::array;
	inputs.Flags = rt_acceleration_structure_build_flags::prefer_fast_trace;
	inputs.NumDescs = desc.instances.size();
	inputs.Type = rt_acceleration_structure_type::top_level;

	rt_acceleration_structure_prebuild_info info = {};
	device12->get_rt_acceleration_structure_prebuild_info(&inputs, &info);

	// Create the buffers. They need to support UAV, and since we are going to immediately use them, we create them with an unordered-access state
	scopedresource scratch = allocateUAVBuffer(device12, info.ScratchDataSizeInBytes, resource_usage::unordered_access);
	scopedresource bvh = allocateUAVBuffer(device12, info.ResultDataMaxSizeInBytes, resource_usage::acceleration_structure);

	const uint32_t instancesSizeBytes = sizeof(rt_instance_desc) * desc.instances.size();
	resource instances;
	device12->create_resource(
		resource_desc(instancesSizeBytes, memory_heap::cpu_to_gpu, resource_usage::shader_resource),
		nullptr, resource_usage::cpu_access, &instances);

	// schedule buffer for deletion
	scopedresource instancelifetime(device12, instances);

	rt_build_acceleration_structure_desc asDesc = {};
	asDesc.Inputs = inputs;
	asDesc.Inputs.instances.instance_descs = desc.instances.data();
	asDesc.Inputs.instances.instances_buffer = { .buffer = instances };
	asDesc.DestData = { .buffer = bvh };
	asDesc.ScratchData = { .buffer = scratch };

	cmdlist->build_acceleration_structure(&asDesc, 0, nullptr);

	// We need to insert a UAV barrier before using the acceleration structures in a raytracing operation
	cmdlist->barrier(bvh, resource_usage::unordered_access, resource_usage::unordered_access);

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

void scopedresource::free()
{
	if (handle)
	{
		deferDestroyResource(_device, *this);
	}
}
