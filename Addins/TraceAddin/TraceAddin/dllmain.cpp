/*
 * Copyright (C) 2021 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include <imgui/imgui.h>
#include <reshade.hpp>
#include <cassert>
#include <sstream>
#include <shared_mutex>
#include <unordered_set>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <scope_guard/scope_guard.h>

#define XXH_STATIC_LINKING_ONLY   /* access advanced declarations */
#define XXH_IMPLEMENTATION
#include <xxhash/xxhash.h>

using namespace reshade::api;
using namespace Microsoft::WRL;

namespace
{
	bool s_capture_continuous = false;
	bool s_do_capture = s_capture_continuous;
	bool ui_filterDraws = false;
	bool ui_filterDrawIndexes = false;
	bool ui_alwaysTraceBlasBuilds = false;
	int ui_drawCallBegin = 0;
	int ui_drawCallEnd = 4095;
	int s_drawCallCount = 0;
	std::shared_mutex s_mutex;
	std::unordered_set<uint64_t> s_samplers;
	std::unordered_set<uint64_t> s_resources;
	std::unordered_set<uint64_t> s_resource_views;
	std::unordered_set<uint64_t> s_pipelines;

	inline auto to_string(shader_stage value)
	{
		switch (value)
		{
		case shader_stage::vertex:
			return "vertex";
		case shader_stage::hull:
			return "hull";
		case shader_stage::domain:
			return "domain";
		case shader_stage::geometry:
			return "geometry";
		case shader_stage::pixel:
			return "pixel";
		case shader_stage::compute:
			return "compute";
		case shader_stage::all:
			return "all";
		case shader_stage::all_graphics:
			return "all_graphics";
		default:
			return "unknown";
		}
	}
	inline auto to_string(pipeline_subobject_type value)
	{
		switch (value)
		{
		case pipeline_subobject_type::vertex_shader:
			return "vertex";
		case pipeline_subobject_type::hull_shader:
			return "hull";
		case pipeline_subobject_type::domain_shader:
			return "domain";
		case pipeline_subobject_type::geometry_shader:
			return "geometry";
		case pipeline_subobject_type::pixel_shader:
			return "pixel";
		case pipeline_subobject_type::compute_shader:
			return "compute";
		default:
			return "unknown";
		}
	}
	inline auto to_string(pipeline_stage value)
	{
		switch (value)
		{
		case pipeline_stage::vertex_shader:
			return "vertex_shader";
		case pipeline_stage::hull_shader:
			return "hull_shader";
		case pipeline_stage::domain_shader:
			return "domain_shader";
		case pipeline_stage::geometry_shader:
			return "geometry_shader";
		case pipeline_stage::pixel_shader:
			return "pixel_shader";
		case pipeline_stage::compute_shader:
			return "compute_shader";
		case pipeline_stage::input_assembler:
			return "input_assembler";
		case pipeline_stage::stream_output:
			return "stream_output";
		case pipeline_stage::rasterizer:
			return "rasterizer";
		case pipeline_stage::depth_stencil:
			return "depth_stencil";
		case pipeline_stage::output_merger:
			return "output_merger";
		case pipeline_stage::all:
			return "all";
		case pipeline_stage::all_graphics:
			return "all_graphics";
		case pipeline_stage::all_shader_stages:
			return "all_shader_stages";
		default:
			return "unknown";
		}
	}
	inline auto to_string(descriptor_type value)
	{
		switch (value)
		{
		case descriptor_type::sampler:
			return "sampler";
		case descriptor_type::sampler_with_resource_view:
			return "sampler_with_resource_view";
		case descriptor_type::shader_resource_view:
			return "shader_resource_view";
		case descriptor_type::unordered_access_view:
			return "unordered_access_view";
		case descriptor_type::constant_buffer:
			return "constant_buffer";
		default:
			return "unknown";
		}
	}
	inline auto to_string(resource_type type, int count)
	{
		switch (type)
		{
		case resource_type::buffer:
			return "buffer";
		case resource_type::texture_1d:
			return "texture1d";
		case resource_type::texture_2d:
			if (count > 1)
				return "textureCube";
			return "texture2d";
		case resource_type::texture_3d:
			return "texture3d";
		case resource_type::surface:
			return "surface";
		default:
			return "unkown";
		}
	}
	inline auto to_string(dynamic_state value)
	{
		switch (value)
		{
		default:
		case dynamic_state::unknown:
			return "unknown";
		case dynamic_state::alpha_test_enable:
			return "alpha_test_enable";
		case dynamic_state::alpha_reference_value:
			return "alpha_reference_value";
		case dynamic_state::alpha_func:
			return "alpha_func";
		case dynamic_state::srgb_write_enable:
			return "srgb_write_enable";
		case dynamic_state::primitive_topology:
			return "primitive_topology";
		case dynamic_state::sample_mask:
			return "sample_mask";
		case dynamic_state::alpha_to_coverage_enable:
			return "alpha_to_coverage_enable";
		case dynamic_state::blend_enable:
			return "blend_enable";
		case dynamic_state::logic_op_enable:
			return "logic_op_enable";
		case dynamic_state::color_blend_op:
			return "color_blend_op";
		case dynamic_state::source_color_blend_factor:
			return "src_color_blend_factor";
		case dynamic_state::dest_color_blend_factor:
			return "dst_color_blend_factor";
		case dynamic_state::alpha_blend_op:
			return "alpha_blend_op";
		case dynamic_state::source_alpha_blend_factor:
			return "src_alpha_blend_factor";
		case dynamic_state::dest_alpha_blend_factor:
			return "dst_alpha_blend_factor";
		case dynamic_state::logic_op:
			return "logic_op";
		case dynamic_state::blend_constant:
			return "blend_constant";
		case dynamic_state::render_target_write_mask:
			return "render_target_write_mask";
		case dynamic_state::fill_mode:
			return "fill_mode";
		case dynamic_state::cull_mode:
			return "cull_mode";
		case dynamic_state::front_counter_clockwise:
			return "front_counter_clockwise";
		case dynamic_state::depth_bias:
			return "depth_bias";
		case dynamic_state::depth_bias_clamp:
			return "depth_bias_clamp";
		case dynamic_state::depth_bias_slope_scaled:
			return "depth_bias_slope_scaled";
		case dynamic_state::depth_clip_enable:
			return "depth_clip_enable";
		case dynamic_state::scissor_enable:
			return "scissor_enable";
		case dynamic_state::multisample_enable:
			return "multisample_enable";
		case dynamic_state::antialiased_line_enable:
			return "antialiased_line_enable";
		case dynamic_state::depth_enable:
			return "depth_enable";
		case dynamic_state::depth_write_mask:
			return "depth_write_mask";
		case dynamic_state::depth_func:
			return "depth_func";
		case dynamic_state::stencil_enable:
			return "stencil_enable";
		case dynamic_state::stencil_read_mask:
			return "stencil_read_mask";
		case dynamic_state::stencil_write_mask:
			return "stencil_write_mask";
		case dynamic_state::stencil_reference_value:
			return "stencil_reference_value";
		case dynamic_state::front_stencil_func:
			return "front_stencil_func";
		case dynamic_state::front_stencil_pass_op:
			return "front_stencil_pass_op";
		case dynamic_state::front_stencil_fail_op:
			return "front_stencil_fail_op";
		case dynamic_state::front_stencil_depth_fail_op:
			return "front_stencil_depth_fail_op";
		case dynamic_state::back_stencil_func:
			return "back_stencil_func";
		case dynamic_state::back_stencil_pass_op:
			return "back_stencil_pass_op";
		case dynamic_state::back_stencil_fail_op:
			return "back_stencil_fail_op";
		case dynamic_state::back_stencil_depth_fail_op:
			return "back_stencil_depth_fail_op";
		}
	}
	inline auto to_string(resource_usage usage)
	{
		std::stringstream s;
		
		if( (usage & resource_usage::index_buffer) != 0)
			s << "index_buffer | ";
		if( (usage & resource_usage::vertex_buffer) != 0)
			s << "vertex_buffer | ";
		if( (usage & resource_usage::constant_buffer) != 0)
			s << "constant_buffer | ";
		if( (usage & resource_usage::stream_output) != 0)
			s << "stream_output | ";
		if( (usage & resource_usage::indirect_argument) != 0)
			s << "indirect_argument | ";
		if( (usage & (resource_usage::depth_stencil|resource_usage::depth_stencil_read|resource_usage::depth_stencil_write)) != 0)
			s << "depth_stencil | ";
		if( (usage & resource_usage::render_target) != 0)
			s << "render_target | ";
		if( (usage & (resource_usage::shader_resource|resource_usage::shader_resource_pixel|resource_usage::shader_resource_non_pixel)) != 0)
			s << "shader_resource | ";
		if( (usage & resource_usage::unordered_access) != 0)
			s << "unordered_access | ";
		if( (usage & resource_usage::copy_dest) != 0)
			s << "copy_dest | ";
		if( (usage & resource_usage::copy_source) != 0)
			s << "copy_source | ";
		if( (usage & resource_usage::resolve_dest) != 0)
			s << "resolve_dest | ";
		if( (usage & resource_usage::resolve_source) != 0)
			s << "resolve_source | ";
		if( (usage & resource_usage::general) != 0)
			s << "general | ";
		if( (usage & resource_usage::present) != 0)
			s << "present | ";
		if( (usage & resource_usage::cpu_access) != 0)
			s << "cpu_access | ";

		return s.str();
	}
	inline auto to_string(query_type value)
	{
		switch (value)
		{
		case query_type::occlusion:
			return "occlusion";
		case query_type::binary_occlusion:
			return "binary_occlusion";
		case query_type::timestamp:
			return "timestamp";
		case query_type::pipeline_statistics:
			return "pipeline_statistics";
		case query_type::stream_output_statistics_0:
			return "stream_output_statistics_0";
		case query_type::stream_output_statistics_1:
			return "stream_output_statistics_1";
		case query_type::stream_output_statistics_2:
			return "stream_output_statistics_2";
		case query_type::stream_output_statistics_3:
			return "stream_output_statistics_3";
		default:
			return "unknown";
		}
	}
	inline auto to_string(format fmt)
	{
		if (fmt == format::a8_unorm)
			return "a8_unorm";
		if (fmt == format::b10g10r10a2_typeless)
			return "b10g10r10a2_typeless";
		if (fmt == format::b10g10r10a2_uint)
			return "b10g10r10a2_uint";
		if (fmt == format::b10g10r10a2_unorm)
			return "b10g10r10a2_unorm";
		if (fmt == format::b4g4r4a4_unorm)
			return "b4g4r4a4_unorm";
		if (fmt == format::b5g5r5a1_unorm)
			return "b5g5r5a1_unorm";
		if (fmt == format::b5g5r5x1_unorm)
			return "b5g5r5x1_unorm";
		if (fmt == format::b5g6r5_unorm)
			return "b5g6r5_unorm";
		if (fmt == format::b8g8r8a8_typeless)
			return "b8g8r8a8_typeless";
		if (fmt == format::b8g8r8a8_unorm)
			return "b8g8r8a8_unorm";
		if (fmt == format::b8g8r8a8_unorm_srgb)
			return "b8g8r8a8_unorm_srgb";
		if (fmt == format::b8g8r8x8_typeless)
			return "b8g8r8x8_typeless";
		if (fmt == format::b8g8r8x8_unorm)
			return "b8g8r8x8_unorm";
		if (fmt == format::b8g8r8x8_unorm_srgb)
			return "b8g8r8x8_unorm_srgb";
		if (fmt == format::bc1_typeless)
			return "bc1_typeless";
		if (fmt == format::bc1_unorm)
			return "bc1_unorm";
		if (fmt == format::bc1_unorm_srgb)
			return "bc1_unorm_srgb";
		if (fmt == format::bc2_typeless)
			return "bc2_typeless";
		if (fmt == format::bc2_unorm)
			return "bc2_unorm";
		if (fmt == format::bc2_unorm_srgb)
			return "bc2_unorm_srgb";
		if (fmt == format::bc3_typeless)
			return "bc3_typeless";
		if (fmt == format::bc3_unorm)
			return "bc3_unorm";
		if (fmt == format::bc3_unorm_srgb)
			return "bc3_unorm_srgb";
		if (fmt == format::bc4_snorm)
			return "bc4_snorm";
		if (fmt == format::bc4_typeless)
			return "bc4_typeless";
		if (fmt == format::bc4_unorm)
			return "bc4_unorm";
		if (fmt == format::bc5_snorm)
			return "bc5_snorm";
		if (fmt == format::bc5_typeless)
			return "bc5_typeless";
		if (fmt == format::bc5_unorm)
			return "bc5_unorm";
		if (fmt == format::bc6h_sfloat)
			return "bc6h_sfloat";
		if (fmt == format::bc6h_typeless)
			return "bc6h_typeless";
		if (fmt == format::bc6h_ufloat)
			return "bc6h_ufloat";
		if (fmt == format::bc7_typeless)
			return "bc7_typeless";
		if (fmt == format::bc7_unorm)
			return "bc7_unorm";
		if (fmt == format::bc7_unorm_srgb)
			return "bc7_unorm_srgb";
		if (fmt == format::d16_unorm)
			return "d16_unorm";
		if (fmt == format::d16_unorm_s8_uint)
			return "d16_unorm_s8_uint";
		if (fmt == format::d24_unorm_s8_uint)
			return "d24_unorm_s8_uint";
		if (fmt == format::d24_unorm_x8_uint)
			return "d24_unorm_x8_uint";
		if (fmt == format::d32_float)
			return "d32_float";
		if (fmt == format::d32_float_s8_uint)
			return "d32_float_s8_uint";
		if (fmt == format::g8r8_g8b8_unorm)
			return "g8r8_g8b8_unorm";
		if (fmt == format::intz)
			return "intz";
		if (fmt == format::l16_unorm)
			return "l16_unorm";
		if (fmt == format::l16a16_unorm)
			return "l16a16_unorm";
		if (fmt == format::l8_unorm)
			return "l8_unorm";
		if (fmt == format::l8a8_unorm)
			return "l8a8_unorm";
		if (fmt == format::r10g10b10a2_typeless)
			return "r10g10b10a2_typeless";
		if (fmt == format::r10g10b10a2_uint)
			return "r10g10b10a2_uint";
		if (fmt == format::r10g10b10a2_unorm)
			return "r10g10b10a2_unorm";
		if (fmt == format::r10g10b10a2_xr_bias)
			return "r10g10b10a2_xr_bias";
		if (fmt == format::r11g11b10_float)
			return "r11g11b10_float";
		if (fmt == format::r16_float)
			return "r16_float";
		if (fmt == format::r16_sint)
			return "r16_sint";
		if (fmt == format::r16_snorm)
			return "r16_snorm";
		if (fmt == format::r16_typeless)
			return "r16_typeless";
		if (fmt == format::r16_uint)
			return "r16_uint";
		if (fmt == format::r16_unorm)
			return "r16_unorm";
		if (fmt == format::r16g16_float)
			return "r16g16_float";
		if (fmt == format::r16g16_sint)
			return "r16g16_sint";
		if (fmt == format::r16g16_snorm)
			return "r16g16_snorm";
		if (fmt == format::r16g16_typeless)
			return "r16g16_typeless";
		if (fmt == format::r16g16_uint)
			return "r16g16_uint";
		if (fmt == format::r16g16_unorm)
			return "r16g16_unorm";
		if (fmt == format::r16g16b16a16_float)
			return "r16g16b16a16_float";
		if (fmt == format::r16g16b16a16_sint)
			return "r16g16b16a16_sint";
		if (fmt == format::r16g16b16a16_snorm)
			return "r16g16b16a16_snorm";
		if (fmt == format::r16g16b16a16_typeless)
			return "r16g16b16a16_typeless";
		if (fmt == format::r16g16b16a16_uint)
			return "r16g16b16a16_uint";
		if (fmt == format::r16g16b16a16_unorm)
			return "r16g16b16a16_unorm";
		if (fmt == format::r1_unorm)
			return "r1_unorm";
		if (fmt == format::r24_g8_typeless)
			return "r24_g8_typeless";
		if (fmt == format::r24_unorm_x8_uint)
			return "r24_unorm_x8_uint";
		if (fmt == format::r32_float)
			return "r32_float";
		if (fmt == format::r32_float_x8_uint)
			return "r32_float_x8_uint";
		if (fmt == format::r32_g8_typeless)
			return "r32_g8_typeless";
		if (fmt == format::r32_sint)
			return "r32_sint";
		if (fmt == format::r32_typeless)
			return "r32_typeless";
		if (fmt == format::r32_uint)
			return "r32_uint";
		if (fmt == format::r32g32_float)
			return "r32g32_float";
		if (fmt == format::r32g32_sint)
			return "r32g32_sint";
		if (fmt == format::r32g32_typeless)
			return "r32g32_typeless";
		if (fmt == format::r32g32_uint)
			return "r32g32_uint";
		if (fmt == format::r32g32b32_float)
			return "r32g32b32_float";
		if (fmt == format::r32g32b32_sint)
			return "r32g32b32_sint";
		if (fmt == format::r32g32b32_typeless)
			return "r32g32b32_typeless";
		if (fmt == format::r32g32b32_uint)
			return "r32g32b32_uint";
		if (fmt == format::r32g32b32a32_float)
			return "r32g32b32a32_float";
		if (fmt == format::r32g32b32a32_sint)
			return "r32g32b32a32_sint";
		if (fmt == format::r32g32b32a32_typeless)
			return "r32g32b32a32_typeless";
		if (fmt == format::r32g32b32a32_uint)
			return "r32g32b32a32_uint";
		if (fmt == format::r8_sint)
			return "r8_sint";
		if (fmt == format::r8_snorm)
			return "r8_snorm";
		if (fmt == format::r8_typeless)
			return "r8_typeless";
		if (fmt == format::r8_uint)
			return "r8_uint";
		if (fmt == format::r8_unorm)
			return "r8_unorm";
		if (fmt == format::r8g8_b8g8_unorm)
			return "r8g8_b8g8_unorm";
		if (fmt == format::r8g8_sint)
			return "r8g8_sint";
		if (fmt == format::r8g8_snorm)
			return "r8g8_snorm";
		if (fmt == format::r8g8_typeless)
			return "r8g8_typeless";
		if (fmt == format::r8g8_uint)
			return "r8g8_uint";
		if (fmt == format::r8g8_unorm)
			return "r8g8_unorm";
		if (fmt == format::r8g8b8a8_sint)
			return "r8g8b8a8_sint";
		if (fmt == format::r8g8b8a8_snorm)
			return "r8g8b8a8_snorm";
		if (fmt == format::r8g8b8a8_typeless)
			return "r8g8b8a8_typeless";
		if (fmt == format::r8g8b8a8_uint)
			return "r8g8b8a8_uint";
		if (fmt == format::r8g8b8a8_unorm)
			return "r8g8b8a8_unorm";
		if (fmt == format::r8g8b8a8_unorm_srgb)
			return "r8g8b8a8_unorm_srgb";
		if (fmt == format::r8g8b8x8_typeless)
			return "r8g8b8x8_typeless";
		if (fmt == format::r8g8b8x8_unorm)
			return "r8g8b8x8_unorm";
		if (fmt == format::r8g8b8x8_unorm_srgb)
			return "r8g8b8x8_unorm_srgb";
		if (fmt == format::r9g9b9e5)
			return "r9g9b9e5";
		if (fmt == format::s8_uint)
			return "s8_uint";
		if (fmt == format::x24_unorm_g8_uint)
			return "x24_unorm_g8_uint";
		if (fmt == format::x32_float_g8_uint)
			return "x32_float_g8_uint";

		return "unkown";
	}
}

static bool do_capture(bool limit_to_range=true)
{
	int drawId = s_drawCallCount;
	return s_do_capture && (!limit_to_range || (drawId >= (ui_drawCallBegin) && drawId <= ui_drawCallEnd));
}

static void on_init_swapchain(swapchain *swapchain)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	const device_api api = swapchain->get_device()->get_api();

	for (uint32_t i = 0; i < swapchain->get_back_buffer_count(); ++i)
	{
		const resource buffer = swapchain->get_back_buffer(i);

		s_resources.emplace(buffer.handle);
		if (api == device_api::d3d9 || api == device_api::opengl)
			s_resource_views.emplace(buffer.handle);

		std::stringstream s;
		s << "create_render_target_view(" << (void *)buffer.handle << ")";

		reshade::log_message(3, s.str().c_str());
	}
}
static void on_destroy_swapchain(swapchain *swapchain)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	const device_api api = swapchain->get_device()->get_api();

	for (uint32_t i = 0; i < swapchain->get_back_buffer_count(); ++i)
	{
		const resource buffer = swapchain->get_back_buffer(i);

		s_resources.erase(buffer.handle);
		if (api == device_api::d3d9 || api == device_api::opengl)
			s_resource_views.erase(buffer.handle);
	}
}
static void on_init_sampler(device *device, const sampler_desc &desc, sampler handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	s_samplers.emplace(handle.handle);
}
static void on_destroy_sampler(device *device, sampler handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	assert(s_samplers.find(handle.handle) != s_samplers.end());
	s_samplers.erase(handle.handle);
}
static bool on_create_resource(device *device, resource_desc& desc, subresource_data *initial_data, resource_usage initial_state)
{
	if (!do_capture())
		return false;

	std::stringstream s;
	s << "on_create_resource: type: " << to_string(desc.type, desc.texture.depth_or_layers) << ", usage: " << to_string(desc.usage);
	if (desc.type == resource_type::texture_2d)
	{
		s << ", format: " << to_string(desc.texture.format);
	}
	reshade::log_message(3, s.str().c_str());

	return false;
}
static void on_init_resource(device *device, const resource_desc &desc, const subresource_data *, resource_usage usage_type, resource handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	if (do_capture())
	{
		std::stringstream s;
		s << "init_resource: " << (void *)handle.handle << ", type: " << to_string(desc.type, desc.texture.depth_or_layers) << ", usage : " << to_string(desc.usage);
		if (desc.type == resource_type::texture_2d)
		{
			s << ", format: " << to_string(desc.texture.format);
		}
		reshade::log_message(3, s.str().c_str());
	}

	if (usage_type == resource_usage::render_target)
	{
		std::stringstream s;
		s << "create_render_target(" << (void *)handle.handle << ")";

		reshade::log_message(3, s.str().c_str());
	}
	else if (usage_type == resource_usage::depth_stencil)
	{
		std::stringstream s;
		s << "create_depth_stencil(" << (void *)handle.handle << ")";

		reshade::log_message(3, s.str().c_str());
	}

	s_resources.emplace(handle.handle);
}
static void on_destroy_resource(device *device, resource handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	assert(s_resources.find(handle.handle) != s_resources.end());
	s_resources.erase(handle.handle);
}
static void on_init_resource_view(device *device, resource resource, resource_usage usage_type, const resource_view_desc &desc, resource_view handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	assert(resource == 0 || s_resources.find(resource.handle) != s_resources.end());
	s_resource_views.emplace(handle.handle);

	if (usage_type == resource_usage::render_target)
	{
		std::stringstream s;
		s << "create_render_target_view( texture(" << (void *)resource.handle << "), view(" << (void *)handle.handle << "))";

		reshade::log_message(3, s.str().c_str());
	}
	else if (usage_type == resource_usage::depth_stencil)
	{
		std::stringstream s;
		s << "create_depth_stencil_view( texture(" << (void*)resource.handle << "), view(" << (void*)handle.handle << "))";

		reshade::log_message(3, s.str().c_str());
	}
}
static void on_destroy_resource_view(device *device, resource_view handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	assert(s_resource_views.find(handle.handle) != s_resource_views.end());
	s_resource_views.erase(handle.handle);
}
static void on_init_pipeline(device *device, pipeline_layout, uint32_t subObjectCount, const pipeline_subobject* subObjects, pipeline handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	s_pipelines.emplace(handle.handle);

	for (uint32_t i = 0; i < subObjectCount; i++)
	{
		const pipeline_subobject &object = subObjects[i];

		if (object.type == pipeline_subobject_type::vertex_shader ||
			object.type == pipeline_subobject_type::pixel_shader)
		{
			shader_desc *shader_data = (shader_desc*)object.data;
			ComPtr<ID3DBlob> blob;
			HRESULT result = D3DDisassemble(shader_data->code, shader_data->code_size, 0, 0, blob.GetAddressOf());

			if (SUCCEEDED(result))
			{
				char *str = (char *)blob->GetBufferPointer();

				XXH64_hash_t hash = XXH3_64bits(shader_data->code, shader_data->code_size);

				std::stringstream s;
				s << "init_pipeline(" << to_string(object.type) << ", " << (void *)handle.handle << ", hash: " << hash << "):\n" << str;

				reshade::log_message(3, s.str().c_str());
			}
		}
	}
}
static void on_destroy_pipeline(device *device, pipeline handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	assert(s_pipelines.find(handle.handle) != s_pipelines.end());
	s_pipelines.erase(handle.handle);
}

static void on_barrier(command_list *, uint32_t num_resources, const resource *resources, const resource_usage *old_states, const resource_usage *new_states)
{
	if (!do_capture())
		return;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	for (uint32_t i = 0; i < num_resources; ++i)
		assert(resources[i] == 0 || s_resources.find(resources[i].handle) != s_resources.end());
	}
#endif

	std::stringstream s;
	for (uint32_t i = 0; i < num_resources; ++i)
		s << "barrier(" << (void *)resources[i].handle << ", " << to_string(old_states[i]) << ", " << to_string(new_states[i]) << ")" << std::endl;

	reshade::log_message(3, s.str().c_str());
}

static void on_begin_render_pass(command_list *, uint32_t count, const render_pass_render_target_desc *rts, const render_pass_depth_stencil_desc *ds)
{
	if (!do_capture())
		return;

	std::stringstream s;
	s << "begin_render_pass(" << count << ", { ";
	for (uint32_t i = 0; i < count; ++i)
		s << (void *)rts[i].view.handle << ", ";
	s << " }, " << (ds != nullptr ? (void *)ds->view.handle : 0) << ")";

	reshade::log_message(3, s.str().c_str());
}
static void on_end_render_pass(command_list *)
{
	if (!do_capture())
		return;

	std::stringstream s;
	s << "end_render_pass()";

	reshade::log_message(3, s.str().c_str());
}
static void on_bind_render_targets_and_depth_stencil(command_list *, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
	if (!do_capture())
		return;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	for (uint32_t i = 0; i < count; ++i)
		assert(rtvs[i] == 0 || s_resource_views.find(rtvs[i].handle) != s_resource_views.end());
	assert(dsv == 0 || s_resource_views.find(dsv.handle) != s_resource_views.end());
	}
#endif

	std::stringstream s;
	s << "bind_render_targets_and_depth_stencil(" << count << ", { ";
	for (uint32_t i = 0; i < count; ++i)
		s << (void *)rtvs[i].handle << ", ";
	s << " }, " << (void *)dsv.handle << ")";

	reshade::log_message(3, s.str().c_str());
}

int s_count = 0;
static void on_bind_pipeline(command_list *, pipeline_stage type, pipeline pipeline)
{
	if (!do_capture())
		return;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	assert(pipeline.handle == 0 || s_pipelines.find(pipeline.handle) != s_pipelines.end());
	}
#endif

	std::stringstream s;
	s << "bind_pipeline(" << to_string(type) << ", " << (void *)pipeline.handle << ")" << ", count: " << s_count;
	s_count++;

	reshade::log_message(3, s.str().c_str());
}
static void on_bind_pipeline_states(command_list *, uint32_t count, const dynamic_state *states, const uint32_t *values)
{
	if (!do_capture())
		return;

	std::stringstream s;
	for (uint32_t i = 0; i < count; ++i)
		s << "bind_pipeline_state(" << to_string(states[i]) << ", " << values[i] << ")" << std::endl;

	reshade::log_message(3, s.str().c_str());
}
static void on_bind_viewports(command_list *, uint32_t first, uint32_t count, const viewport *viewports)
{
	if (!do_capture())
		return;

	std::stringstream s;
	s << "bind_viewports(" << first << ", " << count << ", { ... })";

	reshade::log_message(3, s.str().c_str());
}
static void on_bind_scissor_rects(command_list *, uint32_t first, uint32_t count, const rect *rects)
{
	if (!do_capture())
		return;

	std::stringstream s;
	s << "bind_scissor_rects(" << first << ", " << count << ", { ... })";

	reshade::log_message(3, s.str().c_str());
}
static void on_push_constants(command_list *, shader_stage stages, pipeline_layout layout, uint32_t param_index, uint32_t first, uint32_t count, const uint32_t *values)
{
	if (!do_capture())
		return;

	std::stringstream s;
	if (stages == shader_stage::vertex)
	{
		float *floats = (float *)values;
		s << "push_constants(" << to_string(stages) << ", " << (void *)layout.handle << ", " << param_index << ", " << first << ", " << count << ", { ";
		s << "\n";
		for (uint32_t i = 0; i < count * 4; i += 4)
		{
			s << "\t{ ";
			s << floats[i + 0] << ", ";
			s << floats[i + 1] << ", ";
			s << floats[i + 2] << ", ";
			s << floats[i + 3] << " },\n";
		}	
		s << " })";
	}
	else
	{
		s << "push_constants(" << to_string(stages) << ", " << (void *)layout.handle << ", " << param_index << ", " << first << ", " << count << ", { ";
		for (uint32_t i = 0; i < count * 4; ++i)
			s << std::hex << values[i] << std::dec << ", ";
		s << " })";
	}
	

	reshade::log_message(3, s.str().c_str());
}
static void on_push_descriptors(command_list * cmd_list, shader_stage stages, pipeline_layout layout, uint32_t param_index, const descriptor_set_update &update)
{
	if (!do_capture())
		return;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	resource res;
	resource_desc desc;

	switch (update.type)
	{
	case descriptor_type::sampler:
		for (uint32_t i = 0; i < update.count; ++i)
			assert(static_cast<const sampler *>(update.descriptors)[i].handle == 0 || s_samplers.find(static_cast<const sampler *>(update.descriptors)[i].handle) != s_samplers.end());
		break;
	case descriptor_type::sampler_with_resource_view:
		for (uint32_t i = 0; i < update.count; ++i)
		{
			assert(static_cast<const sampler_with_resource_view *>(update.descriptors)[i].view.handle == 0 || s_resource_views.find(static_cast<const sampler_with_resource_view *>(update.descriptors)[i].view.handle) != s_resource_views.end());

			sampler_with_resource_view *sv = (sampler_with_resource_view *)update.descriptors;

			resource_view view = sv[i].view;
			if (view.handle != 0)
			{
				res = cmd_list->get_device()->get_resource_from_view(view);
				desc = cmd_list->get_device()->get_resource_desc(res);
			}
		}			
		break;
	case descriptor_type::shader_resource_view:
	case descriptor_type::unordered_access_view:
		for (uint32_t i = 0; i < update.count; ++i)
			assert(static_cast<const resource_view *>(update.descriptors)[i].handle == 0 || s_resource_views.find(static_cast<const resource_view *>(update.descriptors)[i].handle) != s_resource_views.end());
		break;
	case descriptor_type::constant_buffer:
		for (uint32_t i = 0; i < update.count; ++i)
			assert(static_cast<const buffer_range *>(update.descriptors)[i].buffer.handle == 0 || s_resources.find(static_cast<const buffer_range *>(update.descriptors)[i].buffer.handle) != s_resources.end());
		break;
	default:
		break;
	}
	}
#endif

	std::stringstream s;
	s << "push_descriptors(" << to_string(stages) << ", " << (void *)layout.handle << ", " << param_index << ", { " << to_string(update.type) << ", " << update.binding << ", " << update.count << " })";

	reshade::log_message(3, s.str().c_str());
}
static void on_bind_descriptor_sets(command_list *, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count, const descriptor_set *sets)
{
	if (!do_capture())
		return;

	std::stringstream s;
	for (uint32_t i = 0; i < count; ++i)
		s << "bind_descriptor_set(" << to_string(stages) << ", " << (void *)layout.handle << ", " << (first + i) << ", " << (void *)sets[i].handle << ")" << std::endl;

	reshade::log_message(3, s.str().c_str());
}
static void on_bind_index_buffer(command_list *, resource buffer, uint64_t offset, uint32_t index_size)
{
	if (!do_capture())
		return;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	assert(buffer.handle == 0 || s_resources.find(buffer.handle) != s_resources.end());
	}
#endif

	std::stringstream s;
	s << "bind_index_buffer(" << (void *)buffer.handle << ", " << offset << ", " << index_size << ")";

	reshade::log_message(3, s.str().c_str());
}
static void on_bind_vertex_buffers(command_list *, uint32_t first, uint32_t count, const resource *buffers, const uint64_t *offsets, const uint32_t *strides)
{
	if (!do_capture())
		return;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	for (uint32_t i = 0; i < count; ++i)
		assert(buffers[i].handle == 0 || s_resources.find(buffers[i].handle) != s_resources.end());
	}
#endif

	std::stringstream s;
	for (uint32_t i = 0; i < count; ++i)
		s << "bind_vertex_buffer(" << (first + i) << ", " << (void *)buffers[i].handle << ", " << (offsets != nullptr ? offsets[i] : 0) << ", " << (strides != nullptr ? strides[i] : 0) << ")" << std::endl;

	reshade::log_message(3, s.str().c_str());
}

static bool on_draw(command_list *, uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance)
{
	auto on_exit = sg::make_scope_guard([&]() {
		s_drawCallCount++;
	});

	bool filter = !(s_drawCallCount >= ui_drawCallBegin && s_drawCallCount <= ui_drawCallEnd);

	if (filter)
		return true;

	if (!do_capture())
		return ui_filterDraws;

	std::stringstream s;
	s << "draw " << s_drawCallCount << " ("<< vertices << ", " << instances << ", " << first_vertex << ", " << first_instance << ")";

	reshade::log_message(3, s.str().c_str());

	return ui_filterDraws;
}
static bool on_draw_indexed(command_list *, uint32_t indices, uint32_t instances, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
{
	bool filter = !(s_drawCallCount >= ui_drawCallBegin && s_drawCallCount <= ui_drawCallEnd);
	int drawId = s_drawCallCount;

	auto on_exit = sg::make_scope_guard([&]() {
		s_drawCallCount++;
	});

	if (filter)
		return true;

	if (!do_capture())
		return filter;

	std::stringstream s;
	s << "draw_indexed " << drawId << " (" << indices << ", " << instances << ", " << first_index << ", " << vertex_offset << ", " << first_instance << ")";

	reshade::log_message(3, s.str().c_str());

	return filter;
}
static bool on_dispatch(command_list *, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
{
	if (!do_capture())
		return false;

	std::stringstream s;
	s << "dispatch(" << group_count_x << ", " << group_count_y << ", " << group_count_z << ")";

	reshade::log_message(3, s.str().c_str());

	return false;
}
static bool on_draw_or_dispatch_indirect(command_list *, indirect_command type, resource buffer, uint64_t offset, uint32_t draw_count, uint32_t stride)
{
	if (!do_capture())
		return false;

	std::stringstream s;
	switch (type)
	{
	case indirect_command::unknown:
		s << "draw_or_dispatch_indirect(" << (void *)buffer.handle << ", " << offset << ", " << draw_count << ", " << stride << ")";
		break;
	case indirect_command::draw:
		s << "draw_indirect(" << (void *)buffer.handle << ", " << offset << ", " << draw_count << ", " << stride << ")";
		break;
	case indirect_command::draw_indexed:
		s << "draw_indexed_indirect(" << (void *)buffer.handle << ", " << offset << ", " << draw_count << ", " << stride << ")";
		break;
	case indirect_command::dispatch:
		s << "dispatch_indirect(" << (void *)buffer.handle << ", " << offset << ", " << draw_count << ", " << stride << ")";
		break;
	}

	reshade::log_message(3, s.str().c_str());

	return false;
}

static bool on_copy_resource(command_list *, resource src, resource dst)
{
	if (!do_capture())
		return false;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	assert(s_resources.find(src.handle) != s_resources.end());
	assert(s_resources.find(dst.handle) != s_resources.end());
	}
#endif

	std::stringstream s;
	s << "copy_resource(" << (void *)src.handle << ", " << (void *)dst.handle << ")";

	reshade::log_message(3, s.str().c_str());

	return false;
}
static bool on_copy_buffer_region(command_list *, resource src, uint64_t src_offset, resource dst, uint64_t dst_offset, uint64_t size)
{
	if (!do_capture())
		return false;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	assert(s_resources.find(src.handle) != s_resources.end());
	assert(s_resources.find(dst.handle) != s_resources.end());
	}
#endif

	std::stringstream s;
	s << "copy_buffer_region(" << (void *)src.handle << ", " << src_offset << ", " << (void *)dst.handle << ", " << dst_offset << ", " << size << ")";

	reshade::log_message(3, s.str().c_str());

	return false;
}
static bool on_copy_buffer_to_texture(command_list *, resource src, uint64_t src_offset, uint32_t row_length, uint32_t slice_height, resource dst, uint32_t dst_subresource, const subresource_box *)
{
	if (!do_capture())
		return false;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	assert(s_resources.find(src.handle) != s_resources.end());
	assert(s_resources.find(dst.handle) != s_resources.end());
	}
#endif

	std::stringstream s;
	s << "copy_buffer_to_texture(" << (void *)src.handle << ", " << src_offset << ", " << row_length << ", " << slice_height << ", " << (void *)dst.handle << ", " << dst_subresource << ")";

	reshade::log_message(3, s.str().c_str());

	return false;
}
static bool on_copy_texture_region(command_list *, resource src, uint32_t src_subresource, const subresource_box *, resource dst, uint32_t dst_subresource, const subresource_box *, filter_mode filter)
{
	if (!do_capture())
		return false;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	assert(s_resources.find(src.handle) != s_resources.end());
	assert(s_resources.find(dst.handle) != s_resources.end());
	}
#endif

	std::stringstream s;
	s << "copy_texture_region(" << (void *)src.handle << ", " << src_subresource << ", " << (void *)dst.handle << ", " << dst_subresource << ", " << (uint32_t)filter << ")";

	reshade::log_message(3, s.str().c_str());

	return false;
}
static bool on_copy_texture_to_buffer(command_list *, resource src, uint32_t src_subresource, const subresource_box *, resource dst, uint64_t dst_offset, uint32_t row_length, uint32_t slice_height)
{
	if (!do_capture())
		return false;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	assert(s_resources.find(src.handle) != s_resources.end());
	assert(s_resources.find(dst.handle) != s_resources.end());
	}
#endif

	std::stringstream s;
	s << "copy_texture_to_buffer(" << (void *)src.handle << ", " << src_subresource << ", " << (void *)dst.handle << ", " << dst_offset << ", " << row_length << ", " << slice_height << ")";

	reshade::log_message(3, s.str().c_str());

	return false;
}
static bool on_resolve_texture_region(command_list *, resource src, uint32_t src_subresource, const subresource_box *, resource dst, uint32_t dst_subresource, int32_t dst_x, int32_t dst_y, int32_t dst_z, format format)
{
	if (!do_capture())
		return false;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	assert(s_resources.find(src.handle) != s_resources.end());
	assert(s_resources.find(dst.handle) != s_resources.end());
	}
#endif

	std::stringstream s;
	s << "resolve_texture_region(" << (void *)src.handle << ", " << src_subresource << ", { ... }, " << (void *)dst.handle << ", " << dst_subresource << ", " << dst_x << ", " << dst_y << ", " << dst_z << ", " << (uint32_t)format << ")";

	reshade::log_message(3, s.str().c_str());

	return false;
}
static void on_map_texture_region(device *device, resource resource, uint32_t subresource, const subresource_box *box, map_access access, subresource_data *data)
{
	if (!do_capture())
		return;

	std::stringstream s;
	s << "map_texture_region(" << (void *)resource.handle << ", " << subresource << ")";

	reshade::log_message(3, s.str().c_str());
}

static bool on_clear_depth_stencil_view(command_list *, resource_view dsv, const float *depth, const uint8_t *stencil, uint32_t, const rect *)
{
	if (!do_capture())
		return false;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	assert(s_resource_views.find(dsv.handle) != s_resource_views.end());
	}
#endif

	std::stringstream s;
	s << "clear_depth_stencil_view(" << (void *)dsv.handle << ", " << (depth != nullptr ? *depth : 0.0f) << ", " << (stencil != nullptr ? *stencil : 0) << ")";

	reshade::log_message(3, s.str().c_str());

	return false;
}
static bool on_clear_render_target_view(command_list *, resource_view rtv, const float color[4], uint32_t, const rect *)
{
	if (!do_capture())
		return false;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	assert(s_resource_views.find(rtv.handle) != s_resource_views.end());
	}
#endif

	std::stringstream s;
	s << "clear_render_target_view(" << (void *)rtv.handle << ", { " << color[0] << ", " << color[1] << ", " << color[2] << ", " << color[3] << " })";

	reshade::log_message(3, s.str().c_str());

	return false;
}
static bool on_clear_unordered_access_view_uint(command_list *, resource_view uav, const uint32_t values[4], uint32_t, const rect *)
{
	if (!do_capture())
		return false;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

assert(s_resource_views.find(uav.handle) != s_resource_views.end());
	}
#endif

	std::stringstream s;
	s << "clear_unordered_access_view_uint(" << (void *)uav.handle << ", { " << values[0] << ", " << values[1] << ", " << values[2] << ", " << values[3] << " })";

	reshade::log_message(3, s.str().c_str());

	return false;
}
static bool on_clear_unordered_access_view_float(command_list *, resource_view uav, const float values[4], uint32_t, const rect *)
{
	if (!do_capture())
		return false;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	assert(s_resource_views.find(uav.handle) != s_resource_views.end());
	}
#endif

	std::stringstream s;
	s << "clear_unordered_access_view_float(" << (void *)uav.handle << ", { " << values[0] << ", " << values[1] << ", " << values[2] << ", " << values[3] << " })";

	reshade::log_message(3, s.str().c_str());

	return false;
}

static bool on_generate_mipmaps(command_list *, resource_view srv)
{
	if (!do_capture())
		return false;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	assert(s_resource_views.find(srv.handle) != s_resource_views.end());
	}
#endif

	std::stringstream s;
	s << "generate_mipmaps(" << (void *)srv.handle << ")";

	reshade::log_message(3, s.str().c_str());

	return false;
}

static bool on_begin_query(command_list *cmd_list, query_pool pool, query_type type, uint32_t index)
{
	if (!do_capture())
		return false;

	std::stringstream s;
	s << "begin_query(" << (void *)pool.handle << ", " << to_string(type) << ", " << index << ")";

	reshade::log_message(3, s.str().c_str());

	return false;
}
static bool on_end_query(command_list *cmd_list, query_pool pool, query_type type, uint32_t index)
{
	if (!do_capture())
		return false;

	std::stringstream s;
	s << "end_query(" << (void *)pool.handle << ", " << to_string(type) << ", " << index << ")";

	reshade::log_message(3, s.str().c_str());

	return false;
}
static bool on_copy_query_pool_results(command_list *cmd_list, query_pool pool, query_type type, uint32_t first, uint32_t count, resource dest, uint64_t dest_offset, uint32_t stride)
{
	if (!do_capture())
		return false;

#ifndef NDEBUG
	{	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	assert(s_resources.find(dest.handle) != s_resources.end());
	}
#endif

	std::stringstream s;
	s << "copy_query_pool_results(" << (void *)pool.handle << ", " << to_string(type) << ", " << first << ", " << count << (void *)dest.handle << ", " << dest_offset << ", " << stride << ")";

	reshade::log_message(3, s.str().c_str());

	return false;
}

static void on_build_acceleration_structure(command_list *cmd_list, const rt_build_acceleration_structure_desc &desc, const buffer_range &buffer)
{
	if (!do_capture())
	{
		if (!(ui_alwaysTraceBlasBuilds && desc.inputs.type == rt_acceleration_structure_type::bottom_level))
		{
			return;
		}
	}

	std::stringstream s;
	if (desc.inputs.type == rt_acceleration_structure_type::top_level)
	{
		s << "build_acceleration_structure(" << (void *)buffer.buffer.handle << ", offset: " << buffer.offset << ", size: " << buffer.size << ", instances: " << desc.inputs.desc_count << ")";
	}
	else
	{
		s << "build_acceleration_structure(" << (void *)buffer.buffer.handle << ", offset: " << buffer.offset << ", size: " << buffer.size << ")";
	}
	

	reshade::log_message(3, s.str().c_str());
}

static void on_present(effect_runtime *runtime)
{
	if (s_do_capture)
	{
		reshade::log_message(3, "present()");
		reshade::log_message(3, "--- End Frame ---");
		if(!s_capture_continuous)
			s_do_capture = false;
	}
	else
	{
		// The keyboard shortcut to trigger logging
		if (runtime->is_key_pressed(VK_F10))
		{
			s_do_capture = true;
		}
		if (s_do_capture)
		{
			reshade::log_message(3, "--- Frame ---");
		}
	}

	s_drawCallCount = 0;
}

static void draw_ui(reshade::api::effect_runtime *)
{
	ImGui::Checkbox("FilterDraws", &ui_filterDraws);
	ImGui::Checkbox("AlwaysTraceBlasBuilds", &ui_alwaysTraceBlasBuilds);
	ImGui::Value("DrawIndexCount: ", s_drawCallCount);
	ImGui::SliderInt("DrawCallBegin: ", &ui_drawCallBegin, 0, s_drawCallCount);
	ImGui::SliderInt("DrawCallEnd: ", &ui_drawCallEnd, 0, s_drawCallCount);
}

extern "C" __declspec(dllexport) const char *NAME = "API Trace";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Example add-on that logs the graphics API calls done by the application of the next frame after pressing a keyboard shortcut.";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;

		reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
		reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
		reshade::register_event<reshade::addon_event::init_sampler>(on_init_sampler);
		reshade::register_event<reshade::addon_event::destroy_sampler>(on_destroy_sampler);
		reshade::register_event<reshade::addon_event::create_resource>(on_create_resource);
		reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
		reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
		reshade::register_event<reshade::addon_event::init_resource_view>(on_init_resource_view);
		reshade::register_event<reshade::addon_event::destroy_resource_view>(on_destroy_resource_view);
		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);

		reshade::register_event<reshade::addon_event::barrier>(on_barrier);
		reshade::register_event<reshade::addon_event::begin_render_pass>(on_begin_render_pass);
		reshade::register_event<reshade::addon_event::end_render_pass>(on_end_render_pass);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets_and_depth_stencil);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::register_event<reshade::addon_event::bind_pipeline_states>(on_bind_pipeline_states);
		reshade::register_event<reshade::addon_event::bind_viewports>(on_bind_viewports);
		reshade::register_event<reshade::addon_event::bind_scissor_rects>(on_bind_scissor_rects);
		reshade::register_event<reshade::addon_event::push_constants>(on_push_constants);
		reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
		reshade::register_event<reshade::addon_event::bind_descriptor_sets>(on_bind_descriptor_sets);
		reshade::register_event<reshade::addon_event::bind_index_buffer>(on_bind_index_buffer);
		reshade::register_event<reshade::addon_event::bind_vertex_buffers>(on_bind_vertex_buffers);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
		reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_or_dispatch_indirect);
		reshade::register_event<reshade::addon_event::copy_resource>(on_copy_resource);
		reshade::register_event<reshade::addon_event::copy_buffer_region>(on_copy_buffer_region);
		reshade::register_event<reshade::addon_event::copy_buffer_to_texture>(on_copy_buffer_to_texture);
		reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
		reshade::register_event<reshade::addon_event::copy_texture_to_buffer>(on_copy_texture_to_buffer);
		reshade::register_event<reshade::addon_event::map_texture_region>(on_map_texture_region);
		reshade::register_event<reshade::addon_event::resolve_texture_region>(on_resolve_texture_region);
		reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_depth_stencil_view);
		reshade::register_event<reshade::addon_event::clear_render_target_view>(on_clear_render_target_view);
		reshade::register_event<reshade::addon_event::clear_unordered_access_view_uint>(on_clear_unordered_access_view_uint);
		reshade::register_event<reshade::addon_event::clear_unordered_access_view_float>(on_clear_unordered_access_view_float);
		reshade::register_event<reshade::addon_event::generate_mipmaps>(on_generate_mipmaps);
		reshade::register_event<reshade::addon_event::begin_query>(on_begin_query);
		reshade::register_event<reshade::addon_event::end_query>(on_end_query);
		reshade::register_event<reshade::addon_event::copy_query_pool_results>(on_copy_query_pool_results);
		reshade::register_event<reshade::addon_event::build_acceleration_structure>(on_build_acceleration_structure);

		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_overlay(nullptr, draw_ui);
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(hModule);
		break;
	}

	return TRUE;
}
