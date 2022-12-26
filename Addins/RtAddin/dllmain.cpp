// dllmain.cpp : Defines the entry point for the DLL application
#include <reshade.hpp>
#include "state_tracking.hpp"

// std
#include <assert.h>
#include <shared_mutex>
#include <unordered_set>
#include <vector>

// dx12
#include <d3d12.h>
#include <d3d9on12.h>
#include "dxhelpers.h"

//ray tracing includes
#include "raytracing.h"

using namespace reshade::api;

namespace
{
	struct PosStreamInfo
	{
		uint32_t streamIndex;
		uint32_t stride;
		uint32_t offset;
		format format;
	};

	struct IndexData
	{
		resource ib = {};
		uint32_t offset = 0;
		uint32_t stride = 0;
		format fmt = format::unknown;
	};

	struct VertexData
	{
		resource vb = {};
		uint32_t offset = 0;
		uint32_t count = 0;
		uint32_t stride = 0;
		format fmt = format::unknown;
	};

	std::shared_mutex s_mutex;
	std::unordered_map<uint64_t, resource_desc> s_resources;
	std::unordered_map<uint64_t, uint64_t> s_shadow_resources;
	std::unordered_map<uint64_t, void *> s_mapped_resources;
	std::unordered_map<uint64_t, PosStreamInfo> s_inputLayoutPipelines;
	std::unordered_map<uint64_t, bool> s_needsBvhBuild;
	std::vector<scopedresource> s_bvhs;

	pipeline s_currentInputLayout;
	IndexData s_currentIB;
	VertexData s_currentVB;

	device* s_d3d12device = nullptr;
	command_list *s_d3d12cmdlist = nullptr;
	command_queue *s_d3d12cmdqueue = nullptr;
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

	if (device->get_api() == device_api::d3d9)
	{
		ID3D12CommandQueue* d3d12queue = (ID3D12CommandQueue * )s_d3d12cmdqueue->get_native();
		//resource->SetPrivateData(__uuidof(device_proxy), &device_proxy, sizeof(device_proxy), 0);
		device->set_private_data(reinterpret_cast<const uint8_t *>(&__uuidof(d3d12queue)), (uint64_t)d3d12queue);
	}
	else if (device->get_api() == device_api::d3d12)
	{
		s_d3d12device = device;
	}

	createDxrDevice(device);
	testCompilePso(device);
}
static void on_destroy_device(device *device)
{
	device->destroy_private_data<device_data>();
}

static void on_init_command_list(command_list *cmd_list)
{
	cmd_list->create_private_data<command_list_data>();

	if (cmd_list->get_device()->get_api() == device_api::d3d12)
	{
		s_d3d12cmdlist = cmd_list;
	}
}
static void on_destroy_command_list(command_list *cmd_list)
{
	cmd_list->destroy_private_data<command_list_data>();
}

static void on_init_command_queue(command_queue *cmd_queue)
{
if (cmd_queue->get_device()->get_api() == device_api::d3d12)
{
	if ((cmd_queue->get_type() & command_queue_type::graphics) != 0)
		s_d3d12cmdqueue = cmd_queue;
}
}
static void on_destroy_command_queue(command_queue *cmd_list)
{

}

static void on_init_pipeline(device *device, pipeline_layout, uint32_t subObjectCount, const pipeline_subobject *subObjects, pipeline handle)
{
#if 1
	for (uint32_t i = 0; i < subObjectCount; i++)
	{
		const pipeline_subobject &object = subObjects[i];
		if (object.type == pipeline_subobject_type::input_layout)
		{
			PosStreamInfo info;
			info.stride = 0;
			info.format = format::unknown;
			uint32_t posElemIndex = 0;

			for (uint32_t elemIdx = 0; elemIdx < object.count; elemIdx++)
			{
				const input_element &elem = reinterpret_cast<input_element *>(object.data)[elemIdx];

				if (strstr(elem.semantic, "POSITION") != nullptr)
				{
					assert(info.format == format::unknown);
					info.format = elem.format;
					info.offset = elem.offset;
					info.streamIndex = elem.buffer_binding;
					posElemIndex = elemIdx;
					break;
				}
			}

			if (info.format != format::unknown)
			{
				// found our position stream, calculate the stride and offset
				for (uint32_t elemIdx = 0; elemIdx < object.count; elemIdx++)
				{
					const input_element &elem = reinterpret_cast<input_element *>(object.data)[elemIdx];

					if (elem.buffer_binding == info.streamIndex)
					{
						if (elemIdx < posElemIndex)
						{
							info.offset += format_size(elem.format);
						}
						info.stride += format_size(elem.format);
					}
				}

				const std::unique_lock<std::shared_mutex> lock(s_mutex);
				// this is hitting.... is memory getting re-used inside the target app?
				/*if (s_inputLayoutPipelines.find(handle.handle) != s_inputLayoutPipelines.end())
				{
					assert(false);
				}*/
				s_inputLayoutPipelines[handle.handle] = info;
				return;
			}
		}
	}
#endif
}
static void on_destroy_pipeline(device *device, pipeline handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	assert(s_inputLayoutPipelines.find(handle.handle) != s_inputLayoutPipelines.end());
	s_inputLayoutPipelines.erase(handle.handle);
}
static void on_bind_pipeline(command_list *, pipeline_stage type, pipeline pipeline)
{
	if (type == pipeline_stage::input_assembler)
	{
		s_currentInputLayout = pipeline;
	}
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

static void on_init_resource(device *device, const resource_desc &desc, const subresource_data *, resource_usage usage, resource handle)
{
	if (!(desc.usage == resource_usage::vertex_buffer || desc.usage == resource_usage::index_buffer))
	{
		return;
	}
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	assert(s_resources.find(handle.handle) == s_resources.end());
	s_resources[handle.handle] = desc;

	if(desc.usage == resource_usage::vertex_buffer)
		s_needsBvhBuild[handle.handle] = true;
}
static void on_destroy_resource(device *device, resource handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	//assert(s_resources.find(handle.handle) != s_resources.end());
	s_resources.erase(handle.handle);
}

void on_map_buffer_region(device *device, resource resource, uint64_t offset, uint64_t size, map_access access, void **data)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	if (s_resources.find(resource.handle) != s_resources.end())
	{
		s_mapped_resources[resource.handle] = *data;
	}	
}

void on_unmap_buffer_region(device *device, resource handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	//create shadow copy
	if (s_mapped_resources.find(handle.handle) != s_mapped_resources.end())
	{
		void *data = s_mapped_resources[handle.handle];

		const resource_desc& desc = s_resources[handle.handle];

		subresource_data srd = {
			.data = data,
			.row_pitch = (uint32_t)desc.buffer.size,
		};

		resource d3d12res;
		s_d3d12device->create_resource(
			resource_desc(desc.buffer.size, memory_heap::cpu_to_gpu, desc.usage),
			nullptr, resource_usage::cpu_access, &d3d12res);

		void *ptr;
		s_d3d12device->map_buffer_region(d3d12res, 0, desc.buffer.size, map_access::write_only, &ptr);
		memcpy(ptr, data, (size_t)desc.buffer.size);
		s_d3d12device->unmap_buffer_region(d3d12res);

		s_shadow_resources[handle.handle] = d3d12res.handle;
		s_mapped_resources.erase(handle.handle);
	}
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
static void on_bind_index_buffer(command_list *, resource buffer, uint64_t offset, uint32_t index_size)
{
	s_currentIB.ib = buffer;
	s_currentIB.offset = (uint32_t)offset;
	s_currentIB.stride = index_size;
	s_currentIB.fmt = index_size == 2 ? format::r16_uint : format::r32_uint;
}
static void on_bind_vertex_buffers(command_list *, uint32_t first, uint32_t count, const resource *buffers, const uint64_t *offsets, const uint32_t *strides)
{
	const PosStreamInfo &posStreamInfo = s_inputLayoutPipelines[s_currentInputLayout.handle];

	//assert((int)count > posStreamInfo.streamIndex);
	if (first <= posStreamInfo.streamIndex && count > (posStreamInfo.streamIndex - first))
	{
		s_currentVB.vb = buffers[posStreamInfo.streamIndex];
		s_currentVB.offset = (uint32_t)offsets[posStreamInfo.streamIndex];
		s_currentVB.count = 0;
		s_currentVB.stride = strides[posStreamInfo.streamIndex];
		s_currentVB.fmt = posStreamInfo.format;
	}	
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
static bool on_draw_indexed(command_list * cmd_list, uint32_t index_count, uint32_t instances, uint32_t first_index, int32_t vertex_offset,
	/*uint32_t first_instance hack: interp instance offset as vertex count*/ uint32_t vertex_count)
{
	assert(s_shadow_resources.find(s_currentVB.vb.handle) != s_shadow_resources.end());
	assert(s_shadow_resources.find(s_currentIB.ib.handle) != s_shadow_resources.end());

	BvhBuildDesc desc = {
		.vb = {
			.res = s_shadow_resources[s_currentVB.vb.handle],
			.offset = s_currentVB.offset + (vertex_offset * s_currentVB.stride),
			.count = vertex_count,
			.stride = s_currentVB.stride,
			.fmt = s_currentVB.fmt
		},
		.ib = {
			.res = s_shadow_resources[s_currentIB.ib.handle],
			.offset = s_currentIB.offset + (first_index * s_currentIB.stride),
			.count = index_count,
			.fmt = s_currentIB.fmt
		}
	};

	const std::unique_lock<std::shared_mutex> lock(s_mutex);
	if (s_needsBvhBuild[s_currentVB.vb.handle])
	{
		scopedresource bvh = buildBvh(cmd_list->get_device(), s_d3d12cmdlist, s_d3d12cmdqueue, desc);

		s_bvhs.push_back(std::move(bvh));
		s_needsBvhBuild[s_currentVB.vb.handle] = false;
	}	

	// should I reset the vb/ib data now?
	// there could be multiple draws with the same vb/ib data
	return false;
}

static void on_present(effect_runtime *runtime)
{
	device *const device = runtime->get_device();
	auto &dev_data = device->get_private_data<device_data>();

	dev_data.hasRenderedThisFrame = false;
	s_d3d12cmdqueue->flush_immediate_command_list();

	doDeferredDeletes();
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
		reshade::register_event<reshade::addon_event::init_command_queue>(on_init_command_queue);
		reshade::register_event<reshade::addon_event::destroy_command_queue>(on_destroy_command_queue);
		reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
		reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
		reshade::register_event<reshade::addon_event::map_buffer_region>(on_map_buffer_region);
		reshade::register_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer_region);
		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::register_event<reshade::addon_event::bind_index_buffer>(on_bind_index_buffer);
		reshade::register_event<reshade::addon_event::bind_vertex_buffers>(on_bind_vertex_buffers);
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

