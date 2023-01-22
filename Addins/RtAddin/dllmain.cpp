// dllmain.cpp : Defines the entry point for the DLL application
#include <imgui/imgui.h>
#include <reshade.hpp>
#include "state_tracking.hpp"
#include <scope_guard/scope_guard.h>

// std
#include <assert.h>
#include <shared_mutex>
#include <unordered_set>
#include <vector>
#include <span>

// dx12
#include <d3d12.h>
#include <d3d9on12.h>
#include "dxhelpers.h"

//ray tracing includes
#include "raytracing.h"
#include "bvh_manager.h"
#include "CompiledShaders\Raytracing_inline.hlsl.h"
#include "CompiledShaders\Raytracing_blit_vs.hlsl.h"
#include "CompiledShaders\Raytracing_blit_ps.hlsl.h"
#include "hash.h"

using namespace reshade::api;
using namespace DirectX;

namespace
{
	struct MapRegion
	{
		struct Buffer {
			void *data;
		};

		struct Texture {
			uint32_t subresource;
			subresource_box box;
			subresource_data data;
		};

		Buffer buffer;
		Texture texture;
	};

	struct StreamInfo
	{
		struct stream
		{
			uint32_t index = 0;
			uint32_t offset = 0;
			uint32_t stride = 0;
			format format = format::unknown;
		};

		stream pos;
		stream uv;
	};

	struct IndexData
	{
		resource ib = {};
		uint32_t offset = 0;
		uint32_t stride = 0;
		uint64_t size_bytes = 0;
		format fmt = format::unknown;
	};

	struct StreamData
	{
		struct stream
		{
			resource res = {};
			uint32_t offset = 0;
			uint32_t elem_offset = 0;
			uint32_t count = 0;
			uint32_t stride = 0;
			uint64_t size_bytes = 0;
			format fmt = format::unknown;
		};

		stream pos;
		stream uv;
	};

	struct BoundTextureData
	{
		resource slots[4];
	};

	struct CameraCb
	{
		XMMATRIX view;

		XMVECTOR pos;

		float fov;
		uint32_t usePrebuiltCamMat = 1;
		uint32_t showNormal = 0;
		uint32_t showUvs = 0;

		uint32_t showTexture = 0;
		uint32_t pad[3];
	};

	std::shared_mutex s_mutex;
	std::unordered_set<uint64_t> s_backbuffers;
	std::unordered_map<uint64_t, resource_desc> s_resources;
	std::unordered_map<uint64_t, scopedresource> s_shadow_resources;
	std::unordered_map<uint64_t, MapRegion> s_mapped_resources;
	std::unordered_set<uint64_t> s_dynamic_resources;
	std::unordered_map<uint64_t, StreamInfo> s_inputLayoutPipelines;
	scopedresource s_tlas;
	scopedresource s_attachments_buffer;
	scopedresourceview s_attachments_srv;
	pipeline_layout s_pipeline_layout;
	pipeline s_pipeline;

	pipeline_layout s_blit_layout;
	pipeline s_blit_pipeline;

	scopedresource s_output;
	scopedresourceview s_output_uav, s_output_srv;
	uint32_t s_width = 0, s_height = 0;

	pipeline s_currentInputLayout;
	IndexData s_currentIB;
	StreamData s_currentVB;
	resource_view s_current_rtv = { 0 };
	BoundTextureData s_currentTextureBindings;

	scopedresource s_empty_buffer;
	scopedresourceview s_empty_srv;

	device* s_d3d12device = nullptr;
	command_list *s_d3d12cmdlist = nullptr;
	command_queue *s_d3d12cmdqueue = nullptr;

	float s_ui_view_rot_x = 0.0;
	float s_ui_view_rot_y = 0.0;
	float s_ui_view_rot_z = 0.0;
	float s_ui_fov = 60.0;
	int s_ui_drawCallBegin = 0;
	int s_ui_drawCallEnd = 4095;
	bool s_ui_use_viewproj = true;
	bool s_ui_show_rt_full = false;
	bool s_ui_show_rt_half = false;
	bool s_ui_show_normals = false;
	bool s_ui_show_uvs = false;
	bool s_ui_show_texture = false;
	bool s_ui_enable = true;
	bool s_ui_pause = false;
	float s_cam_pitch = 0.0;
	float s_cam_yaw = 0.0;
	XMVECTOR s_cam_pos = XMVectorZero();

	uint32_t s_mouse_x = 0;
	uint32_t s_mouse_y = 0;
	bool s_ctrl_down = false;

	const uint64_t StaticGeoVsHash = 18047787432603860385;
	std::unordered_set<uint64_t> s_static_geo_vs_pipelines;
	std::unordered_map<uint64_t, uint32_t> s_vs_transform_map;
	std::unordered_map<uint64_t, uint64_t> s_vs_hash_map;
	bool s_staticgeo_vs_pipeline_is_bound = false;
	pipeline s_current_vs_pipeline;
	bvh_manager s_bvh_manager;

	XMMATRIX s_viewproj;
	XMMATRIX s_view;
	XMMATRIX s_current_wvp = XMMatrixIdentity();
	bool s_got_viewproj = false;

	int s_draw_count = 0;

	bool s_d3d_debug_enabled = false;

	std::vector<scopedresource> s_instance_id_buffers;
	std::vector<scopedresourceview> s_instance_id_srvs;
	scopedresource s_instances_buffer;
	scopedresourceview s_instances_buffer_srv;
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

static void init_vs_mappings()
{
	struct VsTransformMap
	{
		uint64_t hash;
		uint32_t offset;
	};

	static VsTransformMap hashes[] = {
		{6550593362979704143, 74},
		{5461187419696972836, 10},
	};

	for (const VsTransformMap& mapping : hashes)
	{
		s_vs_transform_map[mapping.hash] = mapping.offset;
	}
}

static void clear_bvhs()
{
	s_bvh_manager.destroy();
}

static bool filter_command()
{
	// add 1 for the filter because the draw call index is only incremented on the draw
	// so all the bound state is 1 behind
	const int drawId = s_draw_count;
	return !s_ui_enable || !(drawId >= (s_ui_drawCallBegin) && drawId <= s_ui_drawCallEnd);
}

static void on_init_swapchain(swapchain *swapchain)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	const device_api api = swapchain->get_device()->get_api();

	for (uint32_t i = 0; i < swapchain->get_back_buffer_count(); ++i)
	{
		const resource buffer = swapchain->get_back_buffer(i);

		s_backbuffers.emplace(buffer.handle);
	}
}

static void init_pipeline()
{
	// init rt pipeline
	{
		pipeline_layout_param params[] = {
			pipeline_layout_param(descriptor_range{.binding = 0, .dx_register_space = 0, .count = 1, .visibility = shader_stage::compute, .type = descriptor_type::acceleration_structure}),
			pipeline_layout_param(descriptor_range{.binding = 0, .dx_register_space = 1, .count = 2, .visibility = shader_stage::compute, .type = descriptor_type::shader_resource_view}),
			pipeline_layout_param(descriptor_range{.binding = 0, .count = 1, .visibility = shader_stage::compute, .type = descriptor_type::unordered_access_view}),
			pipeline_layout_param(constant_range{.binding = 0, .count = sizeof(CameraCb)/sizeof(int), .visibility = shader_stage::compute}),
		};

		shader_desc shader_desc = {
			.code = g_pRaytracing_inline,
			.code_size = ARRAYSIZE(g_pRaytracing_inline)
		};
		pipeline_subobject objects[] = {
			{
				.type = pipeline_subobject_type::compute_shader,
				.count = 1,
				.data = &shader_desc,
			}
		};

		ThrowIfFailed(s_d3d12device->create_pipeline_layout(ARRAYSIZE(params), params, &s_pipeline_layout));
		ThrowIfFailed(s_d3d12device->create_pipeline(s_pipeline_layout, ARRAYSIZE(objects), objects, &s_pipeline));
	}

	// init blit pipeline
	{
		pipeline_layout_param params[] = {
			pipeline_layout_param(descriptor_range{.binding = 0, .count = 1, .visibility = shader_stage::pixel, .type = descriptor_type::shader_resource_view }),
		};

		shader_desc vs_desc = {
			.code = g_pRaytracing_blit_vs,
			.code_size = ARRAYSIZE(g_pRaytracing_blit_vs)
		};

		shader_desc ps_desc = {
			.code = g_pRaytracing_blit_ps,
			.code_size = ARRAYSIZE(g_pRaytracing_blit_ps)
		};

		std::vector<pipeline_subobject> subobjects;
		subobjects.push_back({ pipeline_subobject_type::vertex_shader, 1, &vs_desc });
		subobjects.push_back({ pipeline_subobject_type::pixel_shader, 1, &ps_desc });

		format fmt = format::b8g8r8a8_unorm;
		subobjects.push_back({ pipeline_subobject_type::render_target_formats, 1, &fmt });

		rasterizer_desc raster = {
			.cull_mode = cull_mode::none,
		};
		subobjects.push_back({ pipeline_subobject_type::rasterizer_state, 1, &raster });

		depth_stencil_desc ds = {
			.depth_enable = false
		};
		subobjects.push_back({ pipeline_subobject_type::depth_stencil_state, 1, &ds });

		ThrowIfFailed(s_d3d12device->create_pipeline_layout(ARRAYSIZE(params), params, &s_blit_layout));
		ThrowIfFailed(s_d3d12device->create_pipeline(s_blit_layout, static_cast<uint32_t>(subobjects.size()), subobjects.data(), &s_blit_pipeline));
	}
}

static void init_default_resources()
{
	resource d3d12res;
	s_d3d12device->create_resource(
		resource_desc(sizeof(uint32_t), memory_heap::cpu_to_gpu, resource_usage::shader_resource),
		nullptr, resource_usage::cpu_access, &d3d12res);

	void *ptr;
	s_d3d12device->map_buffer_region(d3d12res, 0, sizeof(uint32_t), map_access::write_only, &ptr);
	uint32_t value = 0;
	memcpy(ptr, &value, sizeof(uint32_t));
	s_d3d12device->unmap_buffer_region(d3d12res);

	s_instance_id_buffers.push_back(scopedresource(s_d3d12device, d3d12res));

	resource_view_desc view_desc(format::r32_uint, 0, 1);

	resource_view srv;
	s_d3d12device->create_resource_view(d3d12res, resource_usage::shader_resource, view_desc, &srv);

	s_empty_buffer = scopedresource(s_d3d12device, d3d12res);
	s_empty_srv = scopedresourceview(s_d3d12device, srv);
}

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

		init_pipeline();
		init_default_resources();
		createDxrDevice(device);
		testCompilePso(device);
	}
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
			StreamInfo info;

			uint32_t stream_count = 0;

			for (uint32_t elemIdx = 0; elemIdx < object.count; elemIdx++)
			{
				const input_element &elem = reinterpret_cast<input_element *>(object.data)[elemIdx];

				stream_count = max(elem.buffer_binding + 1u, stream_count);

				if (strstr(elem.semantic, "POSITION") != nullptr)
				{
					assert(info.pos.format == format::unknown);
					info.pos.format = elem.format;
					info.pos.offset = elem.offset;
					info.pos.index = elem.buffer_binding;
					info.pos.stride = elem.stride;
				}
				else if (strstr(elem.semantic, "TEXCOORD") != nullptr)
				{
					info.uv.format = elem.format;
					info.uv.offset = elem.offset;
					info.uv.index = elem.buffer_binding;
					info.uv.stride = elem.stride;

					std::stringstream s;

					s << "vertex_buffer_stream_uv(" << handle.handle << ", ";

					if (info.uv.format == format::r32g32b32a32_float)
					{
						s << "float32x4";
					}
					else if (info.uv.format == format::r32g32b32_float)
					{
						s << "float32x3";
					}
					else if (info.uv.format == format::r32g32_float)
					{
						s << "float32x2";
					}
					else if (info.uv.format == format::r16g16b16a16_float)
					{
						s << "float16x4";
					}
					else if (info.uv.format == format::r16g16_float)
					{
						s << "float16x2";
					}
					else if (info.uv.format == format::r8g8b8a8_unorm)
					{
						s << "float8x4";
					}
					else if (info.uv.format == format::r8g8_unorm)
					{
						s << "float8x2";
					}
					else
					{
						s << "unkown";
					}
					s << ", " << elemIdx;
					s << ")";

					reshade::log_message(3, s.str().c_str());
				}
			}

			if (info.pos.format != format::unknown)
			{
				temp_mem<uint32_t> strides(stream_count);
				for (uint32_t s = 0; s < stream_count; s++)
					strides[s] = 0;

				// found our position stream, calculate the stride
				// the incoming stride is always 0, so we must manually calculate here
				for (uint32_t elemIdx = 0; elemIdx < object.count; elemIdx++)
				{
					const input_element &elem = reinterpret_cast<input_element *>(object.data)[elemIdx];

					strides[elem.buffer_binding] += format_size(elem.format);
				}

				info.pos.stride = strides[info.pos.index];
				if (info.uv.format != format::unknown)
				{
					info.uv.stride = strides[info.uv.index];
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
		else if (object.type == pipeline_subobject_type::vertex_shader)
		{
			shader_desc *shader_data = (shader_desc *)object.data;
			XXH64_hash_t hash = XXH3_64bits(shader_data->code, shader_data->code_size);

			s_vs_hash_map[handle.handle] = hash;
			if (hash == StaticGeoVsHash)
			{
				s_static_geo_vs_pipelines.insert(handle.handle);
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
	if (filter_command())
		return;

	s_current_vs_pipeline = { 0 };

	if (type == pipeline_stage::input_assembler)
	{
		s_currentInputLayout = pipeline;
	}
	else if(type == pipeline_stage::vertex_shader)
	{
		s_staticgeo_vs_pipeline_is_bound = s_static_geo_vs_pipelines.contains(pipeline.handle);

		s_current_vs_pipeline = pipeline;
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
	bool supported_resource = false;
	bool geo_buffer = false;
	bool texture = false;
	if ((desc.usage == resource_usage::vertex_buffer || desc.usage == resource_usage::index_buffer))
	{
		geo_buffer = true;
		supported_resource |= true;
	}
	else if (desc.type == resource_type::texture_2d && (desc.usage & resource_usage::render_target) == 0)
	{
		// make sure it's not dynamic? or strip later?
		// only use texture2d for now
		if (desc.texture.depth_or_layers == 1)
		{
			texture = true;
			supported_resource |= true;
		}		
	}

	if (!supported_resource)
		return;

	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	assert(s_resources.find(handle.handle) == s_resources.end());
	s_resources[handle.handle] = desc;

	if (geo_buffer)
	{
		resource d3d12res;
		s_d3d12device->create_resource(
			resource_desc(desc.buffer.size, memory_heap::cpu_to_gpu, desc.usage),
			nullptr, resource_usage::cpu_access, &d3d12res);

		s_shadow_resources[handle.handle] = std::move(scopedresource(s_d3d12device, d3d12res));
	}
	else if (texture)
	{
		resource_desc new_desc = desc;
		new_desc.heap = memory_heap::gpu_only;

		resource d3d12res;
		s_d3d12device->create_resource(
			new_desc,
			nullptr, resource_usage::shader_resource, &d3d12res);

		s_shadow_resources[handle.handle] = std::move(scopedresource(s_d3d12device, d3d12res));
	}
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

	resource_desc desc = device->get_resource_desc(resource);

	if (desc.usage == resource_usage::vertex_buffer || desc.usage == resource_usage::index_buffer)
	{
		if (s_resources.find(resource.handle) != s_resources.end())
		{
			
			s_mapped_resources[resource.handle] = MapRegion{
				.buffer = MapRegion::Buffer{
					.data = *data
				}
			};
			if (access == map_access::write_discard)
			{
				s_dynamic_resources.insert(resource.handle);
			}
		}
	}	
}
void on_unmap_buffer_region(device *device, resource handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	if (!s_ui_enable)
	{
		return;
	}

	//create shadow copy
	if (s_mapped_resources.find(handle.handle) != s_mapped_resources.end())
	{
		const MapRegion& region = s_mapped_resources[handle.handle];

		const resource_desc& desc = s_resources[handle.handle];

		auto iter = s_shadow_resources.find(handle.handle);
		assert(iter != s_shadow_resources.end());
		resource d3d12res = iter->second.handle();

		// TODO/HACK:
		// this is kind of bad what we're doing here. you should never read from a mapped pointer
		// and this is exactly what we're doing (data). but how else to get to the d3d9 data?
		void *ptr;
		s_d3d12device->map_buffer_region(d3d12res, 0, desc.buffer.size, map_access::write_only, &ptr);
		memcpy(ptr, region.buffer.data, (size_t)desc.buffer.size);
		s_d3d12device->unmap_buffer_region(d3d12res);

		if (!s_ui_pause)
			s_bvh_manager.on_geo_updated(s_shadow_resources[handle.handle].handle());
		s_mapped_resources.erase(handle.handle);
	}
}

static void on_map_texture_region(device *device, resource resource, uint32_t subresource, const subresource_box *box, map_access access, subresource_data *data)
{
	if (s_resources.find(resource.handle) != s_resources.end())
	{
		if (s_shadow_resources.contains(resource.handle))
		{
			s_mapped_resources[resource.handle] = MapRegion{
				.texture = {
					.subresource = subresource,
					.box = box ? *box : subresource_box{},
					.data = *data
				}
			};
		}
		
	}
}
static void on_unmap_texture_region(device *device, resource handle, uint32_t subresource)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	if (!s_ui_enable)
	{
		return;
	}

	//create shadow copy
	if (s_mapped_resources.find(handle.handle) != s_mapped_resources.end())
	{
		// execute command list crashes when d3d debug enabled
		if (!s_d3d_debug_enabled)
		{
			const MapRegion &region = s_mapped_resources[handle.handle];

			const resource_desc &desc = s_resources[handle.handle];

			auto iter = s_shadow_resources.find(handle.handle);
			assert(iter != s_shadow_resources.end());
			resource d3d12res = iter->second.handle();

			// TODO/HACK:
			// this is kind of bad what we're doing here. you should never read from a mapped pointer
			// and this is exactly what we're doing (data). but how else to get to the d3d9 data?
			const bool valid_box = region.texture.box.width() > 0 || region.texture.box.height() > 0 || region.texture.box.depth() > 0;
			s_d3d12device->update_texture_region(
				region.texture.data,
				d3d12res,
				region.texture.subresource,
				valid_box ? &region.texture.box : nullptr);
		}		

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

	// the scene draws straight to the backbuffer.
	// any other target is for secondary effects
	if (s_backbuffers.contains(new_main_rtv.handle))
	{
		s_current_rtv = new_main_rtv;
	}
	else
	{
		s_current_rtv.handle = 0;
	}
}
static void on_bind_index_buffer(command_list *cmd_list, resource buffer, uint64_t offset, uint32_t index_size)
{
	if (filter_command())
		return;

	s_currentIB.ib = buffer;
	s_currentIB.offset = (uint32_t)offset;
	s_currentIB.stride = index_size;
	s_currentIB.fmt = index_size == 2 ? format::r16_uint : format::r32_uint;

	resource_desc desc = cmd_list->get_device()->get_resource_desc(buffer);
	s_currentIB.size_bytes = desc.buffer.size;
}
static void on_bind_vertex_buffers(command_list *cmd_list, uint32_t first, uint32_t count, const resource *buffers, const uint64_t *offsets, const uint32_t *strides)
{
	if (filter_command())
		return;

	const StreamInfo &streamInfo = s_inputLayoutPipelines[s_currentInputLayout.handle];
	if (streamInfo.uv.format == format::unknown)
	{
		s_currentVB.uv = {};
	}

	if (first <= streamInfo.pos.index && count > (streamInfo.pos.index - first))
	{
		s_currentVB.pos.res = buffers[streamInfo.pos.index];
		s_currentVB.pos.offset = (uint32_t)offsets[streamInfo.pos.index];
		s_currentVB.pos.elem_offset = streamInfo.pos.offset;
		s_currentVB.pos.count = 0;
		s_currentVB.pos.stride = strides[streamInfo.pos.index];
		s_currentVB.pos.fmt = streamInfo.pos.format;

		resource_desc desc = cmd_list->get_device()->get_resource_desc(s_currentVB.pos.res);
		s_currentVB.pos.size_bytes = desc.buffer.size;
	}
	if (streamInfo.uv.format != format::unknown && first <= streamInfo.uv.index && count > (streamInfo.uv.index - first))
	{
		s_currentVB.uv.res = buffers[streamInfo.uv.index];
		s_currentVB.uv.offset = (uint32_t)offsets[streamInfo.uv.index];
		s_currentVB.uv.elem_offset = streamInfo.uv.offset;
		s_currentVB.uv.count = 0;
		s_currentVB.uv.stride = strides[streamInfo.uv.index];
		s_currentVB.uv.fmt = streamInfo.uv.format;

		resource_desc desc = cmd_list->get_device()->get_resource_desc(s_currentVB.uv.res);
		s_currentVB.uv.size_bytes = desc.buffer.size;
	}

	s_bvh_manager.update_vbs(std::span<const resource>(buffers, (size_t)count));
}
static void on_push_constants(command_list *, shader_stage stages, pipeline_layout layout, uint32_t param_index, uint32_t first, uint32_t count, const uint32_t *values)
{
	if (filter_command())
		return;

	// early out for the wrong rtv bound
	if (s_current_rtv.handle == 0)
	{
		return;
	}

	if (s_staticgeo_vs_pipeline_is_bound && !s_got_viewproj)
	{
		//extract our viewproj matrix. it is the 1st value in the array
		s_got_viewproj = true;

		XMMATRIX *matrices = (XMMATRIX *)values;
		s_viewproj = matrices[0];

		XMFLOAT3X4 viewAffine = *((XMFLOAT3X4 *)&matrices[1]);
		s_view = XMMATRIX(
			XMLoadFloat4((XMFLOAT4*)viewAffine.m[0]),
			XMLoadFloat4((XMFLOAT4*)viewAffine.m[1]),
			XMLoadFloat4((XMFLOAT4*)viewAffine.m[2]),
			XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f));
		s_view = XMMatrixTranspose(s_view);
		s_view = XMMatrixInverse(0, s_view);
	}

	if (s_current_vs_pipeline.handle != 0)
	{
		// some vs do not have the WVP at slot 0
		// we hashed those shaders earlier and check them here
		{
			assert(s_vs_hash_map.contains(s_current_vs_pipeline.handle));
			const uint64_t hash = s_vs_hash_map[s_current_vs_pipeline.handle];
			if (auto offset = s_vs_transform_map.find(hash); offset != s_vs_transform_map.end())
			{
				//found a mapping, index by vector4 slot
				XMVECTOR *vectors = (XMVECTOR *)values;

				s_current_wvp = *((XMMATRIX *)&vectors[offset->second]);
			}
			else
			{
				//most vs have the same layout, assume so for now
				XMMATRIX *matrices = (XMMATRIX *)values;
				s_current_wvp = matrices[0];
			}
		}	
	}
}
static void on_push_descriptors(command_list *cmd_list, shader_stage stages, pipeline_layout layout, uint32_t param_index, const descriptor_set_update &update)
{
	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	resource res;
	resource_desc desc;

	switch (update.type)
	{
#if 0
	case descriptor_type::sampler:
		for (uint32_t i = 0; i < update.count; ++i)
			assert(static_cast<const sampler *>(update.descriptors)[i].handle == 0 || s_samplers.find(static_cast<const sampler *>(update.descriptors)[i].handle) != s_samplers.end());
		break;
#endif
	case descriptor_type::sampler_with_resource_view:
		for (uint32_t i = 0; i < update.count; ++i)
		{
			sampler_with_resource_view *sv = (sampler_with_resource_view *)update.descriptors;

			resource_view view = sv[i].view;
			if (view.handle != 0)
			{
				res = cmd_list->get_device()->get_resource_from_view(view);
				s_currentTextureBindings.slots[update.binding] = res;
			}
		}
		break;
#if 0
	case descriptor_type::shader_resource_view:
	case descriptor_type::unordered_access_view:
		for (uint32_t i = 0; i < update.count; ++i)
			assert(static_cast<const resource_view *>(update.descriptors)[i].handle == 0 || s_resource_views.find(static_cast<const resource_view *>(update.descriptors)[i].handle) != s_resource_views.end());
		break;
	case descriptor_type::constant_buffer:
		for (uint32_t i = 0; i < update.count; ++i)
			assert(static_cast<const buffer_range *>(update.descriptors)[i].buffer.handle == 0 || s_resources.find(static_cast<const buffer_range *>(update.descriptors)[i].buffer.handle) != s_resources.end());
		break;
#endif
	default:
		break;
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
;
static bool on_draw_indexed(command_list * cmd_list, uint32_t index_count, uint32_t instances, uint32_t first_index, int32_t vertex_offset,
	/*uint32_t first_instance hack: interp instance offset as vertex count*/ uint32_t vertex_count)
{
	auto on_exit = sg::make_scope_guard([&]() {
		s_draw_count++;
	});

	if (filter_command())
		return false;

	if (s_current_rtv.handle == 0)
	{
		return false;
	}

	assert(s_shadow_resources.find(s_currentVB.pos.res.handle) != s_shadow_resources.end());
	assert(s_shadow_resources.find(s_currentIB.ib.handle) != s_shadow_resources.end());
	const bool dynamic_resource = s_dynamic_resources.contains(s_currentVB.pos.res.handle);

	BlasBuildDesc desc = {
		.vb = {
			.res = s_shadow_resources[s_currentVB.pos.res.handle].handle().handle,
			.offset = s_currentVB.pos.offset + (vertex_offset * s_currentVB.pos.stride),
			.count = vertex_count,
			.stride = s_currentVB.pos.stride,
			.fmt = s_currentVB.pos.fmt
		},
		.ib = {
			.res = s_shadow_resources[s_currentIB.ib.handle].handle().handle,
			.offset = s_currentIB.offset + (first_index * s_currentIB.stride),
			.count = index_count,
			.fmt = s_currentIB.fmt
		},
	};

	bvh_manager::AttachmentDesc attachments[] = {
		// ib
		{
			.res = s_shadow_resources[s_currentIB.ib.handle].handle().handle,
			.type = resource_type::buffer,
			.offset = (s_currentIB.offset + (first_index * s_currentIB.stride)) / s_currentIB.stride,
			.count = index_count,
			.stride = s_currentIB.stride,
			.fmt = s_currentIB.fmt,
		},
		// vb
		{
			.res = s_shadow_resources[s_currentVB.pos.res.handle].handle(),
			.type = resource_type::buffer,
			.offset = (s_currentVB.pos.offset + (vertex_offset * s_currentVB.pos.stride)) / s_currentVB.pos.stride,
			.elem_offset = s_currentVB.pos.elem_offset,
			.count = vertex_count,
			.stride = s_currentVB.pos.stride,
			.fmt = s_currentVB.pos.fmt,
			.view_as_raw = true,
		},
		// uv
		{
			// uv may not always be available
			.res = s_currentVB.uv.res.handle ? s_shadow_resources[s_currentVB.uv.res.handle].handle() : resource{0},
			.type = resource_type::buffer,
			.offset = s_currentVB.uv.res.handle ? (s_currentVB.uv.offset + (vertex_offset * s_currentVB.uv.stride)) / s_currentVB.uv.stride : 0,
			.elem_offset = s_currentVB.uv.elem_offset,
			.count = vertex_count,
			.stride = s_currentVB.uv.stride,
			.fmt = s_currentVB.uv.fmt,
			.view_as_raw = true,
		},
		// texture 0 (only if the texcoord is valid)
		{
			.res = s_currentVB.uv.res.handle ? s_shadow_resources[s_currentTextureBindings.slots[0].handle].handle() : resource{0},
			.type = resource_type::texture_2d,
			.fmt = format::unknown,
		},
	};

	bvh_manager::DrawDesc draw_desc = {
		.d3d9device = cmd_list->get_device(),
		.cmd_list = s_d3d12cmdlist,
		.cmd_queue = s_d3d12cmdqueue,
		.blas_desc = desc,
		.transform = s_current_wvp,
		.attachments = attachments,
		.dynamic = dynamic_resource,
	};

	const std::unique_lock<std::shared_mutex> lock(s_mutex);
	if(!s_ui_pause)
		s_bvh_manager.on_geo_draw(draw_desc);	

	return false;
}

static void on_present(effect_runtime *runtime)
{
	device *const device = runtime->get_device();
	auto &dev_data = device->get_private_data<device_data>();

	dev_data.hasRenderedThisFrame = false;

	doDeferredDeletes();

	s_ctrl_down = runtime->is_key_down(VK_CONTROL) || runtime->is_key_down(VK_LCONTROL);
	s_got_viewproj = false;
	s_draw_count = 0;
	s_bvh_manager.update();

	bool is_shift_down = runtime->is_key_down(VK_SHIFT) || runtime->is_key_down(VK_LSHIFT);
	if (s_ctrl_down && is_shift_down && (runtime->is_key_down('r') || runtime->is_key_down('R')))
	{
		s_bvh_manager.destroy();
	}
}

static void update_rt()
{
	s_tlas.free();

	s_tlas = s_bvh_manager.build_tlas(
		s_got_viewproj ? &s_viewproj : nullptr,
		s_d3d12cmdlist,
		s_d3d12cmdqueue);

	auto [buffer, srv] = s_bvh_manager.build_attachments(s_d3d12cmdlist);

	s_attachments_buffer.free();
	s_attachments_srv.free();
	s_attachments_buffer = std::move(buffer);
	s_attachments_srv = std::move(srv);
}

void updateCamera(effect_runtime *runtime)
{
	uint32_t mousex, mousey;
	runtime->get_mouse_cursor_position(&mousex, &mousey);
	float deltaX = float((int)s_mouse_x - (int)mousex) * 0.004f;
	float deltaY = float((int)s_mouse_y - (int)mousey) * 0.004f;
	s_mouse_x = mousex;
	s_mouse_y = mousey;

	if (s_ctrl_down)
	{
		s_cam_pitch += deltaY;
		s_cam_yaw += deltaX;
	}	

	constexpr float limit = XM_PIDIV2 - 0.01f;
	s_cam_pitch = max(-limit, s_cam_pitch);
	s_cam_pitch = min(+limit, s_cam_pitch);

	// keep longitude in sane range by wrapping
	if (s_cam_yaw > XM_PI)
	{
		s_cam_yaw -= XM_2PI;
	}
	else if (s_cam_yaw < -XM_PI)
	{
		s_cam_yaw += XM_2PI;
	}

	float y = sinf(s_cam_pitch);
	float r = cosf(s_cam_pitch);
	float z = r * cosf(s_cam_yaw);
	float x = r * sinf(s_cam_yaw);

	XMVECTOR dir = XMVectorSet(x, y, z, 0.0f) * XMVectorReplicate(1.0f);

	if (runtime->is_key_down('W'))
	{
		s_cam_pos += dir;
	}
	if (runtime->is_key_down('S'))
	{
		s_cam_pos -= dir;
	}
}

XMMATRIX getViewMatrix()
{
	if (s_ui_use_viewproj)
	{
		return XMMatrixInverse(0, s_viewproj);
	}

	return XMMatrixRotationRollPitchYaw(s_cam_pitch, s_cam_yaw, 0.0);
}

static void do_trace(uint32_t width, uint32_t height, resource_desc src_desc)
{
	if (s_width != width || s_height != height)
	{
		s_output.free();
		s_output_srv.free();
		s_output_uav.free();

		resource_desc desc = src_desc;
		desc.usage = resource_usage::unordered_access | resource_usage::shader_resource;
		desc.heap = memory_heap::gpu_only;

		resource res;
		s_d3d12device->create_resource(desc, nullptr, resource_usage::unordered_access, &res);
		s_output = scopedresource(s_d3d12device, res);

		resource_view_desc view_desc(src_desc.texture.format);

		resource_view srv, uav;
		s_d3d12device->create_resource_view(res, resource_usage::unordered_access, view_desc, &uav);
		s_d3d12device->create_resource_view(res, resource_usage::shader_resource, view_desc, &srv);

		s_output_srv = scopedresourceview(s_d3d12device, srv);
		s_output_uav = scopedresourceview(s_d3d12device, uav);

		s_width = width;
		s_height = height;
	}
	else
	{
		s_d3d12cmdlist->barrier(s_output.handle(), resource_usage::shader_resource, resource_usage::unordered_access);
	}

	resource_view tlas_srv;
	{
		resource_view_desc tlas_srv_desc;
		tlas_srv_desc.type = resource_view_type::acceleration_structure;
		tlas_srv_desc.acceleration_structure.offset = 0;
		tlas_srv_desc.acceleration_structure.resource = s_tlas.handle();

		s_d3d12device->create_resource_view(s_tlas.handle(), resource_usage::acceleration_structure, tlas_srv_desc, &tlas_srv);
		scopedresourceview scoped_tlas_view(s_d3d12device, tlas_srv);
	}

	CameraCb cb;
	cb.view = getViewMatrix();
	cb.pos = s_cam_pos;
	cb.fov = tan(s_ui_fov * XM_PI / 180.0f);
	cb.usePrebuiltCamMat = s_ui_use_viewproj;
	cb.showNormal = s_ui_show_normals;
	cb.showUvs = s_ui_show_uvs;
	cb.showTexture = s_ui_show_texture;
	if (cb.usePrebuiltCamMat)
	{
		cb.pos = s_view.r[3];
	}

	auto get_srv = [&](scopedresourceview& srv)
	{
		return srv.handle().handle ? srv.handle() : s_empty_srv.handle();
	};

	resource_view srvs[] = {
		get_srv(s_instances_buffer_srv),
		get_srv(s_attachments_srv),
	};

	//update descriptors
	descriptor_set_update updates[] = {
		{
			.count = 1,
			.type = descriptor_type::acceleration_structure,
			.descriptors = &tlas_srv, // bind tlas srv
		},
		{
			.count = 2,
			.type = descriptor_type::shader_resource_view,
			.descriptors = srvs, // bind tlas srv
		},
		{
			.count = 1,
			.type = descriptor_type::unordered_access_view,
			.descriptors = &s_output_uav, // bind output uav
		}
	};

	s_d3d12cmdlist->bind_pipeline_layout(pipeline_stage::compute_shader, s_pipeline_layout);
	s_d3d12cmdlist->bind_pipeline(pipeline_stage::compute_shader, s_pipeline);
	uint32_t param_index = 0;
	for (param_index = 0; param_index < ARRAYSIZE(updates); param_index++)
	{
		if(((uint64_t*)updates[param_index].descriptors)[0] != 0)
			s_d3d12cmdlist->push_descriptors(shader_stage::compute, s_pipeline_layout, param_index, updates[param_index]);
	}
		
	s_d3d12cmdlist->push_constants(shader_stage::compute, s_pipeline_layout, param_index, 0, sizeof(CameraCb)/sizeof(int), &cb);

	// dispatch
	const uint32_t groupX = (width + 7) / 8;
	const uint32_t groupY = (height + 7) / 8;
	s_d3d12cmdlist->dispatch(groupX, groupY, 1);

	s_d3d12cmdlist->barrier(s_output.handle(), resource_usage::unordered_access, resource_usage::shader_resource);
}

void blit(uint32_t width, uint32_t height, resource_desc target_desc, resource target)
{
	// debug layer causes a crash when binding the unwrapped render target
	if (s_d3d_debug_enabled)
		return;

	resource_view rtv;
	{
		resource_view_desc rtv_desc(target_desc.texture.format);
		s_d3d12device->create_resource_view(target, resource_usage::render_target, rtv_desc, &rtv);
		scopedresourceview delayfree(s_d3d12device, rtv);
	}

	s_d3d12cmdlist->bind_pipeline(pipeline_stage::all_graphics, s_blit_pipeline);

	render_pass_render_target_desc rt_desc = {
		.view = rtv,
	};
	s_d3d12cmdlist->begin_render_pass(1, &rt_desc, nullptr);

	descriptor_set_update updates[] = {
	   {
		   .count = 1,
		   .type = descriptor_type::shader_resource_view,
		   .descriptors = &s_output_srv, // bind output of trace pass
	   },
	};
	s_d3d12cmdlist->push_descriptors(shader_stage::pixel, s_blit_layout, 0, updates[0]);

	const viewport viewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
	s_d3d12cmdlist->bind_viewports(0, 1, &viewport);
	const rect scissor_rect = { 0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height) };
	s_d3d12cmdlist->bind_scissor_rects(0, 1, &scissor_rect);

	s_d3d12cmdlist->draw(3, 1, 0, 0);

	s_d3d12cmdlist->end_render_pass();
}

void on_tech_render(effect_runtime *runtime, effect_technique technique, command_list *cmd_list, resource_view rtv, resource_view rtv_srgb)
{
	//const auto tech = reinterpret_cast<const technique *>(technique.handle);

	

	// check name, grab resources, submit commandlist
}

bool on_tech_pass_render(effect_runtime *runtime, effect_technique technique, command_list *cmd_list, size_t pass_index)
{
	if (!s_ui_enable)
	{
		return false;
	}

	size_t nameLength = 0;
	runtime->get_technique_name(technique, 0, &nameLength);
	nameLength += 1;

	std::string name;
	name.resize(nameLength);
	runtime->get_technique_name(technique, name.data(), &nameLength);

	//make sure this is the right technique
	if (strstr(name.c_str(), "Raytracing") == nullptr)
	{
		return false;
	}

	//we replace the "dummy" technique pass at index 0
	if (pass_index != 0)
	{
		//update the pass uniforms 1st before returning
		auto full = runtime->find_uniform_variable("Simple.fx", "g_showRtResultFull");
		auto half = runtime->find_uniform_variable("Simple.fx", "g_showRtResultHalf");

		runtime->set_uniform_value_bool(full, &s_ui_show_rt_full, 1);
		runtime->set_uniform_value_bool(half, &s_ui_show_rt_half, 1);
		return false;
	}

	descriptor_set outputs;
	span<const resource> resources;
	runtime->get_technique_pass_storage(technique, pass_index, &outputs);
	runtime->get_technique_pass_resources(technique, pass_index, &resources);

	if (resources.size() == 0)
		return false;

	update_rt();

	if (s_tlas.handle() == 0)
	{
		return false;
	}

	updateCamera(runtime);

	resource output_target = resources[0];

	resource res12 = lock_resource(runtime->get_device(), s_d3d12cmdqueue, output_target);
	{
		resource_desc src_desc = s_d3d12device->get_resource_desc(res12);

		uint32_t width, height;
		runtime->get_screenshot_width_and_height(&width, &height);
		do_trace(width, height, src_desc);
		blit(width, height, src_desc, res12);
	}

	uint64_t signal = 0, fence = 0;
	s_d3d12cmdqueue->flush_immediate_command_list(&signal, &fence);

	ID3D12Fence *fence12 = reinterpret_cast<ID3D12Fence *>(fence);
	unlock_resource(runtime->get_device(), signal, fence12, output_target);

	//return true to skip the reshade runtime from drawing the pass
	return true;
}

static void draw_ui(reshade::api::effect_runtime *)
{
	ImGui::Checkbox("Enable", &s_ui_enable);
	ImGui::Checkbox("Pause", &s_ui_pause);

	ImGui::SliderFloat("ViewRotX: ", &s_ui_view_rot_x, -180.0f, 180.0f);
	ImGui::SliderFloat("ViewRotY: ", &s_ui_view_rot_y, -180.0f, 180.0f);
	ImGui::SliderFloat("ViewRotZ: ", &s_ui_view_rot_z, -180.0f, 180.0f);
	ImGui::SliderFloat("ViewFov: ", &s_ui_fov, 0, 90.0f);

	ImGui::Checkbox("UseViewProjMat", &s_ui_use_viewproj);
	ImGui::Checkbox("Show Rt result fullscreen", &s_ui_show_rt_full);
	ImGui::Checkbox("Show Rt result halfscreen", &s_ui_show_rt_half);

	ImGui::SliderInt("DrawCallBegin: ", &s_ui_drawCallBegin, 0, s_draw_count);
	ImGui::SliderInt("DrawCallEnd: ", &s_ui_drawCallEnd, 0, s_draw_count);

	ImGui::Checkbox("Show normals", &s_ui_show_normals);
	ImGui::Checkbox("Show uvs", &s_ui_show_uvs);
	ImGui::Checkbox("Show texture", &s_ui_show_texture);
}

static void on_init_runtime(effect_runtime *runtime)
{
	reshade::config_get_value(runtime, "APP", "EnableGraphicsDebugLayer", s_d3d_debug_enabled);
}

static void do_init()
{
	init_vs_mappings();
}

static void do_shutdown()
{
	s_tlas.free();
	s_output.free();

	s_output_uav.free();
	s_output_srv.free();

	s_bvh_manager.destroy();

	s_instance_id_buffers.clear();
	s_instance_id_srvs.clear();

	s_instances_buffer.free();
	s_instances_buffer_srv.free();

	s_attachments_buffer.free();
	s_attachments_srv.free();

	s_empty_buffer.free();
	s_empty_srv.free();

	s_shadow_resources.clear();

	// all resource frees must happen before this as they will add to the deferred delete list
	// otherwise the delete order doesn't matter (resources vs srvs)
	doDeferredDeletesAll();
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
		reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
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
		reshade::register_event<reshade::addon_event::map_texture_region>(on_map_texture_region);
		reshade::register_event<reshade::addon_event::unmap_texture_region>(on_unmap_texture_region);
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
		reshade::register_event<reshade::addon_event::push_constants>(on_push_constants);
		reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_event<reshade::addon_event::reshade_render_technique_pass>(on_tech_pass_render);
		reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_runtime);

		reshade::register_overlay(nullptr, draw_ui);

		do_init();
		register_state_tracking();
		break;
    case DLL_PROCESS_DETACH:
		do_shutdown();
		unregister_state_tracking();

		reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}

