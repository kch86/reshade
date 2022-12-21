// dllmain.cpp : Defines the entry point for the DLL application
#include <reshade.hpp>
#include "state_tracking.hpp"

// dx12
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <iosfwd>
#include <d3d12.h>
#include <d3d9on12.h>
#include "d3dx12.h"

//ray tracing includes
#include "Shaders\RaytracingHlslCompat.h"
#include "CompiledShaders\Raytracing.hlsl.h"


using namespace reshade::api;
using namespace Microsoft::WRL;


void compilePso(reshade::api::device *device);

class HrException : public std::runtime_error
{
	inline std::string HrToString(HRESULT hr)
	{
		char s_str[64] = {};
		sprintf_s(s_str, "HRESULT of 0x%08X", static_cast<UINT>(hr));
		return std::string(s_str);
	}
public:
	HrException(HRESULT hr) : std::runtime_error(HrToString(hr)), m_hr(hr) {}
	HRESULT Error() const { return m_hr; }
private:
	const HRESULT m_hr;
};

inline void ThrowIfFailed(HRESULT hr, const wchar_t *msg)
{
	if (FAILED(hr))
	{
		OutputDebugString(msg);
		throw HrException(hr);
	}
}

struct __declspec(uuid("7251932A-ADAF-4DFC-B5CB-9A4E8CD5D6EB")) device_data
{
	effect_runtime *main_runtime = nullptr;
	uint32_t offset_from_last_pass = 0;
	uint32_t last_render_pass_count = 1;
	uint32_t current_render_pass_count = 0;
	bool hasRenderedThisFrame = false;
};
struct __declspec(uuid("036CD16B-E823-4D6C-A137-5C335D6FD3E6")) command_list_data
{
	bool has_multiple_rtvs = false;
	resource_view current_main_rtv = { 0 };
	resource_view current_dsv = { 0 };
	uint32_t current_render_pass_index = 0;
};

static void on_init_device(device *device)
{
	device->create_private_data<device_data>();

	compilePso(device);
}

static void on_destroy_device(device *device)
{
	device->destroy_private_data<device_data>();
}

static void on_init_command_list(command_list *cmd_list)
{
	cmd_list->create_private_data<command_list_data>();
}
static void on_destroy_command_list(command_list *cmd_list)
{
	cmd_list->destroy_private_data<command_list_data>();
}

static void on_init_effect_runtime(effect_runtime *runtime)
{
	auto &dev_data = runtime->get_device()->get_private_data<device_data>();
	// Assume last created effect runtime is the main one
	dev_data.main_runtime = runtime;
}
static void on_destroy_effect_runtime(effect_runtime *runtime)
{
	auto &dev_data = runtime->get_device()->get_private_data<device_data>();
	if (runtime == dev_data.main_runtime)
		dev_data.main_runtime = nullptr;
}

static void on_bind_render_targets_and_depth_stencil(command_list *cmd_list, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
	auto &data = cmd_list->get_private_data<command_list_data>();

	const resource_view new_main_rtv = (count != 0) ? rtvs[0] : resource_view{ 0 };
	/*if (new_main_rtv != data.current_main_rtv)
		on_end_render_pass(cmd_list);*/

	data.has_multiple_rtvs = count > 1;
	data.current_main_rtv = new_main_rtv;
	data.current_dsv = dsv;
}

bool g_drawBeforeUi = false;
static bool on_draw(command_list* cmd_list, uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance)
{
	auto &data = cmd_list->get_private_data<command_list_data>();

	//if (data.has_multiple_rtvs || data.current_main_rtv == 0)
		//return; // Ignore when game is rendering to multiple render targets simultaneously

	device *const device = cmd_list->get_device();
	auto &dev_data = device->get_private_data<device_data>();

	uint32_t width, height;
	dev_data.main_runtime->get_screenshot_width_and_height(&width, &height);

	const resource_desc render_target_desc = device->get_resource_desc(device->get_resource_from_view(data.current_main_rtv));

	if (render_target_desc.texture.width != width || render_target_desc.texture.height != height)
		return false; // Ignore render targets that do not match the effect runtime back buffer dimensions

	// Render post-processing effects when a specific render pass is found (instead of at the end of the frame)
	// This is not perfect, since there may be multiple command lists and this will try and render effects in every single one ...
	//if (data.current_render_pass_index++ == (dev_data.last_render_pass_count - dev_data.offset_from_last_pass))
	if(g_drawBeforeUi && !dev_data.hasRenderedThisFrame && data.current_dsv == 0)
	{
		// TODO: find last valid 3d render target and apply that before drawing
		const auto &current_state = cmd_list->get_private_data<state_block>();

		dev_data.main_runtime->render_effects(cmd_list, data.current_main_rtv);

		// Re-apply state to the command-list, as it may have been modified by the call to 'render_effects'
		//current_state.apply(cmd_list);
		dev_data.hasRenderedThisFrame = true;
	}

	return false;
}

static void on_present(effect_runtime *runtime)
{
	device *const device = runtime->get_device();
	auto &dev_data = device->get_private_data<device_data>();

	dev_data.hasRenderedThisFrame = false;
}

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

bool g_createPso = true;
void compilePso(reshade::api::device *device)
{
	//if(g_createPso)
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

	//Direct3DDevice9* d3d9Device = (Direct3DDevice9*)device;

	if (device->get_api() == device_api::d3d12)
	{
		//ComPtr<ID3D12Device> d3d12_device;
		//IDirect3DDevice9On12 *d3d9On12Device = reinterpret_cast<IDirect3DDevice9On12 *>(device->get_native());
		//d3d9On12Device->GetD3D12Device(IID_PPV_ARGS(&d3d12_device));

		ID3D12Device*d3d12_device = reinterpret_cast<ID3D12Device*>(device->get_native());

		ComPtr<ID3D12Device5> dxrDevice;

		ThrowIfFailed(d3d12_device->QueryInterface(IID_PPV_ARGS(&dxrDevice)), L"Couldn't get DirectX Raytracing interface for the device.\n");
		//d3d12_device->QueryInterface(IID_PPV_ARGS(&dxrDevice));

		ComPtr<ID3D12StateObject> m_dxrStateObject;
		ThrowIfFailed(dxrDevice->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_dxrStateObject)), L"Couldn't create DirectX Raytracing state object.\n");
		//dxrDevice->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_dxrStateObject));
	}
	else if (false && device->get_api() == device_api::d3d9)
	{
		ComPtr<ID3D12Device> d3d12_device;
		IDirect3DDevice9On12 *d3d9On12Device = reinterpret_cast<IDirect3DDevice9On12 *>(device->get_native());
		d3d9On12Device->GetD3D12Device(IID_PPV_ARGS(&d3d12_device));

		ComPtr<ID3D12Device5> dxrDevice;

		ThrowIfFailed(d3d12_device->QueryInterface(IID_PPV_ARGS(&dxrDevice)), L"Couldn't get DirectX Raytracing interface for the device.\n");
		//d3d12_device->QueryInterface(IID_PPV_ARGS(&dxrDevice));


		ComPtr<ID3D12StateObject> m_dxrStateObject;
		ThrowIfFailed(dxrDevice->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_dxrStateObject)), L"Couldn't create DirectX Raytracing state object.\n");
		//dxrDevice->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_dxrStateObject));
	}
	
}

extern "C" __declspec(dllexport) const char *NAME = "Rt Addon";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Provide ray tracing functionality.";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;
		reshade::register_event<reshade::addon_event::init_device>(on_init_device);
		reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
		reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
		reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
		reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets_and_depth_stencil);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		register_state_tracking();
		break;
    case DLL_PROCESS_DETACH:
		unregister_state_tracking();

		reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}

