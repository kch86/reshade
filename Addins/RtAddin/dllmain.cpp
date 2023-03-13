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
#include <fstream>
#include <iostream>
#include <sstream>
#include "psapi.h"

// dx12
#include <d3d12.h>
#include <d3d9on12.h>
#include "dxhelpers.h"

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION; }

extern "C" { __declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\"; }

//ray tracing includes
#include "raytracing.h"
#include "bvh_manager.h"
#include "Shaders/RtShared.h"
#include "hash.h"
#include "camera.h"
#include "timing.h"
#include "sample_gen.h"
#include "profiling.h"

#define INCLUDE_RT_SHADERS 0
#if INCLUDE_RT_SHADERS
#include "CompiledShaders/Raytracing_inline.hlsl.h"
#endif
#include "CompiledShaders/Raytracing_blit_vs.hlsl.h"
#include "CompiledShaders/Raytracing_blit_ps.hlsl.h"

using namespace reshade::api;
using namespace DirectX;

namespace math
{
	constexpr float DegToRad = XM_PI / 180.0f;
	constexpr float RadToDeg = 180.0f / XM_PI;
}

namespace
{
	struct MapRegion
	{
		struct Buffer {
			void *data;
			void *dst_data;
			uint64_t offset;
			uint64_t size;
			map_access flags;
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
		stream normal;
		stream color;
		stream uv;
	};

	struct StreamData
	{
		struct stream
		{
			resource res = {};
			uint32_t offset = 0;
			uint32_t elem_offset = 0;
			uint32_t stride = 0;
			uint64_t size_bytes = 0;
			format fmt = format::unknown;
		};

		stream index;
		stream pos;
		stream normal;
		stream color;
		stream uv;
	};

	struct TextureBindings
	{
		resource slots[4];
		void clear()
		{
			for (auto &res : slots)
				res.handle = 0;
		}
	};

	// wrapper for updatable buffers
	// if a map_discard comes along, then we create a 2nd resource to ping pong with
	struct DynamicResource
	{
		scopedresource res[2];
		scopedresourceview view[2];
		resource_desc desc;
		resource_view_desc view_desc;
		device *dev;
		uint32_t map_index = 0;

		DynamicResource() = default;

		DynamicResource(device* _dev, resource _res, const resource_desc& _desc, resource_view _view, const resource_view_desc& _view_desc)
		{
			res[0] = std::move(scopedresource(_dev, _res));
			view[0] = std::move(scopedresourceview(_dev, _view));
			desc = _desc;
			view_desc = _view_desc;
			dev = _dev;
		}

		void set_dynamic()
		{
			// only supported by buffers
			assert(desc.type == resource_type::buffer);
			if (res[1].handle().handle == 0)
			{
				resource d3d12res;
				dev->create_resource(
					desc,
					nullptr, resource_usage::cpu_access, &d3d12res);

				res[1] = std::move(scopedresource(dev, d3d12res));

				resource_view srv{};
				dev->create_resource_view(d3d12res, resource_usage::shader_resource, view_desc, &srv);

				view[1] = std::move(scopedresourceview(dev, srv));
			}
		}

		resource get_map_resource()
		{
			if (res[1].handle().handle)
			{
				uint32_t index = map_index & 1;
				assert(index == 0 || index == 1);
				map_index++;
				return res[index].handle();
			}

			return res[0].handle();
		}

		resource handle()
		{
			if (res[1].handle().handle)
			{
				uint32_t index = map_index & 1;
				assert(index == 0 || index == 1);
				return res[1-index].handle();
			}

			return res[0].handle();
		}

		resource_view srv()
		{
			if (view[1].handle().handle)
			{
				uint32_t index = map_index & 1;
				assert(index == 0 || index == 1);
				return view[1 - index].handle();
			}

			return view[0].handle();
		}
	};

	struct MaterialMapping
	{
		// offsets in float4 slots as in the shader disasm
		int diffuse_offset;
		int specular_offset;
		int specular_power_offset;
		int env_power_offset;
		int min_spec_offset;
	};

	struct FrameState
	{
		TextureBindings bindings;
		StreamData stream_data;
		Material mtrl;
		XMMATRIX wvp = XMMatrixIdentity();

		pipeline vs = {};
		pipeline ps = {};
		pipeline il = {};
		resource_view rtv = {};
		resource_desc rtv_desc = {};
		resource reflection_texture = {};

		uint32_t draw_count = 0;
		uint32_t width;
		uint32_t height;

		blend_factor dst_blend = blend_factor::zero;
		bool blend_enable = false;
		bool alpha_test_enable = false;
		bool null_shader_has_been_bound = false;
		bool static_geo_shader_is_bound = false;
		bool got_viewproj = false;
		bool rtv_is_main_backbuffer = false;

		void reset()
		{
			*this = {};
			bindings.clear();
		}
	};

	std::shared_mutex s_mutex;
	std::unordered_set<uint64_t> s_backbuffers;
	std::unordered_map<uint64_t, resource_desc> s_resources;
	std::unordered_map<uint64_t, DynamicResource> s_shadow_resources;
	std::unordered_map<uint64_t, MapRegion> s_mapped_resources;
	std::unordered_set<uint64_t> s_dynamic_resources;
	std::unordered_map<uint64_t, StreamInfo> s_inputLayoutPipelines;
	scopedresource s_tlas;
	resource_view s_attachments_srv;
	resource_view s_instance_data_srv;
	scopedresource s_samples_buffer;
	scopedresourceview s_samples_srv;
	pipeline_layout s_pipeline_layout;
	pipeline s_pipeline;

	pipeline_layout s_blit_layout;
	pipeline s_blit_pipeline;

	scopedresource s_output;
	scopedresourceview s_output_uav, s_output_srv;
	scopedresource s_history;
	scopedresourceview s_history_uav;
	uint32_t s_width = 0, s_height = 0;

	FrameState s_frame_state;
	resource s_spec_cube_resource = { 0 };
	sampler s_spec_cube_sampler = {};

	scopedresource s_empty_buffer;
	scopedresourceview s_empty_srv;

	device *s_d3d12device = nullptr;
	command_list *s_d3d12cmdlist = nullptr;
	command_queue *s_d3d12cmdqueue = nullptr;

	float s_ui_fov = 60.0f;
	float s_ui_sun_azimuth = 0.0f;
	float s_ui_sun_elevation = 0.0f;
	float s_ui_sun_intensity = 2.0f;
	float s_ui_sun_radius = 0.1f;
	float s_ui_bounce_boost = 1.0f;
	int s_ui_drawCallBegin = 0;
	int s_ui_drawCallEnd = 4095;
	int s_ui_pathtrace_path_count = 3;
	int s_ui_pathtrace_iter_count = 2;
	int s_ui_show_debug = 0;
	int s_ui_debug_channel = 4;
	bool s_ui_use_game_camera = true;
	bool s_ui_show_rt = false;
	bool s_ui_transparent_enable = true;
	bool s_ui_enable = true;
	bool s_ui_pause = false;
	bool s_ui_render_before_ui = true;

	uint32_t s_mouse_x = 0;
	uint32_t s_mouse_y = 0;
	bool s_ctrl_down = false;

	const uint64_t StaticGeoVsHash = 18047787432603860385;
	std::unordered_set<uint64_t> s_static_geo_vs_pipelines;
	std::unordered_map<uint64_t, uint32_t> s_vs_transform_map;
	std::unordered_map<uint64_t, uint64_t> s_vs_hash_map;
	std::unordered_map<uint64_t, uint64_t> s_ps_hash_map;
	std::unordered_set<uint64_t> s_ps_texbinding_map;
	std::unordered_map<uint64_t, MaterialMapping> s_vs_material_map;

	const uint64_t UiVsHash = 9844442386646808009;
	const uint64_t UiPsHash = 15657049591930699901;
	std::unordered_set<uint64_t> s_ui_pipelines;

	bvh_manager s_bvh_manager;
	GameCamera s_game_camera;
	FpsCamera s_camera;
	uint32_t s_frame_id = 0;
	uint32_t s_rt_timer = 0;

	bool s_d3d_debug_enabled = false;
}

struct __declspec(uuid("7251932A-ADAF-4DFC-B5CB-9A4E8CD5D6EB")) device_data
{
	effect_runtime *main_runtime = nullptr;
	uint32_t offset_from_last_pass = 0;
	uint32_t last_render_pass_count = 1;
	uint32_t current_render_pass_count = 0;
	bool hasRenderedThisFrame = false;
};

bvh_manager::AttachmentDesc get_attach_desc(const StreamData::stream &stream, uint32_t count, uint32_t offset, bool is_raw)
{
	const resource_view srv = stream.res.handle == 0 ? resource_view{} : s_shadow_resources[stream.res.handle].srv();
	return
	{
		.srv = srv,
		.type = resource_type::buffer,
		.offset = srv == 0 ? 0 : (stream.offset + (offset * stream.stride)) / stream.stride,
		.elem_offset = stream.elem_offset,
		.stride = stream.stride,
		.fmt = stream.fmt,
		.view_as_raw = is_raw,
	};
}

StreamData::stream get_stream_data(
	command_list *cmd_list,
	const StreamInfo::stream &info,
	const resource *buffers,
	const uint64_t *offsets,
	const uint32_t *strides,
	uint32_t first,
	uint32_t count)
{
	if (info.format == format::unknown || !(first <= info.index && count > (info.index - first)))
	{
		return {};
	}

	resource res = buffers[info.index];
	resource_desc desc = cmd_list->get_device()->get_resource_desc(res);
	return
	{
		.res = res,
		.offset = (uint32_t)offsets[info.index],
		.elem_offset = info.offset,
		.stride = strides[info.index],
		.size_bytes = desc.buffer.size,
		.fmt = info.format,
	};

}

MaterialType get_material_type(FrameState frame)
{
	const uint64_t car_vs_hashes[] = {
		6550593362979704143,
		5461187419696972836
	};

	const uint64_t car_ps_hashes[] = {
		7314718503845779620
	};

	const uint64_t vshash = s_vs_hash_map[frame.vs.handle];
	const uint64_t pshash = s_ps_hash_map[frame.ps.handle];

	auto is_in_hash = [](std::span<const uint64_t> s, uint64_t hash)
	{
		for (uint64_t h : s)
			if (h == hash)
				return true;
		return false;
	};
	const bool is_car_shader = is_in_hash(car_vs_hashes, vshash) && is_in_hash(car_ps_hashes, pshash);

	MaterialType type = Material_Standard;

	if (is_car_shader)
	{
		type = Material_Coat;
		if (frame.blend_enable)
		{
			if (frame.dst_blend == blend_factor::one)
			{
				type = Material_Headlight;
			}
			else
			{
				type = Material_Glass;
			}
		}		
	}
	else if(frame.blend_enable)
	{
		if (frame.dst_blend == blend_factor::one)
		{
			type = Material_Standard_Additive;
		}
	}

	return type;
}

static void init_vs_mappings()
{
	// vs transform constants mappings
	{
		struct VsTransformMap
		{
			uint64_t hash;
			uint32_t offset; //offset in float4 slots as in the shader disasm
		};

		static VsTransformMap hashes[] = {
			{6550593362979704143, 74},
			{5461187419696972836, 10},
		};

		for (const VsTransformMap &mapping : hashes)
		{
			s_vs_transform_map[mapping.hash] = mapping.offset;
		}
	}

	// car constants mapping
	{
		struct VsMaterialMap
		{
			uint64_t hash;
			MaterialMapping mtrl;
		};

		static VsMaterialMap hashes[] = {
			{
				5461187419696972836,
				{
					.diffuse_offset = 18,
					.specular_offset = 20,
					.specular_power_offset = 22,
					.env_power_offset = 23,
					.min_spec_offset = 20,
				}
			},
			{
				6550593362979704143,
				{
					.diffuse_offset = 82,
					.specular_offset = -1,
					.specular_power_offset = -1,
					.env_power_offset = -1,
					.min_spec_offset = -1,
				}
			}
		};

		for (const VsMaterialMap &mapping : hashes)
		{
			s_vs_material_map[mapping.hash] = mapping.mtrl;
		}
	}
}

static void init_ps_mappings()
{
	static uint64_t hashes[] = {
		7314718503845779620,
	};

	for (uint64_t mapping : hashes)
	{
		s_ps_texbinding_map.insert(mapping);
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
	const int drawId = s_frame_state.draw_count;
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

static void load_rt_pipeline()
{
#if INCLUDE_RT_SHADERS == 0
	const char *file_name = "Shaders/Raytracing_inline.cso";
	std::ifstream t(file_name, std::ios::binary);

	t.seekg(0, std::ios::end);
	size_t size = (size_t)t.tellg();
	t.seekg(0);

	std::vector<char> buffer(size, 0);
	t.read(buffer.data(), size);
	t.close();

	shader_desc shader_desc = {
			.code = buffer.data(),
			.code_size = buffer.size(),
	};
	pipeline_subobject objects[] = {
		{
			.type = pipeline_subobject_type::compute_shader,
			.count = 1,
			.data = &shader_desc,
		}
	};
#else
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
#endif

	pipeline_layout_param params[] = {
			pipeline_layout_param(descriptor_range{.binding = 0, .dx_register_space = 0, .count = 1, .visibility = shader_stage::compute, .type = descriptor_type::sampler}),
			pipeline_layout_param(descriptor_range{.binding = 0, .dx_register_space = 0, .count = 1, .visibility = shader_stage::compute, .type = descriptor_type::acceleration_structure}),
			pipeline_layout_param(descriptor_range{.binding = 0, .dx_register_space = 1, .count = 4, .visibility = shader_stage::compute, .type = descriptor_type::shader_resource_view}),
			pipeline_layout_param(descriptor_range{.binding = 0, .count = 2, .visibility = shader_stage::compute, .type = descriptor_type::unordered_access_view}),
			pipeline_layout_param(constant_range{.binding = 0, .count = sizeof(RtConstants) / sizeof(int), .visibility = shader_stage::compute}),
	};

	pipeline_layout _layout;
	pipeline _pipeline;
	ThrowIfFailed(s_d3d12device->create_pipeline_layout(ARRAYSIZE(params), params, &_layout));
	ThrowIfFailed(s_d3d12device->create_pipeline(_layout, ARRAYSIZE(objects), objects, &_pipeline));

	if (s_pipeline_layout.handle)
	{
		s_d3d12device->destroy_pipeline_layout(s_pipeline_layout);
		s_d3d12device->destroy_pipeline(s_pipeline);
	}

	s_pipeline_layout = _layout;
	s_pipeline = _pipeline;
}
static void init_pipeline()
{
	// init rt pipeline
	{
		load_rt_pipeline();
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

	resource_view_desc view_desc(format::r32_uint, 0, 1);

	resource_view srv;
	s_d3d12device->create_resource_view(d3d12res, resource_usage::shader_resource, view_desc, &srv);

	s_empty_buffer = scopedresource(s_d3d12device, d3d12res);
	s_empty_srv = scopedresourceview(s_d3d12device, srv);

	sampler_desc s_desc{};
	s_d3d12device->create_sampler(s_desc, &s_spec_cube_sampler);

	// 8 for max path count
	auto [res, view] = sample_gen::gen_sobol_sequence(s_d3d12device, 8, 2048);
	s_samples_buffer = scopedresource(s_d3d12device, res);
	s_samples_srv = scopedresourceview(s_d3d12device, view);
}

static void on_init_device(device *device)
{
	device->create_private_data<device_data>();

	if (device->get_api() == device_api::d3d9)
	{
		ID3D12CommandQueue *d3d12queue = (ID3D12CommandQueue *)s_d3d12cmdqueue->get_native();
		//resource->SetPrivateData(__uuidof(device_proxy), &device_proxy, sizeof(device_proxy), 0);
		device->set_private_data(reinterpret_cast<const uint8_t *>(&__uuidof(d3d12queue)), (uint64_t)d3d12queue);
	}
	else if (device->get_api() == device_api::d3d12)
	{
		s_d3d12device = device;
	}
}
static void on_destroy_device(device *device)
{
	device->destroy_private_data<device_data>();
}

static void on_init_command_list(command_list *cmd_list)
{
	if (cmd_list->get_device()->get_api() == device_api::d3d12)
	{
		s_d3d12cmdlist = cmd_list;

		init_pipeline();
		init_default_resources();
	}
}
static void on_destroy_command_list(command_list *cmd_list)
{
}

static void on_init_command_queue(command_queue *cmd_queue)
{
	if (cmd_queue->get_device()->get_api() == device_api::d3d12)
	{
		if (!s_d3d12cmdqueue && (cmd_queue->get_type() & command_queue_type::graphics) != 0)
		{
			s_d3d12cmdqueue = cmd_queue;
			timing::init(s_d3d12cmdqueue, 64);
			s_rt_timer = timing::alloc_timer_handle();
		}
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

#if 0
			// todo: there are a bunch of texcoord streams. maybbe some are normals mislabeled...
			{
				std::stringstream s;
				s << "init_pipeline(input_layout, " << (void *)handle.handle << ", ";

				for (uint32_t elemIdx = 0; elemIdx < object.count; elemIdx++)
				{
					const input_element &elem = reinterpret_cast<input_element *>(object.data)[elemIdx];

					s << elem.semantic << ", ";
				}

				s << ")";
				reshade::log_message(3, s.str().c_str());
			}
#endif	

			for (uint32_t elemIdx = 0; elemIdx < object.count; elemIdx++)
			{
				const input_element &elem = reinterpret_cast<input_element *>(object.data)[elemIdx];

				stream_count = std::max(elem.buffer_binding + 1u, stream_count);

				if (strstr(elem.semantic, "POSITION") != nullptr)
				{
					assert(info.pos.format == format::unknown);
					info.pos.format = elem.format;
					info.pos.offset = elem.offset;
					info.pos.index = elem.buffer_binding;
					info.pos.stride = elem.stride;
				}
				// there are a lot of texcoord semantics, i'm surprised this is working at all?
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
				else if (strstr(elem.semantic, "NORMAL") != nullptr)
				{
					info.normal.format = elem.format;
					info.normal.offset = elem.offset;
					info.normal.index = elem.buffer_binding;
					info.normal.stride = elem.stride;

					std::stringstream s;

					s << "vertex_buffer_stream_normal(" << handle.handle << ", ";

					if (info.normal.format == format::r32g32b32a32_float)
					{
						s << "float32x4";
					}
					else if (info.normal.format == format::r32g32b32_float)
					{
						s << "float32x3";
					}
					else if (info.normal.format == format::r16g16b16a16_float)
					{
						s << "float16x4";
					}
					else
					{
						s << "unkown";
					}
					s << ", " << elemIdx;
					s << ")";

					reshade::log_message(3, s.str().c_str());
				}
				else if (strstr(elem.semantic, "COLOR") != nullptr)
				{
					info.color.format = elem.format;
					info.color.offset = elem.offset;
					info.color.index = elem.buffer_binding;
					info.color.stride = elem.stride;

					std::stringstream s;

					s << "vertex_buffer_stream_color(" << handle.handle << ", ";

					if (info.color.format == format::r32g32b32a32_float)
					{
						s << "float32x4";
					}
					else if (info.color.format == format::r32g32b32_float)
					{
						s << "float32x3";
					}
					else if (info.color.format == format::r16g16b16a16_float)
					{
						s << "float16x4";
					}
					else if (info.color.format == format::b8g8r8a8_unorm)
					{
						s << "float8x4";
					}
					else
					{
						s << "unkown";
						assert(false);
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
				if (info.normal.format != format::unknown)
				{
					info.normal.stride = strides[info.normal.index];
				}
				if (info.color.format != format::unknown)
				{
					info.color.stride = strides[info.color.index];
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
			else if (hash == UiVsHash)
			{
				s_ui_pipelines.insert(handle.handle);
			}
		}
		else if (object.type == pipeline_subobject_type::pixel_shader)
		{
			shader_desc *shader_data = (shader_desc *)object.data;
			XXH64_hash_t hash = XXH3_64bits(shader_data->code, shader_data->code_size);

			s_ps_hash_map[handle.handle] = hash;

			if (hash == UiPsHash)
			{
				s_ui_pipelines.insert(handle.handle);
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

	if (type == pipeline_stage::input_assembler)
	{
		s_frame_state.il = pipeline;
	}
	else if (type == pipeline_stage::vertex_shader)
	{
		s_frame_state.static_geo_shader_is_bound = s_static_geo_vs_pipelines.contains(pipeline.handle);

		s_frame_state.vs = pipeline;
	}
	else if (type == pipeline_stage::pixel_shader)
	{
		s_frame_state.ps = pipeline;
	}
}
static void on_bind_pipeline_states(command_list *, uint32_t count, const dynamic_state *states, const uint32_t *values)
{
	if (filter_command())
		return;

	for (uint32_t i = 0; i < count; ++i)
	{
		if (states[i] == dynamic_state::blend_enable)
		{
			s_frame_state.blend_enable = values[i];
			break;
		}
		else if (states[i] == dynamic_state::alpha_test_enable)
		{
			s_frame_state.alpha_test_enable = values[i];
			break;
		}
		else if (states[i] == dynamic_state::dest_color_blend_factor)
		{
			s_frame_state.dst_blend = (blend_factor)values[i];
		}
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
		//if (desc.texture.depth_or_layers == 1)
		//if((desc.flags & resource_flags::dynamic) == 0)
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
		resource_desc new_desc = resource_desc(desc.buffer.size, memory_heap::cpu_to_gpu, desc.usage);
		resource d3d12res;
		ThrowIfFailed(s_d3d12device->create_resource(
			new_desc,
			nullptr, resource_usage::cpu_access, &d3d12res));

		const bool view_as_raw = desc.usage == resource_usage::vertex_buffer;
		format fmt = desc.buffer.format;
		uint32_t offset = 0;
		uint32_t count = 0;
		resource_view_flags flags = resource_view_flags::shader_visible;
		if (view_as_raw)
		{
			fmt = format::r32_uint;
			flags |= resource_view_flags::raw;
			assert((desc.buffer.size % sizeof(uint32_t)) == 0);
			count = uint32_t(desc.buffer.size) / sizeof(uint32_t);
		}
		else
		{
			const uint32_t stride = desc.buffer.format == format::r16_uint ? 2 : 4;
			assert((desc.buffer.size % stride) == 0);
			count = uint32_t(desc.buffer.size) / stride;
		}

		resource_view_desc view_desc(fmt, 0, count);
		view_desc.flags = flags;

		resource_view srv{};
		s_d3d12device->create_resource_view(d3d12res, resource_usage::shader_resource, view_desc, &srv);

		s_shadow_resources[handle.handle] = DynamicResource(s_d3d12device, d3d12res, new_desc, srv, view_desc);
	}
	else if (texture)
	{
		resource_desc new_desc = desc;
		new_desc.heap = memory_heap::gpu_only;
		new_desc.flags &= ~resource_flags::dynamic;

		if (new_desc.texture.depth_or_layers > 1)
		{
			s_spec_cube_resource = handle;
		}

		resource d3d12res;
		s_d3d12device->create_resource(
			new_desc,
			nullptr, resource_usage::shader_resource, &d3d12res);

		resource_view_desc view_desc(new_desc.texture.format);
		view_desc.flags = resource_view_flags::shader_visible;

		resource_view srv{};
		s_d3d12device->create_resource_view(d3d12res, resource_usage::shader_resource, view_desc, &srv);

		s_shadow_resources[handle.handle] = DynamicResource(s_d3d12device, d3d12res, new_desc, srv, view_desc);
	}
}
static void on_destroy_resource(device *device, resource handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	//assert(s_resources.find(handle.handle) != s_resources.end());
	s_resources.erase(handle.handle);
}

bool on_map_buffer_region(device *device, resource handle, uint64_t offset, uint64_t size, map_access access, void **data)
{
	PROFILE_SCOPE("rtAddon::on_map_buffer");
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	if (!s_ui_enable)
	{
		return false;
	}

	resource_desc desc = device->get_resource_desc(handle);

	if (desc.usage == resource_usage::vertex_buffer || desc.usage == resource_usage::index_buffer)
	{
		if (s_resources.find(handle.handle) != s_resources.end())
		{
			resource_desc desc = device->get_resource_desc(handle);

			uint64_t map_size = size == UINT64_MAX ? desc.buffer.size : size;

			MapRegion region = MapRegion{
				.buffer = MapRegion::Buffer{
					.data = malloc((size_t)map_size),
					.dst_data = *data,
					.offset = offset,
					.size = map_size,
					.flags = access
				}
			};

			*data = region.buffer.data;

			s_mapped_resources[handle.handle] = region;
			if (access == map_access::write_discard)
			{
				s_dynamic_resources.insert(handle.handle);
				s_shadow_resources[handle.handle].set_dynamic();
			}
		}

		return true;
	}

	return false;
}
map_range on_unmap_buffer_region(device *device, resource handle)
{
	PROFILE_SCOPE("rtAddon::on_unmap_buffer");

	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	if (!s_ui_enable)
	{
		return map_range{};
	}

	//create shadow copy
	if (s_mapped_resources.find(handle.handle) != s_mapped_resources.end())
	{
		const MapRegion &region = s_mapped_resources[handle.handle];
		if (!s_ui_pause)
		{
			const resource_desc &desc = s_resources[handle.handle];
			assert(region.buffer.size <= desc.buffer.size);
			assert((region.buffer.offset + region.buffer.size) <= desc.buffer.size);

			auto iter = s_shadow_resources.find(handle.handle);
			assert(iter != s_shadow_resources.end());
			resource d3d12res = iter->second.get_map_resource();

			void *ptr;
			s_d3d12device->map_buffer_region(d3d12res, region.buffer.offset, region.buffer.size, map_access::write_only, &ptr);
			memcpy(ptr, region.buffer.data, (size_t)region.buffer.size);
			s_d3d12device->unmap_buffer_region(d3d12res);

			s_bvh_manager.on_geo_updated(iter->second.handle());

			//defer free the malloc so the ptr is valid by the caller
			reshade::api::alloc handle = { (uint64_t)region.buffer.data };
			scopedalloc alloc(s_d3d12device, handle);
		}

		// copy the range data as the erase will clear out this data
		map_range range = map_range{
			.data = region.buffer.data,
			.dst_data = region.buffer.dst_data,
			.offset = region.buffer.offset,
			.size = region.buffer.size,
			.access_flags = region.buffer.flags
		};

		s_mapped_resources.erase(handle.handle);

		return range;
	}

	return map_range{};
}

static void on_map_texture_region(device *device, resource resource, uint32_t subresource, const subresource_box *box, map_access access, subresource_data *data)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	if (!s_ui_enable)
	{
		return;
	}

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
	const resource_view new_main_rtv = (count != 0) ? rtvs[0] : resource_view{ 0 };

	// the scene draws straight to the backbuffer.
	// any other target is for secondary effects
	if (s_backbuffers.contains(new_main_rtv.handle))
	{
		s_frame_state.rtv = new_main_rtv;
		device *d = cmd_list->get_device();
		s_frame_state.rtv_desc = d->get_resource_desc(d->get_resource_from_view(new_main_rtv));

		s_frame_state.rtv_is_main_backbuffer =
			s_frame_state.rtv_desc.texture.width == s_frame_state.width &&
			s_frame_state.rtv_desc.texture.height == s_frame_state.height;
	}
	else
	{
		s_frame_state.rtv.handle = 0;
		s_frame_state.reflection_texture = cmd_list->get_device()->get_resource_from_view(new_main_rtv);
		s_frame_state.rtv_is_main_backbuffer = false;
	}
}
static void on_bind_index_buffer(command_list *cmd_list, resource buffer, uint64_t offset, uint32_t index_size)
{
	if (filter_command())
		return;

	s_frame_state.stream_data.index.res = buffer;
	s_frame_state.stream_data.index.offset = (uint32_t)offset;
	s_frame_state.stream_data.index.elem_offset = 0;
	s_frame_state.stream_data.index.stride = index_size;
	s_frame_state.stream_data.index.fmt = index_size == 2 ? format::r16_uint : format::r32_uint;

	resource_desc desc = cmd_list->get_device()->get_resource_desc(buffer);
	s_frame_state.stream_data.index.size_bytes = desc.buffer.size;
}
static void on_bind_vertex_buffers(command_list *cmd_list, uint32_t first, uint32_t count, const resource *buffers, const uint64_t *offsets, const uint32_t *strides)
{
	if (filter_command())
		return;

	const StreamInfo &streamInfo = s_inputLayoutPipelines[s_frame_state.il.handle];

	s_frame_state.stream_data.pos = get_stream_data(cmd_list, streamInfo.pos, buffers, offsets, strides, first, count);
	s_frame_state.stream_data.normal = get_stream_data(cmd_list, streamInfo.normal, buffers, offsets, strides, first, count);
	s_frame_state.stream_data.uv = get_stream_data(cmd_list, streamInfo.uv, buffers, offsets, strides, first, count);
	s_frame_state.stream_data.color = get_stream_data(cmd_list, streamInfo.color, buffers, offsets, strides, first, count);

	s_bvh_manager.update_vbs(std::span<const resource>(buffers, (size_t)count));
}
static void on_push_constants(command_list *, shader_stage stages, pipeline_layout layout, uint32_t param_index, uint32_t first, uint32_t count, const uint32_t *values)
{
	if (filter_command())
		return;

	// early out for the wrong rtv bound
	if (s_frame_state.rtv.handle == 0)
	{
		return;
	}

	if (s_frame_state.static_geo_shader_is_bound && !s_frame_state.got_viewproj)
	{
		//extract our viewproj matrix. it is the 1st value in the array
		s_frame_state.got_viewproj = true;

		XMMATRIX *matrices = (XMMATRIX *)values;
		XMFLOAT3X4 viewAffine = *((XMFLOAT3X4 *)&matrices[1]);
		s_game_camera.set_view(viewAffine);
		s_game_camera.set_viewproj(matrices[0]);
	}

	if (s_frame_state.vs.handle != 0 && (stages & shader_stage::vertex) != 0)
	{
		// extract the wvp from vertex shader constants
		// some vs do not have the WVP at slot 0
		// we hashed those shaders earlier and check them here
		{
			assert(s_vs_hash_map.contains(s_frame_state.vs.handle));
			const uint64_t hash = s_vs_hash_map[s_frame_state.vs.handle];
			if (auto offset = s_vs_transform_map.find(hash); offset != s_vs_transform_map.end())
			{
				//found a mapping, index by vector4 slot
				XMVECTOR *vectors = (XMVECTOR *)values;

				s_frame_state.wvp = *((XMMATRIX *)&vectors[offset->second]);
			}
			else
			{
				//most vs have the same layout, assume so for now
				XMMATRIX *matrices = (XMMATRIX *)values;
				s_frame_state.wvp = matrices[0];
			}
		}

		// extract material data from vertex constants
		{
			assert(s_vs_hash_map.contains(s_frame_state.vs.handle));
			const uint64_t hash = s_vs_hash_map[s_frame_state.vs.handle];
			if (auto offset = s_vs_material_map.find(hash); offset != s_vs_material_map.end())
			{
				XMVECTOR *vectors = (XMVECTOR *)values;

				auto get_elem = [=](int offset, XMVECTOR default_value) {
					if (offset >= 0 && offset < (int)count)
					{
						return vectors[offset];
					}

					return default_value;
				};

				const float default_roughness = 0.8f;
				auto get_roughness = [=](XMVECTOR spec_power) {
					float s = XMVectorGetX(spec_power);
					float roughness = sqrtf(2.0f / (s + 2.0f));

					// TODO: this helps with the car paint, but hurts the tires
					// I probably need a per-mtrl override, where mtrl is vs/ps/texture bindings combo
					// analyze the spec min, range, and power. maybe this will be enough info to modify roughness accordingly
					roughness = powf(roughness, 5.0);
					return roughness;
				};

				// interpreting env power as roughness seems to work well
				// envpower is smoother 
				// there's a bug somewhere with either 0.0 or 1.0 roughness
				// clamp to [.1, .9]
				auto get_roughness_envpower = [=](XMVECTOR spec_power) {
					float s = 1.0f - XMVectorGetX(spec_power);
					if (s < 0.0f)
						s = 0.0f;
					if (s > 1.0f)
						s = 1.0f;

					float roughness = powf(s, 2.0f);
					return roughness;
				};

				auto get_elem_f = [=](int offset, float default_value) {
					if (offset >= 0 && offset < (int)count)
					{
						return XMVectorGetX(vectors[offset]);
					}

					return default_value;
				};

				// this seems to work well on the car
				// doesn't quite match every setup but looks ok most of the time
				auto get_roughness2 = [=](float spec_power, float spec_min, float env_power) {
					float s = spec_power;
					//float e = max(0.4f, env_power);
					float e = std::max(0.0f, std::min(1.0f, 1.0f - env_power));
					float m = spec_min == 0.0f ? 1.0f : spec_min;
					s = (s * m) / e;
					float roughness = sqrtf(2.0f / (s + 2.0f));

					roughness = powf(roughness, 2.0);
					return roughness;
				};

				s_frame_state.mtrl = {
					.diffuse = get_elem(offset->second.diffuse_offset, {1.0f, 1.0f, 1.0f, 1.0f}),
					//TODO most of the specular color values are bogus pre-pbr values
					.specular = {1.0f, 1.0f, 1.0f, 1.0f},// get_elem(offset->second.specular_offset, {0.0f, 0.0f, 0.0f, 0.0f}), 
					//.roughness = get_roughness(get_elem(offset->second.specular_power_offset, XMVectorReplicate(default_roughness))),
					//.roughness = get_roughness_envpower(get_elem(offset->second.env_power_offset, XMVectorReplicate(.1f))),
					.roughness = get_roughness2(get_elem_f(offset->second.specular_power_offset, .8f),
												get_elem_f(offset->second.min_spec_offset, .5f),
												get_elem_f(offset->second.env_power_offset, .8f)),
				};
			}
			else
			{
				s_frame_state.mtrl = {};
			}
		}
	}
}
static void on_push_descriptors(command_list *cmd_list, shader_stage stages, pipeline_layout layout, uint32_t param_index, const descriptor_set_update &update)
{
	if (filter_command())
		return;

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
				s_frame_state.bindings.slots[update.binding] = res;
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

static bool on_draw(command_list *cmd_list, uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance)
{
	PROFILE_SCOPE("rtAddon::on_draw");

	auto on_exit = sg::make_scope_guard([&]() {
		s_frame_state.draw_count++;
	});

	if (filter_command())
		return false;

	// null pipelines are bound and a draw occurs right before the ui is drawn
	if (s_frame_state.vs.handle == 0 && s_frame_state.ps.handle == 0)
	{
		s_frame_state.null_shader_has_been_bound = true;
	}

	// we filtered out this rtv at some point
	if (s_frame_state.rtv_is_main_backbuffer)
	{
		return false;
	}

	// Render post-processing effects when a specific render pass is found (instead of at the end of the frame)
	// This is not perfect, since there may be multiple command lists and this will try and render effects in every single one ...
	if (s_ui_render_before_ui && s_frame_state.null_shader_has_been_bound && s_ui_pipelines.contains(s_frame_state.vs.handle) && s_ui_pipelines.contains(s_frame_state.ps.handle))
	{
		device *const device = cmd_list->get_device();
		auto &dev_data = device->get_private_data<device_data>();

		// TODO: find last valid 3d render target and apply that before drawing
		const auto &current_state = cmd_list->get_private_data<state_block>();

		dev_data.main_runtime->render_effects(cmd_list, s_frame_state.rtv);

		// Re-apply state to the command-list, as it may have been modified by the call to 'render_effects'
		current_state.apply(cmd_list);
		dev_data.hasRenderedThisFrame = true;
	}

	return false;
}

static bool on_draw_indexed(command_list *cmd_list, uint32_t index_count, uint32_t instances, uint32_t first_index, int32_t vertex_offset,
	/*uint32_t first_instance hack: interp instance offset as vertex count*/ uint32_t vertex_count)
{
	PROFILE_SCOPE("rtAddon::on_draw_indexed");

	auto on_exit = sg::make_scope_guard([&]() {
		s_frame_state.draw_count++;
		s_frame_state.bindings.clear();
	});

	if (filter_command())
		return false;

	if (s_frame_state.rtv.handle == 0)
	{
		return false;
	}

	assert(s_shadow_resources.find(s_frame_state.stream_data.pos.res.handle) != s_shadow_resources.end());
	assert(s_shadow_resources.find(s_frame_state.stream_data.index.res.handle) != s_shadow_resources.end());
	const bool dynamic_resource = s_dynamic_resources.contains(s_frame_state.stream_data.pos.res.handle);

	BlasBuildDesc desc = {
		.vb = {
			.res = s_shadow_resources[s_frame_state.stream_data.pos.res.handle].handle().handle,
			.offset = s_frame_state.stream_data.pos.offset + (vertex_offset * s_frame_state.stream_data.pos.stride),
			.count = vertex_count,
			.stride = s_frame_state.stream_data.pos.stride,
			.fmt = s_frame_state.stream_data.pos.fmt
		},
		.ib = {
			.res = s_shadow_resources[s_frame_state.stream_data.index.res.handle].handle().handle,
			.offset = s_frame_state.stream_data.index.offset + (first_index * s_frame_state.stream_data.index.stride),
			.count = index_count,
			.fmt = s_frame_state.stream_data.index.fmt
		},
		.transparent = s_frame_state.blend_enable,
		.alphatest = s_frame_state.alpha_test_enable,
	};

	const bool has_uvs = s_frame_state.stream_data.uv.res.handle != 0;
	uint32_t texslot = 0;
	uint64_t texhandle = 0;

	// get the albedo texture slot
	{
		if (s_frame_state.ps.handle)
		{
			assert(s_ps_hash_map.contains(s_frame_state.ps.handle));
			const uint64_t hash = s_ps_hash_map[s_frame_state.ps.handle];
			if (s_ps_texbinding_map.contains(hash))
			{
				texslot = 1;
			}
		}
		if (texslot == 1 && s_frame_state.bindings.slots[1].handle)
		{
			texhandle = s_frame_state.bindings.slots[1].handle;
		}
		else
		{
			texhandle = s_frame_state.bindings.slots[0].handle;
		}
	}

	// is the reflection view bound?
	bool reflection_view_bound = false;
	for (auto &res : s_frame_state.bindings.slots)
	{
		if (res.handle == s_frame_state.reflection_texture)
		{
			reflection_view_bound = true;
			break;
		}
	}

	// this layout needs to match the layout in the shader. see: RtInstanceAttachments
	bvh_manager::AttachmentDesc attachments[] = {
		// ib
		get_attach_desc(s_frame_state.stream_data.index, index_count, first_index, false),
		// pos
		get_attach_desc(s_frame_state.stream_data.pos, vertex_count, vertex_offset, true),
		// uv
		get_attach_desc(s_frame_state.stream_data.uv, vertex_count, vertex_offset, true),
		// normal
		get_attach_desc(s_frame_state.stream_data.normal, vertex_count, vertex_offset, true),
		// vert color
		get_attach_desc(s_frame_state.stream_data.color, vertex_count, vertex_offset, true),
		// texture 0 (only if the texcoord is valid)
		{
			.srv = has_uvs && texhandle != 0 ? s_shadow_resources[texhandle].srv() : resource_view{0},
			.type = resource_type::texture_2d,
			.fmt = format::unknown,
		},
	};

	Material mtrl = s_frame_state.mtrl;
	mtrl.type = get_material_type(s_frame_state);
#if 0
	// disable this for now as multiplying roughness by texture.a seems to work well
	if (reflection_view_bound)
	{
		mtrl.roughness = 0.15f;
	}
#endif

	bvh_manager::DrawDesc draw_desc = {
		.d3d9device = cmd_list->get_device(),
		.cmd_list = s_d3d12cmdlist,
		.cmd_queue = s_d3d12cmdqueue,
		.blas_desc = desc,
		.transform = s_frame_state.wvp,
		.attachments = attachments,
		.material = mtrl,
		.dynamic = dynamic_resource,
		.is_static = s_frame_state.static_geo_shader_is_bound,
	};

	const std::unique_lock<std::shared_mutex> lock(s_mutex);
	if (!s_ui_pause)
		s_bvh_manager.on_geo_draw(draw_desc);

	return false;
}

static void on_present(effect_runtime *runtime)
{
	PROFILE_SCOPE("rtAddon::on_present");

	device *const device = runtime->get_device();
	auto &dev_data = device->get_private_data<device_data>();

	dev_data.hasRenderedThisFrame = false;

	doDeferredDeletes();

	timing::flush(s_d3d12cmdlist);

	s_ctrl_down = runtime->is_key_down(VK_CONTROL) || runtime->is_key_down(VK_LCONTROL);
	s_frame_state.draw_count = 0;
	s_bvh_manager.update();
	s_frame_state.reset();

	bool is_shift_down = runtime->is_key_down(VK_SHIFT) || runtime->is_key_down(VK_LSHIFT);
	if (s_ctrl_down && is_shift_down && (runtime->is_key_down('r') || runtime->is_key_down('R')))
	{
		s_bvh_manager.destroy();
	}
	else if (runtime->is_key_pressed(VK_F11))
	{
		load_rt_pipeline();
	}
	else if (runtime->is_key_pressed(VK_F12))
	{
		PROCESS_MEMORY_COUNTERS memCounter;
		BOOL result = GetProcessMemoryInfo(GetCurrentProcess(),
										   &memCounter,
										   sizeof(memCounter));

		const uint32_t in_use = memCounter.WorkingSetSize / 1024 / 1024;
		const uint32_t in_use_hw = memCounter.PeakWorkingSetSize / 1024 / 1024;
		std::stringstream s;
		s << "memory in use: " << in_use << "MB, HW: " << in_use_hw << "MB";
		reshade::log_message(3, s.str().c_str());
	}

	s_frame_id++;
	PROFILE_END_FRAME();
}

static void on_reset_cmd_list(command_list *cmd_list)
{
	//PROFILE_BEGIN("RtAddin::Frame");
}

static void on_execute_cmd_list(command_queue* cmd_queue, command_list *cmd_list)
{
	//PROFILE_END();
}

static void update_rt()
{
	s_tlas.free();

	s_tlas = s_bvh_manager.build_tlas(
		s_frame_state.got_viewproj ? &s_game_camera.get_viewproj() : nullptr,
		s_d3d12cmdlist,
		s_d3d12cmdqueue);

	// build all the bindless attachments
	s_attachments_srv = s_bvh_manager.build_attachments(s_d3d12cmdlist);

	// build the per instance data buffer
	s_instance_data_srv = s_bvh_manager.build_instance_data(s_d3d12cmdlist);
}

void updateCamera(effect_runtime *runtime)
{
	uint32_t mousex, mousey;
	runtime->get_mouse_cursor_position(&mousex, &mousey);
	float deltaX = float((int)s_mouse_x - (int)mousex) * 0.004f;
	float deltaY = float((int)s_mouse_y - (int)mousey) * 0.004f;
	s_mouse_x = mousex;
	s_mouse_y = mousey;

	if (s_ctrl_down && (fabs(deltaX) > 0.0f || fabs(deltaY) > 0.0f))
	{
		s_frame_id = 0;
		s_camera.rotate(deltaX, deltaY);
	}

	if (runtime->is_key_down('W'))
	{
		s_frame_id = 0;
		s_camera.move_forward(0.5f);
	}
	else if (runtime->is_key_down('S'))
	{
		s_frame_id = 0;
		s_camera.move_forward(-0.5f);
	}
	if (runtime->is_key_down('A'))
	{
		s_frame_id = 0;
		s_camera.move_lateral(-0.5f);
	}
	else if (runtime->is_key_down('D'))
	{
		s_frame_id = 0;
		s_camera.move_lateral(0.5f);
	}

	s_camera.set_fov(s_ui_fov * math::DegToRad);
}

XMMATRIX getViewMatrix()
{
	if (s_ui_use_game_camera)
	{
		return XMMatrixInverse(0, s_game_camera.get_viewproj());
	}

	return XMMatrixInverse(0, s_camera.get_viewproj());
}

XMVECTOR getViewPos()
{
	if (s_ui_use_game_camera)
	{
		return s_game_camera.get_pos();
	}

	return s_camera.get_pos();
}

XMFLOAT3 getSunDirection(float azimuth, float elevation)
{
	float sinTheta;
	float cosTheta;
	XMScalarSinCos(&sinTheta, &cosTheta, azimuth * math::DegToRad);

	float sinPhi;
	float cosPhi;
	XMScalarSinCos(&sinPhi, &cosPhi, elevation * math::DegToRad);

	return XMFLOAT3(sinTheta * cosPhi, cosTheta * cosPhi, sinPhi);
}

bool create_trace_resources(uint32_t width, uint32_t height, resource_desc src_desc)
{
	if (s_width != width || s_height != height)
	{
		// rt output
		{
			s_output.free();
			s_output_srv.free();
			s_output_uav.free();

			resource_desc desc = src_desc;
			desc.usage = resource_usage::unordered_access | resource_usage::shader_resource;
			desc.heap = memory_heap::gpu_only;
			desc.texture.format = format::r16g16b16a16_float;

			resource res;
			s_d3d12device->create_resource(desc, nullptr, resource_usage::unordered_access, &res);
			s_output = scopedresource(s_d3d12device, res);

			resource_view_desc view_desc(desc.texture.format);

			resource_view srv, uav;
			s_d3d12device->create_resource_view(res, resource_usage::unordered_access, view_desc, &uav);
			s_d3d12device->create_resource_view(res, resource_usage::shader_resource, view_desc, &srv);

			s_output_srv = scopedresourceview(s_d3d12device, srv);
			s_output_uav = scopedresourceview(s_d3d12device, uav);
		}

		// history
		{
			s_history.free();
			s_history_uav.free();

			resource_desc desc = src_desc;
			desc.usage = resource_usage::unordered_access | resource_usage::shader_resource;
			desc.heap = memory_heap::gpu_only;
			desc.texture.format = format::r16g16_float;

			resource res;
			s_d3d12device->create_resource(desc, nullptr, resource_usage::unordered_access, &res);
			s_history = scopedresource(s_d3d12device, res);

			resource_view_desc view_desc(desc.texture.format);

			resource_view uav;
			s_d3d12device->create_resource_view(res, resource_usage::unordered_access, view_desc, &uav);

			s_history_uav = scopedresourceview(s_d3d12device, uav);
		}

		s_width = width;
		s_height = height;

		return true;
	}

	return false;
}

static void do_trace(uint32_t width, uint32_t height, resource_desc src_desc)
{
	// if new resources weren't created (false), then barrier
	if (!create_trace_resources(width, height, src_desc))
	{
		resource resources[] = {
			s_output.handle(),
			s_history.handle()
		};
		resource_usage before[] = {
			resource_usage::shader_resource,
			resource_usage::unordered_access
		};
		resource_usage after[] = {
			resource_usage::unordered_access,
			resource_usage::unordered_access
		};
		const uint32_t count = ARRAYSIZE(resources);
		s_d3d12cmdlist->barrier(count, resources, before, after);
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
	resource_view spec_srv;
	{
		DynamicResource &res = s_shadow_resources[s_spec_cube_resource.handle];
		resource_view_desc view_desc(res.desc.texture.format);
		view_desc.type = resource_view_type::texture_cube;

		s_d3d12device->create_resource_view(res.handle(), resource_usage::shader_resource, view_desc, &spec_srv);
		scopedresourceview scoped_tlas_view(s_d3d12device, spec_srv);
	}

	RtConstants cb;
	cb.viewMatrix = getViewMatrix();
	cb.viewPos = getViewPos();
	cb.debugView = (DebugViewEnum)s_ui_show_debug;
	cb.debugChannel = s_ui_debug_channel;
	cb.transparentEnable = s_ui_transparent_enable;
	cb.sunDirection = getSunDirection(s_ui_sun_azimuth, s_ui_sun_elevation);
	cb.sunIntensity = s_ui_sun_intensity;
	cb.sunRadius = s_ui_sun_radius;
	cb.pathCount = s_ui_pathtrace_path_count;
	cb.iterCount = s_ui_pathtrace_iter_count;
	cb.frameIndex = s_frame_id;
	cb.bounceBoost = s_ui_bounce_boost;

	auto get_srv = [&](scopedresourceview &srv)
	{
		return srv.handle().handle ? srv.handle() : s_empty_srv.handle();
	};
	auto get_srv2 = [&](resource_view &srv)
	{
		return srv.handle ? srv : s_empty_srv.handle();
	};

	sampler samplers[] = {
		s_spec_cube_sampler
	};

	resource_view srvs[] = {
		get_srv2(s_attachments_srv),
		get_srv2(s_instance_data_srv),
		get_srv2(spec_srv),
		get_srv(s_samples_srv)
	};

	resource_view uavs[] = {
		s_output_uav.handle(),
		s_history_uav.handle(),
	};

	//update descriptors
	descriptor_set_update updates[] = {
		{
			.count = ARRAYSIZE(samplers),
			.type = descriptor_type::sampler,
			.descriptors = samplers, // bind tlas srv
		},
		{
			.count = 1,
			.type = descriptor_type::acceleration_structure,
			.descriptors = &tlas_srv, // bind tlas srv
		},
		{
			.count = ARRAYSIZE(srvs),
			.type = descriptor_type::shader_resource_view,
			.descriptors = srvs, // bind tlas srv
		},
		{
			.count = ARRAYSIZE(uavs),
			.type = descriptor_type::unordered_access_view,
			.descriptors = uavs, // bind output uav
		}
	};

	s_d3d12cmdlist->bind_pipeline_layout(pipeline_stage::compute_shader, s_pipeline_layout);
	s_d3d12cmdlist->bind_pipeline(pipeline_stage::compute_shader, s_pipeline);
	uint32_t param_index = 0;
	for (param_index = 0; param_index < ARRAYSIZE(updates); param_index++)
	{
		if (((uint64_t *)updates[param_index].descriptors)[0] != 0)
			s_d3d12cmdlist->push_descriptors(shader_stage::compute, s_pipeline_layout, param_index, updates[param_index]);
	}

	s_d3d12cmdlist->push_constants(shader_stage::compute, s_pipeline_layout, param_index, 0, sizeof(RtConstants) / sizeof(int), &cb);

	timing::start_timer(s_d3d12cmdlist, s_rt_timer);
	{

		// dispatch
		const uint32_t groupX = (width + 7) / 8;
		const uint32_t groupY = (height + 7) / 8;
		s_d3d12cmdlist->dispatch(groupX, groupY, 1);
	}
	timing::stop_timer(s_d3d12cmdlist, s_rt_timer);

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
	PROFILE_SCOPE("rtAddon::on_tech_pass_render");

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
		auto full = runtime->find_uniform_variable("Simple.fx", "g_showRtResult");
		runtime->set_uniform_value_bool(full, &s_ui_show_rt, 1);
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

	// don't do the trace/blit if we're not displaying
	if ((s_ui_show_rt) == false)
	{
		// if we skip "rendering", we still need to flush the d3d12 command list
		s_d3d12cmdqueue->flush_immediate_command_list();
		return true;
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

	timing::set_fence(fence, signal);

	//return true to skip the reshade runtime from drawing the pass
	return true;
}

static void draw_ui(reshade::api::effect_runtime *)
{
	ImGui::Checkbox("Enable", &s_ui_enable);
	ImGui::Checkbox("Pause", &s_ui_pause);

	ImGui::SliderFloat("ViewFov: ", &s_ui_fov, 0, 90.0f);

	bool use_game_camera = s_ui_use_game_camera;
	ImGui::Checkbox("UseGameCamera", &s_ui_use_game_camera);
	ImGui::Checkbox("Show Rt result", &s_ui_show_rt);
	ImGui::Checkbox("Render Before UI", &s_ui_render_before_ui);

	ImGui::SliderInt("DrawCallBegin: ", &s_ui_drawCallBegin, 0, s_frame_state.draw_count);
	ImGui::SliderInt("DrawCallEnd: ", &s_ui_drawCallEnd, 0, s_frame_state.draw_count);

	// debug view elements
	{
		const char *debug_views[] = {
		"none", "instanceid", "normals", "uvs", "texture", "color", "motion",
		};
		static_assert(ARRAYSIZE(debug_views) == DebugView_Count);

		static const char *selected_debug = debug_views[0];
		if (ImGui::BeginCombo("Draw Debug", selected_debug))
		{
			for (int n = 0; n < ARRAYSIZE(debug_views); n++)
			{
				CONST bool is_selected = (selected_debug == debug_views[n]); // You can store your selection however you want, outside or inside your objects
				if (ImGui::Selectable(debug_views[n], is_selected))
				{
					selected_debug = debug_views[n];
					s_ui_show_debug = n;
				}

				if (is_selected)
				{
					ImGui::SetItemDefaultFocus();   // You may set the initial focus when opening the combo (scrolling + for keyboard navigation support)
					s_ui_show_debug = n;
				}

			}
			ImGui::EndCombo();
		}
		if (ImGui::BeginTable("channel_table", 5))
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (ImGui::RadioButton("R", s_ui_debug_channel == 0))
			{
				s_ui_debug_channel = 0;
			}
			ImGui::TableNextColumn();
			if (ImGui::RadioButton("G", s_ui_debug_channel == 1))
			{
				s_ui_debug_channel = 1;
			}
			ImGui::TableNextColumn();
			if (ImGui::RadioButton("B", s_ui_debug_channel == 2))
			{
				s_ui_debug_channel = 2;
			}
			ImGui::TableNextColumn();
			if (ImGui::RadioButton("A", s_ui_debug_channel == 3))
			{
				s_ui_debug_channel = 3;
			}
			ImGui::TableNextColumn();
			if (ImGui::RadioButton("RGB", s_ui_debug_channel == 4))
			{
				s_ui_debug_channel = 4;
			}
			ImGui::EndTable();
		}
	}	

	ImGui::Checkbox("Enable Transparent", &s_ui_transparent_enable);

	ImGui::SliderFloat("Sun Azimuth: ", &s_ui_sun_azimuth, 0.0f, 360.0f);
	ImGui::SliderFloat("Sun Elevation: ", &s_ui_sun_elevation, -90.0f, 90.0f);
	ImGui::InputFloat("Sun Intensity: ", &s_ui_sun_intensity, 0.1f, 0.5f);
	ImGui::InputFloat("Sun Radius: ", &s_ui_sun_radius, 0.05f, 0.5f);

	int path_count = s_ui_pathtrace_path_count;
	ImGui::SliderInt("Pathtrace path count: ", &s_ui_pathtrace_path_count, 1, 10);
	ImGui::SliderInt("Pathtrace iter count: ", &s_ui_pathtrace_iter_count, 1, 10);
	ImGui::InputFloat("Pathtrace bounce boost", &s_ui_bounce_boost, 0.1f, 0.5f);

	float timer = timing::get_timer_value(s_rt_timer);
	ImGui::Text("trace: %.2fms", timer);

	if (path_count != s_ui_pathtrace_path_count || use_game_camera != s_ui_use_game_camera)
	{
		s_frame_id = 0;
	}
}

static void on_init_runtime(effect_runtime *runtime)
{
	reshade::config_get_value(runtime, "APP", "EnableGraphicsDebugLayer", s_d3d_debug_enabled);
	reshade::config_get_value(runtime, "LIGHTING", "SunAzimuth", s_ui_sun_azimuth);
	reshade::config_get_value(runtime, "LIGHTING", "SunElevation", s_ui_sun_elevation);

	uint32_t width;
	uint32_t height;
	runtime->get_screenshot_width_and_height(&width, &height);

	s_camera.init(60.0f * math::DegToRad, width / float(height));
	s_camera.place(XMVectorSet(0.0f, 0.0f, 5.0f, 0.0f));
	s_camera.move_forward(-7.0f);
}

static void do_init()
{
	init_vs_mappings();
	init_ps_mappings();

	s_bvh_manager.init();
}

static void do_shutdown()
{
	s_tlas.free();
	s_output.free();
	s_history.free();

	s_output_uav.free();
	s_output_srv.free();
	s_history_uav.free();

	s_bvh_manager.destroy();

	s_empty_buffer.free();
	s_empty_srv.free();

	s_dynamic_resources.clear();
	s_shadow_resources.clear();

	timing::destroy(s_d3d12device);

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
		reshade::register_event<reshade::addon_event::bind_pipeline_states>(on_bind_pipeline_states);
		reshade::register_event<reshade::addon_event::bind_index_buffer>(on_bind_index_buffer);
		reshade::register_event<reshade::addon_event::bind_vertex_buffers>(on_bind_vertex_buffers);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets_and_depth_stencil);
		reshade::register_event<reshade::addon_event::push_constants>(on_push_constants);
		reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_event<reshade::addon_event::reshade_render_technique_pass>(on_tech_pass_render);
		reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_runtime);
		reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_cmd_list);
		reshade::register_event<reshade::addon_event::execute_command_list>(on_execute_cmd_list);

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

