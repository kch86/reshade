#include "dxhelpers.h"

#include <reshade.hpp>

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

inline void allocateUAVBuffer(ID3D12Device *pDevice, UINT64 bufferSize, ID3D12Resource **ppResource, const wchar_t *resourceName = nullptr)
{
	auto uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	ThrowIfFailed(pDevice->CreateCommittedResource(
		&uploadHeapProperties,
		D3D12_HEAP_FLAG_NONE,
		&bufferDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(ppResource)));
	if (resourceName)
	{
		(*ppResource)->SetName(resourceName);
	}
}

AsBuffers buildBvh(command_list *cmdlist, const BvhBuildDesc &desc)
{
	device *reshade_device = cmdlist->get_device();

	DxrDevicedata &data = reshade_device->create_private_data<DxrDevicedata>();

	ComPtr<ID3D12Device5> dxrDevice = data.dxrDevice;

	return AsBuffers();
}
