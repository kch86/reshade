/*
 * Copyright (C) 2021 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "dll_log.hpp"
#include "ini_file.hpp"
#include "opengl_impl_device.hpp"
#include "opengl_impl_render_context.hpp"
#include "opengl_impl_type_convert.hpp"

#define gl gl3wProcs.gl

reshade::opengl::device_impl::device_impl(HDC initial_hdc, HGLRC shared_hglrc, bool compatibility_context) :
	api_object_impl(shared_hglrc), _compatibility_context(compatibility_context)
{
	RECT window_rect = {};
	GetClientRect(WindowFromDC(initial_hdc), &window_rect);

	_default_fbo_width = window_rect.right - window_rect.left;
	_default_fbo_height = window_rect.bottom - window_rect.top;

	// The pixel format has to be the same for all device contexts used with this rendering context, so can cache information about it here
	// See https://docs.microsoft.com/windows/win32/api/wingdi/nf-wingdi-wglmakecurrent
	_pixel_format = GetPixelFormat(initial_hdc);

	PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd) };
	DescribePixelFormat(initial_hdc, _pixel_format, sizeof(pfd), &pfd);

	switch (pfd.cRedBits)
	{
	default:
	case  0: _default_color_format = GL_NONE;
		break;
	case  8: _default_color_format = GL_RGBA8;
		break;
	case 10: _default_color_format = GL_RGB10_A2;
		break;
	case 16: _default_color_format = GL_RGBA16F;
		break;
	}

	switch (pfd.cDepthBits)
	{
	default:
	case  0: _default_depth_format = GL_NONE; // No depth in this pixel format
		break;
	case 16: _default_depth_format = GL_DEPTH_COMPONENT16;
		break;
	case 24: _default_depth_format = pfd.cStencilBits ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24;
		break;
	case 32: _default_depth_format = pfd.cStencilBits ? GL_DEPTH32F_STENCIL8 : GL_DEPTH_COMPONENT32;
		break;
	}

	if (pfd.dwFlags & PFD_STEREO)
		_default_fbo_stereo = true;

	const auto wglGetPixelFormatAttribivARB = reinterpret_cast<BOOL(WINAPI *)(HDC hdc, int iPixelFormat, int iLayerPlane, UINT nAttributes, const int *piAttributes, int *piValues)>(wglGetProcAddress("wglGetPixelFormatAttribivARB"));
	if (wglGetPixelFormatAttribivARB != nullptr)
	{
		int attribs[2] = { 0x2042 /* WGL_SAMPLES_ARB */, 1 };
		if (wglGetPixelFormatAttribivARB(initial_hdc, _pixel_format, 0, 1, &attribs[0], &attribs[1]) && attribs[1] != 0)
			_default_fbo_samples = attribs[1];
	}

	// Check whether this context supports Direct State Access
	_supports_dsa = gl3wIsSupported(4, 5);

	// Check for special extension to detect whether this is a compatibility context (https://www.khronos.org/opengl/wiki/OpenGL_Context#OpenGL_3.1_and_ARB_compatibility)
	GLint num_extensions = 0;
	gl.GetIntegerv(GL_NUM_EXTENSIONS, &num_extensions);
	for (GLint i = 0; i < num_extensions; ++i)
	{
		const GLubyte *const extension = gl.GetStringi(GL_EXTENSIONS, i);
		if (std::strcmp(reinterpret_cast<const char *>(extension), "GL_ARB_compatibility") == 0)
		{
			_compatibility_context = true;
			break;
		}
	}

#ifndef NDEBUG
	const auto debug_message_callback = [](unsigned int /*source*/, unsigned int type, unsigned int /*id*/, unsigned int /*severity*/, int /*length*/, const char *message, const void * /*userParam*/) {
		if (type == GL_DEBUG_TYPE_ERROR || type == GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR)
			OutputDebugStringA(message), OutputDebugStringA("\n");
	};

	gl.Enable(GL_DEBUG_OUTPUT);
	gl.Enable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
	gl.DebugMessageCallback(debug_message_callback, nullptr);
#endif

	// Some games use fixed resource names, which can clash with the ones ReShade generates below, since most implementations will return values linearly
	// Reserve a configurable range of resource names in old OpenGL games (which will use a compatibility context) to work around this
	// - Call of Duty uses buffer and texture names in range 0-1500
	// - Star Wars Jedi Knight II: Jedi Outcast uses texture names in range 2000-3000
	auto num_reserve_buffer_names = _compatibility_context ? 2000 : 0u;
	reshade::global_config().get("APP", "ReserveBufferNames", num_reserve_buffer_names);
	_reserved_buffer_names.resize(num_reserve_buffer_names);
	if (!_reserved_buffer_names.empty())
		gl.GenBuffers(static_cast<GLsizei>(_reserved_buffer_names.size()), _reserved_buffer_names.data());
	auto num_reserve_texture_names = _compatibility_context ? 4000 : 0u;
	reshade::global_config().get("APP", "ReserveTextureNames", num_reserve_texture_names);
	_reserved_texture_names.resize(num_reserve_texture_names);
	if (!_reserved_texture_names.empty())
		gl.GenTextures(static_cast<GLsizei>(_reserved_texture_names.size()), _reserved_texture_names.data());

#if RESHADE_ADDON
	load_addons();

	invoke_addon_event<addon_event::init_device>(this);

	GLint max_combined_texture_image_units = 0;
	gl.GetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &max_combined_texture_image_units);
	GLint max_shader_storage_buffer_bindings = 0;
	gl.GetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &max_shader_storage_buffer_bindings);
	GLint max_uniform_buffer_bindings = 0;
	gl.GetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &max_uniform_buffer_bindings);
	GLint max_image_units = 0;
	gl.GetIntegerv(GL_MAX_IMAGE_UNITS, &max_image_units);

	const api::pipeline_layout_param global_pipeline_layout_params[6] = {
		api::descriptor_range { 0, 0, 0, static_cast<uint32_t>(max_combined_texture_image_units), api::shader_stage::all, 1, api::descriptor_type::sampler_with_resource_view },
		api::descriptor_range { 0, 0, 0, static_cast<uint32_t>(max_shader_storage_buffer_bindings), api::shader_stage::all, 1, api::descriptor_type::shader_storage_buffer },
		api::descriptor_range { 0, 0, 0, static_cast<uint32_t>(max_uniform_buffer_bindings), api::shader_stage::all, 1, api::descriptor_type::constant_buffer },
		api::descriptor_range { 0, 0, 0, static_cast<uint32_t>(max_image_units), api::shader_stage::all, 1, api::descriptor_type::unordered_access_view },
		/* Float uniforms */ api::constant_range { 0, 0, 0, std::numeric_limits<uint32_t>::max(), api::shader_stage::all },
		/* Integer uniforms */ api::constant_range { 0, 0, 0, std::numeric_limits<uint32_t>::max(), api::shader_stage::all },
	};
	invoke_addon_event<addon_event::init_pipeline_layout>(this, static_cast<uint32_t>(std::size(global_pipeline_layout_params)), global_pipeline_layout_params, global_pipeline_layout);
#endif
}
reshade::opengl::device_impl::~device_impl()
{
#if RESHADE_ADDON
	invoke_addon_event<addon_event::destroy_pipeline_layout>(this, global_pipeline_layout);

	invoke_addon_event<addon_event::destroy_device>(this);

	unload_addons();
#endif

	assert(_map_lookup.empty());

	// Free range of reserved resource names
	gl.DeleteBuffers(static_cast<GLsizei>(_reserved_buffer_names.size()), _reserved_buffer_names.data());
	gl.DeleteTextures(static_cast<GLsizei>(_reserved_texture_names.size()), _reserved_texture_names.data());
}

bool reshade::opengl::device_impl::check_capability(api::device_caps capability) const
{
	GLint value;

	switch (capability)
	{
	case api::device_caps::compute_shader:
		return true; // OpenGL 4.3
	case api::device_caps::geometry_shader:
		return true; // OpenGL 3.2
	case api::device_caps::hull_and_domain_shader:
		return true; // OpenGL 4.0
	case api::device_caps::logic_op:
		return true; // OpenGL 1.1
	case api::device_caps::dual_source_blend:
		return true; // OpenGL 3.3
	case api::device_caps::independent_blend:
		return true; // OpenGL 4.0
	case api::device_caps::fill_mode_non_solid:
		return true;
	case api::device_caps::conservative_rasterization:
		return false;
	case api::device_caps::bind_render_targets_and_depth_stencil:
	case api::device_caps::multi_viewport:
		return true;
	case api::device_caps::partial_push_constant_updates:
		return false;
	case api::device_caps::partial_push_descriptor_updates:
		return true;
	case api::device_caps::draw_instanced:
		return true; // OpenGL 3.1
	case api::device_caps::draw_or_dispatch_indirect:
		return true; // OpenGL 4.0
	case api::device_caps::copy_buffer_region:
	case api::device_caps::copy_buffer_to_texture:
	case api::device_caps::blit:
	case api::device_caps::resolve_region:
		return true;
	case api::device_caps::copy_query_pool_results:
		return gl.GetQueryBufferObjectui64v != nullptr; // OpenGL 4.5
	case api::device_caps::sampler_compare:
		return true;
	case api::device_caps::sampler_anisotropic:
		gl.GetIntegerv(GL_TEXTURE_MAX_ANISOTROPY, &value); // Core in OpenGL 4.6
		return value > 1;
	case api::device_caps::sampler_with_resource_view:
		return true;
	case api::device_caps::shared_resource:
	case api::device_caps::shared_resource_nt_handle:
		// TODO: Implement using 'GL_EXT_memory_object' and 'GL_EXT_memory_object_win32' extensions
	default:
		return false;
	}
}
bool reshade::opengl::device_impl::check_format_support(api::format format, api::resource_usage usage) const
{
	const GLenum internal_format = convert_format(format);
	if (internal_format == GL_NONE)
		return false;

	GLint supported = GL_FALSE;
	gl.GetInternalformativ(GL_TEXTURE_2D, internal_format, GL_INTERNALFORMAT_SUPPORTED, 1, &supported);

	GLint supported_depth = GL_TRUE;
	GLint supported_stencil = GL_TRUE;
	if ((usage & api::resource_usage::depth_stencil) != 0)
	{
		gl.GetInternalformativ(GL_TEXTURE_2D, internal_format, GL_DEPTH_RENDERABLE, 1, &supported_depth);
		gl.GetInternalformativ(GL_TEXTURE_2D, internal_format, GL_STENCIL_RENDERABLE, 1, &supported_stencil);
	}

	GLint supported_color_render = GL_TRUE;
	GLint supported_render_target = GL_CAVEAT_SUPPORT;
	if ((usage & api::resource_usage::render_target) != 0)
	{
		gl.GetInternalformativ(GL_TEXTURE_2D, internal_format, GL_COLOR_RENDERABLE, 1, &supported_color_render);
		gl.GetInternalformativ(GL_TEXTURE_2D, internal_format, GL_FRAMEBUFFER_RENDERABLE, 1, &supported_render_target);
	}

	GLint supported_unordered_access_load = GL_CAVEAT_SUPPORT;
	GLint supported_unordered_access_store = GL_CAVEAT_SUPPORT;
	if ((usage & api::resource_usage::unordered_access) != 0)
	{
		gl.GetInternalformativ(GL_TEXTURE_2D, internal_format, GL_SHADER_IMAGE_LOAD, 1, &supported_unordered_access_load);
		gl.GetInternalformativ(GL_TEXTURE_2D, internal_format, GL_SHADER_IMAGE_STORE, 1, &supported_unordered_access_store);
	}

	return supported && (supported_depth || supported_stencil) && (supported_color_render && supported_render_target) && (supported_unordered_access_load && supported_unordered_access_store);
}

bool reshade::opengl::device_impl::create_sampler(const api::sampler_desc &desc, api::sampler *out_handle)
{
	GLuint object = 0;
	gl.GenSamplers(1, &object);

	GLenum min_filter = GL_NONE;
	GLenum mag_filter = GL_NONE;
	switch (desc.filter)
	{
	case api::filter_mode::min_mag_mip_point:
	case api::filter_mode::compare_min_mag_mip_point:
		min_filter = GL_NEAREST_MIPMAP_NEAREST;
		mag_filter = GL_NEAREST;
		break;
	case api::filter_mode::min_mag_point_mip_linear:
	case api::filter_mode::compare_min_mag_point_mip_linear:
		min_filter = GL_NEAREST_MIPMAP_LINEAR;
		mag_filter = GL_NEAREST;
		break;
	case api::filter_mode::min_point_mag_linear_mip_point:
	case api::filter_mode::compare_min_point_mag_linear_mip_point:
		min_filter = GL_NEAREST_MIPMAP_NEAREST;
		mag_filter = GL_LINEAR;
		break;
	case api::filter_mode::min_point_mag_mip_linear:
	case api::filter_mode::compare_min_point_mag_mip_linear:
		min_filter = GL_NEAREST_MIPMAP_LINEAR;
		mag_filter = GL_LINEAR;
		break;
	case api::filter_mode::min_linear_mag_mip_point:
	case api::filter_mode::compare_min_linear_mag_mip_point:
		min_filter = GL_LINEAR_MIPMAP_NEAREST;
		mag_filter = GL_NEAREST;
		break;
	case api::filter_mode::min_linear_mag_point_mip_linear:
	case api::filter_mode::compare_min_linear_mag_point_mip_linear:
		min_filter = GL_LINEAR_MIPMAP_LINEAR;
		mag_filter = GL_NEAREST;
		break;
	case api::filter_mode::min_mag_linear_mip_point:
	case api::filter_mode::compare_min_mag_linear_mip_point:
		min_filter = GL_LINEAR_MIPMAP_NEAREST;
		mag_filter = GL_LINEAR;
		break;
	case api::filter_mode::anisotropic:
	case api::filter_mode::compare_anisotropic:
		gl.SamplerParameterf(object, GL_TEXTURE_MAX_ANISOTROPY, desc.max_anisotropy);
		[[fallthrough]];
	case api::filter_mode::min_mag_mip_linear:
	case api::filter_mode::compare_min_mag_mip_linear:
		min_filter = GL_LINEAR_MIPMAP_LINEAR;
		mag_filter = GL_LINEAR;
		break;
	}

	const auto convert_address_mode = [](api::texture_address_mode value) {
		switch (value)
		{
		default:
			return GL_NONE;
		case api::texture_address_mode::wrap:
			return GL_REPEAT;
		case api::texture_address_mode::mirror:
			return GL_MIRRORED_REPEAT;
		case api::texture_address_mode::clamp:
			return GL_CLAMP_TO_EDGE;
		case api::texture_address_mode::border:
			return GL_CLAMP_TO_BORDER;
		}
	};

	gl.SamplerParameteri(object, GL_TEXTURE_MIN_FILTER, min_filter);
	gl.SamplerParameteri(object, GL_TEXTURE_MAG_FILTER, mag_filter);
	gl.SamplerParameteri(object, GL_TEXTURE_WRAP_S, convert_address_mode(desc.address_u));
	gl.SamplerParameteri(object, GL_TEXTURE_WRAP_T, convert_address_mode(desc.address_v));
	gl.SamplerParameteri(object, GL_TEXTURE_WRAP_R, convert_address_mode(desc.address_w));
	gl.SamplerParameterf(object, GL_TEXTURE_LOD_BIAS, desc.mip_lod_bias);
	gl.SamplerParameteri(object, GL_TEXTURE_COMPARE_MODE, (static_cast<uint32_t>(desc.filter) & 0x80) != 0 ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE);
	gl.SamplerParameteri(object, GL_TEXTURE_COMPARE_FUNC, convert_compare_op(desc.compare_op));
	gl.SamplerParameterf(object, GL_TEXTURE_MIN_LOD, desc.min_lod);
	gl.SamplerParameterf(object, GL_TEXTURE_MAX_LOD, desc.max_lod);

	gl.SamplerParameterfv(object, GL_TEXTURE_BORDER_COLOR, desc.border_color);

	*out_handle = { static_cast<uint64_t>(object) };
	return true;
}
void reshade::opengl::device_impl::destroy_sampler(api::sampler handle)
{
	const GLuint object = handle.handle & 0xFFFFFFFF;
	gl.DeleteSamplers(1, &object);
}

bool reshade::opengl::device_impl::create_resource(const api::resource_desc &desc, const api::subresource_data *initial_data, api::resource_usage, api::resource *out_handle, HANDLE * /* shared_handle */)
{
	*out_handle = { 0 };

	GLenum target = GL_NONE;
	switch (desc.type)
	{
	case api::resource_type::buffer:
		switch (desc.usage & (api::resource_usage::index_buffer | api::resource_usage::vertex_buffer | api::resource_usage::constant_buffer))
		{
		case api::resource_usage::index_buffer:
			target = GL_ELEMENT_ARRAY_BUFFER;
			break;
		case api::resource_usage::vertex_buffer:
			target = GL_ARRAY_BUFFER;
			break;
		case api::resource_usage::constant_buffer:
			target = GL_UNIFORM_BUFFER;
			break;
		case api::resource_usage::stream_output:
			target = GL_TRANSFORM_FEEDBACK_BUFFER;
			break;
		default:
			target = GL_COPY_WRITE_BUFFER;
			if (desc.heap == api::memory_heap::gpu_to_cpu)
				target = GL_PIXEL_PACK_BUFFER;
			else if (desc.heap == api::memory_heap::cpu_to_gpu)
				target = GL_PIXEL_UNPACK_BUFFER;
			break;
		}
		break;
	case api::resource_type::texture_1d:
		target = desc.texture.depth_or_layers > 1 ? GL_TEXTURE_1D_ARRAY : GL_TEXTURE_1D;
		break;
	case api::resource_type::texture_2d:
		if ((desc.flags & api::resource_flags::cube_compatible) == 0)
			target = desc.texture.depth_or_layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
		else
			target = desc.texture.depth_or_layers > 6 ? GL_TEXTURE_CUBE_MAP_ARRAY : GL_TEXTURE_CUBE_MAP;
		break;
	case api::resource_type::texture_3d:
		target = GL_TEXTURE_3D;
		break;
	default:
		return false;
	}

#if 0
	GLenum shared_handle_type = GL_NONE;
	if ((desc.flags & api::resource_flags::shared) != 0)
	{
		// Only import is supported
		if (shared_handle == nullptr || *shared_handle == nullptr)
			return false;

		assert(initial_data == nullptr);

		if ((desc.flags & api::resource_flags::shared_nt_handle) != 0)
			shared_handle_type = GL_HANDLE_TYPE_OPAQUE_WIN32_EXT;
		else
			shared_handle_type = GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT;
	}
#else
	if ((desc.flags & api::resource_flags::shared) != 0)
		return false;
#endif

	GLuint object = 0;
	GLuint prev_binding = 0;
	gl.GetIntegerv(get_binding_for_target(target), reinterpret_cast<GLint *>(&prev_binding));

	// Clear any errors that may still be on the stack
	while (gl.GetError() != GL_NO_ERROR)
		continue;
	GLenum status = GL_NO_ERROR;

	if (desc.type == api::resource_type::buffer)
	{
		if (desc.buffer.size == 0)
			return false;

		gl.GenBuffers(1, &object);
		gl.BindBuffer(target, object);

		GLsizeiptr buffer_size = 0;
		GLenum usage = GL_NONE;
		convert_resource_desc(desc, buffer_size, usage);

#if 0
		if (shared_handle_type != GL_NONE)
		{
			GLuint mem = 0;
			gl.CreateMemoryObjectsEXT(1, &mem);
			gl.ImportMemoryWin32HandleEXT(mem, buffer_size, shared_handle_type, *shared_handle);

			gl.BufferStorageMemEXT(target, buffer_size, mem, 0);

			gl.DeleteMemoryObjectsEXT(1, &mem);
		}
		else
#endif
		{
			GLbitfield usage_flags = GL_NONE;
			convert_memory_usage_to_flags(usage, usage_flags);

			// Upload of initial data is using 'glBufferSubData', which requires the dynamic storage flag
			if (initial_data != nullptr)
				usage_flags |= GL_DYNAMIC_STORAGE_BIT;

			gl.BufferStorage(target, buffer_size, nullptr, usage_flags);
		}

		status = gl.GetError();

		if (initial_data != nullptr && status == GL_NO_ERROR)
		{
			update_buffer_region(initial_data->data, make_resource_handle(GL_BUFFER, object), 0, desc.buffer.size);
		}

		gl.BindBuffer(target, prev_binding);

		// Handles to buffer resources always have the target set to 'GL_BUFFER'
		target = GL_BUFFER;

		if (status != GL_NO_ERROR)
		{
			gl.DeleteBuffers(1, &object);
			return false;
		}
	}
	else
	{
		GLint swizzle_mask[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };

		const GLenum internal_format = convert_format(desc.texture.format, swizzle_mask);
		if (desc.texture.width == 0 || internal_format == GL_NONE)
			return false;

		gl.GenTextures(1, &object);
		gl.BindTexture(target, object);

		GLuint depth_or_layers = desc.texture.depth_or_layers;

#if 0
		if (shared_handle_type != GL_NONE)
		{
			GLuint mem = 0;
			gl.CreateMemoryObjectsEXT(1, &mem);

			GLuint64 import_size = 0;
			for (uint32_t level = 0, width = desc.texture.width, height = desc.texture.height; level < desc.texture.levels; ++level, width /= 2, height /= 2)
				import_size += api::format_slice_pitch(desc.texture.format, api::format_row_pitch(desc.texture.format, width), height);
			import_size *= desc.texture.depth_or_layers;

			gl.ImportMemoryWin32HandleEXT(mem, import_size, shared_handle_type, *shared_handle);

			switch (target)
			{
			case GL_TEXTURE_1D:
			case GL_TEXTURE_1D_ARRAY:
				if (desc.texture.samples > 1)
					gl.TexStorageMem2DMultisampleEXT(target, desc.texture.samples, internal_format, desc.texture.width, depth_or_layers, GL_FALSE, mem, 0);
				else
					gl.TexStorageMem2DEXT(target, desc.texture.levels, internal_format, desc.texture.width, depth_or_layers, mem, 0);
				break;
			case GL_TEXTURE_CUBE_MAP:
				assert(depth_or_layers == 6);
				[[fallthrough]];
			case GL_TEXTURE_2D:
				if (desc.texture.samples > 1)
					gl.TexStorageMem2DMultisampleEXT(target, desc.texture.samples, internal_format, desc.texture.width, desc.texture.height, GL_FALSE, mem, 0);
				else
					gl.TexStorageMem2DEXT(target, desc.texture.levels, internal_format, desc.texture.width, desc.texture.height, mem, 0);
				break;
			case GL_TEXTURE_CUBE_MAP_ARRAY:
				assert((depth_or_layers % 6) == 0);
				depth_or_layers /= 6;
				[[fallthrough]];
			case GL_TEXTURE_2D_ARRAY:
			case GL_TEXTURE_3D:
				if (desc.texture.samples > 1)
					gl.TexStorageMem3DMultisampleEXT(target, desc.texture.samples, internal_format, desc.texture.width, desc.texture.height, depth_or_layers, GL_FALSE, mem, 0);
				else
					gl.TexStorageMem3DEXT(target, desc.texture.levels, internal_format, desc.texture.width, desc.texture.height, depth_or_layers, mem, 0);
				break;
			}

			gl.DeleteMemoryObjectsEXT(1, &mem);
		}
		else
#endif
		{
			switch (target)
			{
			case GL_TEXTURE_1D:
				gl.TexStorage1D(target, desc.texture.levels, internal_format, desc.texture.width);
				break;
			case GL_TEXTURE_1D_ARRAY:
				if (desc.texture.samples > 1)
					gl.TexStorage2DMultisample(target, desc.texture.samples, internal_format, desc.texture.width, depth_or_layers, GL_FALSE);
				else
					gl.TexStorage2D(target, desc.texture.levels, internal_format, desc.texture.width, depth_or_layers);
				break;
			case GL_TEXTURE_CUBE_MAP:
				assert(depth_or_layers == 6);
				[[fallthrough]];
			case GL_TEXTURE_2D:
				if (desc.texture.samples > 1)
					gl.TexStorage2DMultisample(target, desc.texture.samples, internal_format, desc.texture.width, desc.texture.height, GL_FALSE);
				else
					gl.TexStorage2D(target, desc.texture.levels, internal_format, desc.texture.width, desc.texture.height);
				break;
			case GL_TEXTURE_CUBE_MAP_ARRAY:
				assert((depth_or_layers % 6) == 0);
				depth_or_layers /= 6;
				[[fallthrough]];
			case GL_TEXTURE_2D_ARRAY:
			case GL_TEXTURE_3D:
				if (desc.texture.samples > 1)
					gl.TexStorage3DMultisample(target, desc.texture.samples, internal_format, desc.texture.width, desc.texture.height, depth_or_layers, GL_FALSE);
				else
					gl.TexStorage3D(target, desc.texture.levels, internal_format, desc.texture.width, desc.texture.height, depth_or_layers);
				break;
			}
		}

		status = gl.GetError();

		gl.TexParameteriv(target, GL_TEXTURE_SWIZZLE_RGBA, swizzle_mask);

		if (initial_data != nullptr && status == GL_NO_ERROR)
		{
			for (uint32_t subresource = 0; subresource < static_cast<uint32_t>(desc.texture.depth_or_layers) * desc.texture.levels; ++subresource)
				update_texture_region(initial_data[subresource], make_resource_handle(target, object), subresource, nullptr);
		}

		gl.BindTexture(target, prev_binding);

		if (status != GL_NO_ERROR)
		{
			gl.DeleteTextures(1, &object);
			return false;
		}
	}

	*out_handle = make_resource_handle(target, object);
	return true;
}
void reshade::opengl::device_impl::destroy_resource(api::resource handle)
{
	const GLuint object = handle.handle & 0xFFFFFFFF;
	switch (handle.handle >> 40)
	{
	case GL_BUFFER:
		gl.DeleteBuffers(1, &object);
		break;
	case GL_TEXTURE_BUFFER:
	case GL_TEXTURE_1D:
	case GL_TEXTURE_1D_ARRAY:
	case GL_TEXTURE_2D:
	case GL_TEXTURE_2D_ARRAY:
	case GL_TEXTURE_2D_MULTISAMPLE:
	case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
	case GL_TEXTURE_3D:
	case GL_TEXTURE_CUBE_MAP:
	case GL_TEXTURE_CUBE_MAP_ARRAY:
	case GL_TEXTURE_RECTANGLE:
		gl.DeleteTextures(1, &object);
		break;
	case GL_RENDERBUFFER:
		gl.DeleteRenderbuffers(1, &object);
		break;
	case GL_FRAMEBUFFER_DEFAULT:
		assert(false); // It is not allowed to destroy the default frame buffer
		break;
	default:
		assert(object == 0);
		break;
	}
}

reshade::api::resource_desc reshade::opengl::device_impl::get_resource_desc(api::resource resource) const
{
	const GLenum target = resource.handle >> 40;
	const GLuint object = resource.handle & 0xFFFFFFFF;

	switch (target)
	{
		case GL_BUFFER:
		{
#ifndef _WIN64
			GLint size = 0;
#else
			GLint64 size = 0;
#endif
			GLint usage = GL_NONE;

			if (_supports_dsa)
			{
#ifndef _WIN64
				gl.GetNamedBufferParameteriv(object, GL_BUFFER_SIZE, &size);
#else
				gl.GetNamedBufferParameteri64v(object, GL_BUFFER_SIZE, &size);
#endif
				gl.GetNamedBufferParameteriv(object, GL_BUFFER_USAGE, &usage);
			}
			else
			{
				GLuint prev_binding = 0;
				gl.GetIntegerv(GL_COPY_READ_BUFFER_BINDING, reinterpret_cast<GLint *>(&prev_binding));
				gl.BindBuffer(GL_COPY_READ_BUFFER, object);

#ifndef _WIN64
				gl.GetBufferParameteriv(GL_COPY_READ_BUFFER, GL_BUFFER_SIZE, &size);
#else
				gl.GetBufferParameteri64v(GL_COPY_READ_BUFFER, GL_BUFFER_SIZE, &size);
#endif
				gl.GetBufferParameteriv(GL_COPY_READ_BUFFER, GL_BUFFER_USAGE, &usage);

				gl.BindBuffer(GL_COPY_READ_BUFFER, prev_binding);
			}

			return convert_resource_desc(target, size, usage);
		}
		case GL_TEXTURE_BUFFER:
		case GL_TEXTURE_1D:
		case GL_TEXTURE_1D_ARRAY:
		case GL_TEXTURE_2D:
		case GL_TEXTURE_2D_ARRAY:
		case GL_TEXTURE_2D_MULTISAMPLE:
		case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
		case GL_TEXTURE_3D:
		case GL_TEXTURE_CUBE_MAP:
		case GL_TEXTURE_CUBE_MAP_ARRAY:
		case GL_TEXTURE_RECTANGLE:
		{
			GLint width = 0, height = 1, depth = 1, levels = 1, samples = 1, internal_format = GL_NONE;
			GLint swizzle_mask[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };

			if (_supports_dsa)
			{
				gl.GetTextureLevelParameteriv(object, 0, GL_TEXTURE_WIDTH, &width);
				gl.GetTextureLevelParameteriv(object, 0, GL_TEXTURE_HEIGHT, &height);
				gl.GetTextureLevelParameteriv(object, 0, GL_TEXTURE_DEPTH, &depth);
				gl.GetTextureLevelParameteriv(object, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);
				gl.GetTextureLevelParameteriv(object, 0, GL_TEXTURE_SAMPLES, &samples);

				gl.GetTextureParameteriv(object, GL_TEXTURE_IMMUTABLE_LEVELS, &levels);
				if (levels == 0)
				{
					// If number of mipmap levels is not immutable, need to walk through the mipmap chain and check how many actually exist
					gl.GetTextureParameteriv(object, GL_TEXTURE_MAX_LEVEL, &levels);
					for (GLsizei level = 1, level_w = 0; level < levels; ++level)
					{
						// Check if this mipmap level does exist
						gl.GetTextureLevelParameteriv(object, level, GL_TEXTURE_WIDTH, &level_w);
						if (0 == level_w)
						{
							levels = level;
							break;
						}
					}
				}

				gl.GetTextureParameteriv(object, GL_TEXTURE_SWIZZLE_RGBA, swizzle_mask);
			}
			else
			{
				GLuint prev_binding = 0;
				gl.GetIntegerv(get_binding_for_target(target), reinterpret_cast<GLint *>(&prev_binding));
				gl.BindTexture(target, object);

				GLenum level_target = target;
				if (target == GL_TEXTURE_CUBE_MAP || target == GL_TEXTURE_CUBE_MAP_ARRAY)
					level_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X;

				gl.GetTexLevelParameteriv(level_target, 0, GL_TEXTURE_WIDTH, &width);
				gl.GetTexLevelParameteriv(level_target, 0, GL_TEXTURE_HEIGHT, &height);
				gl.GetTexLevelParameteriv(level_target, 0, GL_TEXTURE_DEPTH, &depth);
				gl.GetTexLevelParameteriv(level_target, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);
				gl.GetTexLevelParameteriv(level_target, 0, GL_TEXTURE_SAMPLES, &samples);

				gl.GetTexParameteriv(target, GL_TEXTURE_IMMUTABLE_LEVELS, &levels);
				if (levels == 0)
				{
					gl.GetTexParameteriv(target, GL_TEXTURE_MAX_LEVEL, &levels);
					for (GLsizei level = 1, level_w = 0; level < levels; ++level)
					{
						gl.GetTexLevelParameteriv(level_target, level, GL_TEXTURE_WIDTH, &level_w);
						if (0 == level_w)
						{
							levels = level;
							break;
						}
					}
				}

				gl.GetTexParameteriv(target, GL_TEXTURE_SWIZZLE_RGBA, swizzle_mask);

				gl.BindTexture(target, prev_binding);
			}

			if (0 == levels)
				levels = 1;
			if (0 == samples)
				samples = 1;

			return convert_resource_desc(target, levels, samples, internal_format, width, height, depth, swizzle_mask);
		}
		case GL_RENDERBUFFER:
		{
			GLint width = 0, height = 1, samples = 1, internal_format = GL_NONE;

			if (_supports_dsa)
			{
				gl.GetNamedRenderbufferParameteriv(object, GL_RENDERBUFFER_WIDTH, &width);
				gl.GetNamedRenderbufferParameteriv(object, GL_RENDERBUFFER_HEIGHT, &height);
				gl.GetNamedRenderbufferParameteriv(object, GL_RENDERBUFFER_INTERNAL_FORMAT, &internal_format);
				gl.GetNamedRenderbufferParameteriv(object, GL_RENDERBUFFER_SAMPLES, &samples);
			}
			else
			{
				GLuint prev_binding = 0;
				gl.GetIntegerv(GL_RENDERBUFFER_BINDING, reinterpret_cast<GLint *>(&prev_binding));
				gl.BindRenderbuffer(GL_RENDERBUFFER, object);

				gl.GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
				gl.GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
				gl.GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &internal_format);
				gl.GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &samples);

				gl.BindRenderbuffer(GL_RENDERBUFFER, prev_binding);
			}

			if (0 == samples)
				samples = 1;

			return convert_resource_desc(target, 1, samples, internal_format, width, height);
		}
		case GL_FRAMEBUFFER_DEFAULT:
		{
			const GLenum internal_format = (object == GL_DEPTH_STENCIL_ATTACHMENT || object == GL_DEPTH_ATTACHMENT || object == GL_STENCIL_ATTACHMENT) ? _default_depth_format : _default_color_format;

			return convert_resource_desc(target, 1, _default_fbo_samples, internal_format, _default_fbo_width, _default_fbo_height, _default_fbo_stereo ? 2 : 1);
		}
	}

	assert(false); // Not implemented
	return api::resource_desc {};
}

bool reshade::opengl::device_impl::create_resource_view(api::resource resource, api::resource_usage, const api::resource_view_desc &desc, api::resource_view *out_handle)
{
	*out_handle = { 0 };

	if (resource.handle == 0)
		return false;

	const GLenum resource_target = resource.handle >> 40;
	const GLenum resource_object = resource.handle & 0xFFFFFFFF;
	if (resource_target == GL_FRAMEBUFFER_DEFAULT && _default_fbo_stereo && desc.texture.layer_count == 1)
	{
		*out_handle = make_resource_view_handle(resource_target, desc.texture.first_layer == 0 ? GL_BACK_LEFT : GL_BACK_RIGHT);
		return true;
	}
	else if (resource_target == GL_FRAMEBUFFER_DEFAULT || resource_target == GL_RENDERBUFFER)
	{
		*out_handle = make_resource_view_handle(resource_target, resource_object);
		return true;
	}

	GLenum target = GL_NONE;
	switch (desc.type)
	{
	case api::resource_view_type::buffer:
		assert(resource_target == GL_BUFFER);
		target = GL_TEXTURE_BUFFER;
		break;
	case api::resource_view_type::texture_1d:
		target = GL_TEXTURE_1D;
		break;
	case api::resource_view_type::texture_1d_array:
		target = GL_TEXTURE_1D_ARRAY;
		break;
	case api::resource_view_type::texture_2d:
		target = GL_TEXTURE_2D;
		break;
	case api::resource_view_type::texture_2d_array:
		target = GL_TEXTURE_2D_ARRAY;
		break;
	case api::resource_view_type::texture_2d_multisample:
		target = GL_TEXTURE_2D_MULTISAMPLE;
		break;
	case api::resource_view_type::texture_2d_multisample_array:
		target = GL_TEXTURE_2D_MULTISAMPLE_ARRAY;
		break;
	case api::resource_view_type::texture_3d:
		target = GL_TEXTURE_3D;
		break;
	case api::resource_view_type::texture_cube:
		target = GL_TEXTURE_CUBE_MAP;
		break;
	case api::resource_view_type::texture_cube_array:
		target = GL_TEXTURE_CUBE_MAP_ARRAY;
		break;
	default:
		return false;
	}

	GLenum resource_format = GL_NONE;
	if (desc.type != api::resource_view_type::buffer)
	{
		if (_supports_dsa)
		{
			gl.GetTextureLevelParameteriv(resource_object, 0, GL_TEXTURE_INTERNAL_FORMAT, reinterpret_cast<GLint *>(&resource_format));
		}
		else
		{
			GLuint prev_binding = 0;
			gl.GetIntegerv(get_binding_for_target(resource_target), reinterpret_cast<GLint *>(&prev_binding));
			gl.BindTexture(resource_target, resource_object);

			gl.GetTexLevelParameteriv(resource_target, 0, GL_TEXTURE_INTERNAL_FORMAT, reinterpret_cast<GLint *>(&resource_format));

			gl.BindTexture(resource_target, prev_binding);
		}
	}

	GLint texture_swizzle[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };

	const GLenum internal_format = convert_format(desc.format, texture_swizzle);
	if (internal_format == GL_NONE)
		return false;

	if (target == resource_target &&
		desc.texture.first_level == 0 && desc.texture.first_layer == 0 && internal_format == resource_format)
	{
		assert(target != GL_TEXTURE_BUFFER);

		// No need to create a view, so use resource directly, but set a bit so to not destroy it twice via 'destroy_resource_view'
		*out_handle = make_resource_view_handle(target, resource_object);
		return true;
	}
	else if (resource_target == GL_TEXTURE_CUBE_MAP && target == GL_TEXTURE_2D && desc.texture.layer_count == 1)
	{
		// Cube map face is handled via special target
		*out_handle = make_resource_view_handle(GL_TEXTURE_CUBE_MAP_POSITIVE_X + desc.texture.first_layer, resource_object);
		return true;
	}

	GLuint object = 0;
	GLuint prev_binding = 0;
	gl.GetIntegerv(get_binding_for_target(target), reinterpret_cast<GLint *>(&prev_binding));

	// Clear any errors that may still be on the stack
	while (gl.GetError() != GL_NO_ERROR)
		continue;
	GLenum status = GL_NO_ERROR;

	gl.GenTextures(1, &object);

	if (desc.type == reshade::api::resource_view_type::buffer)
	{
		gl.BindTexture(target, object);

		if (desc.buffer.offset == 0 && desc.buffer.size == UINT64_MAX)
		{
			gl.TexBuffer(target, internal_format, resource_object);
		}
		else
		{
			assert(desc.buffer.offset <= static_cast<uint64_t>(std::numeric_limits<GLintptr>::max()));
			assert(desc.buffer.size <= static_cast<uint64_t>(std::numeric_limits<GLsizeiptr>::max()));
			gl.TexBufferRange(target, internal_format, resource_object, static_cast<GLintptr>(desc.buffer.offset), static_cast<GLsizeiptr>(desc.buffer.size));
		}
	}
	else
	{
		// Number of levels and layers are clamped to those of the original texture
		gl.TextureView(object, target, resource_object, internal_format, desc.texture.first_level, desc.texture.level_count, desc.texture.first_layer, desc.texture.layer_count);

		gl.BindTexture(target, object);
		gl.TexParameteriv(target, GL_TEXTURE_SWIZZLE_RGBA, texture_swizzle);
	}

	status = gl.GetError();

	gl.BindTexture(target, prev_binding);

	if (status != GL_NO_ERROR)
	{
		gl.DeleteTextures(1, &object);
		return false;
	}

	*out_handle = make_resource_view_handle(target, object, true);
	return true;
}
void reshade::opengl::device_impl::destroy_resource_view(api::resource_view handle)
{
	// Check if this is a standalone object (see 'make_resource_view_handle')
	if (((handle.handle >> 32) & 0x1) != 0)
		destroy_resource({ handle.handle });
}

reshade::api::format reshade::opengl::device_impl::get_resource_view_format(api::resource_view view) const
{
	const GLenum target = view.handle >> 40;
	const GLuint object = view.handle & 0xFFFFFFFF;

	GLint internal_format = GL_NONE;
	GLint swizzle_mask[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };

	switch (target)
	{
	case GL_TEXTURE_BUFFER:
	case GL_TEXTURE_1D:
	case GL_TEXTURE_1D_ARRAY:
	case GL_TEXTURE_2D:
	case GL_TEXTURE_2D_ARRAY:
	case GL_TEXTURE_2D_MULTISAMPLE:
	case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
	case GL_TEXTURE_3D:
	case GL_TEXTURE_CUBE_MAP:
	case GL_TEXTURE_CUBE_MAP_ARRAY:
	case GL_TEXTURE_RECTANGLE:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
	case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
	case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
	case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
		if (_supports_dsa)
		{
			gl.GetTextureLevelParameteriv(object, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);
			gl.GetTextureParameteriv(object, GL_TEXTURE_SWIZZLE_RGBA, swizzle_mask);
		}
		else
		{
			GLuint prev_binding = 0;
			gl.GetIntegerv(get_binding_for_target(target), reinterpret_cast<GLint *>(&prev_binding));
			gl.BindTexture(target, object);

			GLenum level_target = target;
			if (target == GL_TEXTURE_CUBE_MAP || target == GL_TEXTURE_CUBE_MAP_ARRAY)
				level_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X;

			gl.GetTexLevelParameteriv(level_target, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);
			gl.GetTexParameteriv(target, GL_TEXTURE_SWIZZLE_RGBA, swizzle_mask);

			gl.BindTexture(target, prev_binding);
		}
		break;
	case GL_RENDERBUFFER:
		if (_supports_dsa)
		{
			gl.GetNamedRenderbufferParameteriv(object, GL_RENDERBUFFER_INTERNAL_FORMAT, &internal_format);
		}
		else
		{
			GLuint prev_binding = 0;
			gl.GetIntegerv(GL_RENDERBUFFER_BINDING, reinterpret_cast<GLint *>(&prev_binding));
			gl.BindRenderbuffer(GL_RENDERBUFFER, object);

			gl.GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &internal_format);

			gl.BindRenderbuffer(GL_RENDERBUFFER, prev_binding);
		}
		break;
	case GL_FRAMEBUFFER_DEFAULT:
		internal_format = (object == GL_DEPTH_STENCIL_ATTACHMENT || object == GL_DEPTH_ATTACHMENT || object == GL_STENCIL_ATTACHMENT) ? _default_depth_format : _default_color_format;
		break;
	default:
		assert(false);
		break;
	}

	return convert_format(internal_format, swizzle_mask);
}
reshade::api::resource reshade::opengl::device_impl::get_resource_from_view(api::resource_view view) const
{
	assert(view.handle != 0);

	const GLenum target = view.handle >> 40;
	const GLuint object = view.handle & 0xFFFFFFFF;

	if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
		return make_resource_handle(GL_TEXTURE_2D, object);

	if (target != GL_TEXTURE_BUFFER)
	{
		return make_resource_handle(target, object);
	}
	else
	{
		GLint binding = 0;

		if (_supports_dsa)
		{
			gl.GetTextureLevelParameteriv(object, 0, GL_TEXTURE_BUFFER_DATA_STORE_BINDING, &binding);
		}
		else
		{
			GLuint prev_binding = 0;
			gl.GetIntegerv(get_binding_for_target(target), reinterpret_cast<GLint *>(&prev_binding));
			gl.BindTexture(target, object);

			gl.GetTexLevelParameteriv(target, 0, GL_TEXTURE_BUFFER_DATA_STORE_BINDING, &binding);

			gl.BindTexture(target, prev_binding);
		}

		return make_resource_handle(GL_BUFFER, binding);
	}
}
reshade::api::resource_view_desc reshade::opengl::device_impl::get_resource_view_desc(api::resource_view view) const
{
	assert(view.handle != 0);

	const GLenum target = view.handle >> 40;
	const GLuint object = view.handle & 0xFFFFFFFF;

	if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
		return api::resource_view_desc(get_resource_view_format(view), 0, UINT32_MAX, target - GL_TEXTURE_CUBE_MAP_POSITIVE_X, 1);
	if (((view.handle >> 32) & 0x1) == 0)
		return api::resource_view_desc(get_resource_view_format(view), 0, UINT32_MAX, 0, UINT32_MAX);

	if (target != GL_TEXTURE_BUFFER)
	{
		GLint min_level = 0, min_layer = 0, num_levels = 0, num_layers = 0, internal_format = GL_NONE;

		if (_supports_dsa)
		{
			gl.GetTextureParameteriv(object, GL_TEXTURE_VIEW_MIN_LEVEL, &min_level);
			gl.GetTextureParameteriv(object, GL_TEXTURE_VIEW_MIN_LAYER, &min_layer);
			gl.GetTextureParameteriv(object, GL_TEXTURE_VIEW_NUM_LEVELS, &num_levels);
			gl.GetTextureParameteriv(object, GL_TEXTURE_VIEW_NUM_LAYERS, &num_layers);
			gl.GetTextureLevelParameteriv(object, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);
		}
		else
		{
			GLuint prev_binding = 0;
			gl.GetIntegerv(get_binding_for_target(target), reinterpret_cast<GLint *>(&prev_binding));
			gl.BindTexture(target, object);

			gl.GetTexParameteriv(target, GL_TEXTURE_VIEW_MIN_LEVEL, &min_level);
			gl.GetTexParameteriv(target, GL_TEXTURE_VIEW_MIN_LAYER, &min_layer);
			gl.GetTexParameteriv(target, GL_TEXTURE_VIEW_NUM_LEVELS, &num_levels);
			gl.GetTexParameteriv(target, GL_TEXTURE_VIEW_NUM_LAYERS, &num_layers);
			gl.GetTexLevelParameteriv(target, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);

			gl.BindTexture(target, prev_binding);
		}

		return convert_resource_view_desc(target, internal_format, min_level, num_levels, min_layer, num_layers);
	}
	else
	{
		GLint offset = 0, size = 0, internal_format = GL_NONE;

		if (_supports_dsa)
		{
			gl.GetTextureLevelParameteriv(object, 0, GL_TEXTURE_BUFFER_OFFSET, &offset);
			gl.GetTextureLevelParameteriv(object, 0, GL_TEXTURE_BUFFER_SIZE, &size);
			gl.GetTextureLevelParameteriv(object, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);
		}
		else
		{
			GLuint prev_binding = 0;
			gl.GetIntegerv(get_binding_for_target(target), reinterpret_cast<GLint *>(&prev_binding));
			gl.BindTexture(target, object);

			gl.GetTexLevelParameteriv(target, 0, GL_TEXTURE_BUFFER_OFFSET, &offset);
			gl.GetTexLevelParameteriv(target, 0, GL_TEXTURE_BUFFER_SIZE, &size);
			gl.GetTexLevelParameteriv(target, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);

			gl.BindTexture(target, prev_binding);
		}

		return convert_resource_view_desc(target, internal_format, offset, size);
	}
}

reshade::api::resource_view reshade::opengl::render_context_impl::get_framebuffer_attachment(GLuint fbo_object, GLenum type, uint32_t index) const
{
	// Zero is valid too, in which case the default frame buffer is referenced, instead of a FBO
	if (!fbo_object)
	{
		if (index == 0)
		{
			if (type == GL_COLOR || type == GL_COLOR_BUFFER_BIT)
			{
				return make_resource_view_handle(GL_FRAMEBUFFER_DEFAULT, GL_BACK);
			}
			if (_device_impl->_default_depth_format != GL_NONE)
			{
				return make_resource_view_handle(GL_FRAMEBUFFER_DEFAULT, GL_DEPTH_STENCIL_ATTACHMENT);
			}
		}

		return { 0 };
	}

	GLenum attachment;
	switch (type)
	{
	case GL_COLOR:
	case GL_COLOR_BUFFER_BIT:
		attachment = GL_COLOR_ATTACHMENT0 + index;
		break;
	case GL_DEPTH:
	case GL_DEPTH_BUFFER_BIT:
		attachment = GL_DEPTH_ATTACHMENT;
		break;
	case GL_STENCIL:
	case GL_STENCIL_BUFFER_BIT:
		attachment = GL_STENCIL_ATTACHMENT;
		break;
	case GL_DEPTH_STENCIL:
	case GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT:
		// Only return the depth attachment in case there are different depth and stencil attachments, since calling 'glGetNamedFramebufferAttachmentParameteriv' with 'GL_DEPTH_STENCIL_ATTACHMENT' would fail in that case
		attachment = GL_DEPTH_ATTACHMENT;
		break;
	default:
		return { 0 };
	}

	GLenum target = GL_NONE, object = 0;
	if (_device_impl->_supports_dsa)
	{
		gl.GetNamedFramebufferAttachmentParameteriv(fbo_object, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, reinterpret_cast<GLint *>(&target));

		// Check if FBO does have this attachment
		if (target != GL_NONE)
		{
			gl.GetNamedFramebufferAttachmentParameteriv(fbo_object, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, reinterpret_cast<GLint *>(&object));

			// Get actual texture target from the texture object
			if (target == GL_TEXTURE)
				gl.GetTextureParameteriv(object, GL_TEXTURE_TARGET, reinterpret_cast<GLint *>(&target));
		}
	}
	else
	{
		GLuint prev_binding = 0;
		gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, reinterpret_cast<GLint *>(&prev_binding));
		// Must not bind framebuffer again if this was called from 'glBindFramebuffer' hook, or else black screen occurs in Jak Project for some reason
		if (fbo_object != prev_binding)
			gl.BindFramebuffer(GL_FRAMEBUFFER, fbo_object);

		gl.GetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, reinterpret_cast<GLint *>(&target));

		if (target != GL_NONE)
		{
			gl.GetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, reinterpret_cast<GLint *>(&object));

			if (target == GL_TEXTURE && gl.GetTextureParameteriv != nullptr)
				gl.GetTextureParameteriv(object, GL_TEXTURE_TARGET, reinterpret_cast<GLint *>(&target));
		}

		if (fbo_object != prev_binding)
			gl.BindFramebuffer(GL_FRAMEBUFFER, prev_binding);
	}

	if (target == GL_NONE)
		return { 0 };

	// TODO: Create view based on 'GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL', 'GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE' and 'GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER'
	return make_resource_view_handle(target, object);
}

bool reshade::opengl::device_impl::map_buffer_region(api::resource resource, uint64_t offset, uint64_t size, api::map_access access, void **out_data)
{
	if (out_data == nullptr)
		return false;

	assert(resource.handle != 0 && (resource.handle >> 40) == GL_BUFFER);
	assert(offset <= static_cast<uint64_t>(std::numeric_limits<GLintptr>::max()) && (size == UINT64_MAX || size <= static_cast<uint64_t>(std::numeric_limits<GLsizeiptr>::max())));

	const GLuint object = resource.handle & 0xFFFFFFFF;

	if (_supports_dsa)
	{
		if (UINT64_MAX == size)
		{
#ifndef _WIN64
			GLint max_size = 0;
			gl.GetNamedBufferParameteriv(object, GL_BUFFER_SIZE, &max_size);
#else
			GLint64 max_size = 0;
			gl.GetNamedBufferParameteri64v(object, GL_BUFFER_SIZE, &max_size);
#endif
			size = max_size;
		}

		*out_data = gl.MapNamedBufferRange(object, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), convert_access_flags(access));
	}
	else
	{
		GLuint prev_binding = 0;
		gl.GetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, reinterpret_cast<GLint *>(&prev_binding));
		gl.BindBuffer(GL_COPY_WRITE_BUFFER, object);

		if (UINT64_MAX == size)
		{
#ifndef _WIN64
			GLint max_size = 0;
			gl.GetBufferParameteriv(GL_COPY_WRITE_BUFFER, GL_BUFFER_SIZE, &max_size);
#else
			GLint64 max_size = 0;
			gl.GetBufferParameteri64v(GL_COPY_WRITE_BUFFER, GL_BUFFER_SIZE, &max_size);
#endif
			size = max_size;
		}

		*out_data = gl.MapBufferRange(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), convert_access_flags(access));

		gl.BindBuffer(GL_COPY_WRITE_BUFFER, prev_binding);
	}

	return *out_data != nullptr;
}
void reshade::opengl::device_impl::unmap_buffer_region(api::resource resource)
{
	assert(resource.handle != 0 && (resource.handle >> 40) == GL_BUFFER);

	const GLuint object = resource.handle & 0xFFFFFFFF;

	if (_supports_dsa)
	{
		gl.UnmapNamedBuffer(object);
	}
	else
	{
		GLuint prev_binding = 0;
		gl.GetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, reinterpret_cast<GLint *>(&prev_binding));
		gl.BindBuffer(GL_COPY_WRITE_BUFFER, object);

		gl.UnmapBuffer(GL_COPY_WRITE_BUFFER);

		gl.BindBuffer(GL_COPY_WRITE_BUFFER, prev_binding);
	}
}
bool reshade::opengl::device_impl::map_texture_region(api::resource resource, uint32_t subresource, const api::subresource_box *box, api::map_access access, api::subresource_data *out_data)
{
	if (out_data == nullptr)
		return false;

	out_data->data = nullptr;
	out_data->row_pitch = 0;
	out_data->slice_pitch = 0;

	assert(resource.handle != 0);

	size_t hash = 0;
	hash_combine(hash, resource.handle);
	hash_combine(hash, subresource);

	if (const auto it = _map_lookup.find(hash);
		it != _map_lookup.end())
		return false; // Cannot map a subresource that is already mapped

	const api::resource_desc desc = get_resource_desc(resource);

	const GLuint level = subresource % desc.texture.levels;
	      GLuint layer = subresource / desc.texture.levels;

	GLuint xoffset, yoffset, zoffset, width, height, depth;
	if (box != nullptr)
	{
		xoffset = box->left;
		yoffset = box->top;
		zoffset = box->front;
		width   = box->width();
		height  = box->height();
		depth   = box->depth();
	}
	else
	{
		xoffset = yoffset = zoffset = 0;
		width   = std::max(1u, desc.texture.width >> level);
		height  = std::max(1u, desc.texture.height >> level);
		depth   = (desc.type == api::resource_type::texture_3d ? std::max(1u, static_cast<uint32_t>(desc.texture.depth_or_layers) >> level) : 1u);
	}

	const auto row_pitch = api::format_row_pitch(desc.texture.format, width);
	const auto slice_pitch = api::format_slice_pitch(desc.texture.format, row_pitch, height);
	const auto total_image_size = depth * static_cast<size_t>(slice_pitch);

	uint8_t *const pixels = new uint8_t[total_image_size];

	out_data->data = pixels;
	out_data->row_pitch = row_pitch;
	out_data->slice_pitch = slice_pitch;

	_map_lookup.emplace(hash, map_info {
		*out_data, {
			static_cast<int32_t>(xoffset),
			static_cast<int32_t>(yoffset),
			static_cast<int32_t>(zoffset),
			static_cast<int32_t>(xoffset + width),
			static_cast<int32_t>(yoffset + height),
			static_cast<int32_t>(zoffset + depth)
		}, access });

	if (access == api::map_access::write_only || access == api::map_access::write_discard)
		return true;

	const GLenum target = resource.handle >> 40;
	const GLuint object = resource.handle & 0xFFFFFFFF;

	// Get current state
	GLint prev_pack_lsb = GL_FALSE;
	GLint prev_pack_swap = GL_FALSE;
	GLint prev_pack_alignment = 0;
	GLint prev_pack_row_length = 0;
	GLint prev_pack_image_height = 0;
	GLint prev_pack_skip_rows = 0;
	GLint prev_pack_skip_pixels = 0;
	GLint prev_pack_skip_images = 0;
	gl.GetIntegerv(GL_PACK_LSB_FIRST, &prev_pack_lsb);
	gl.GetIntegerv(GL_PACK_SWAP_BYTES, &prev_pack_swap);
	gl.GetIntegerv(GL_PACK_ALIGNMENT, &prev_pack_alignment);
	gl.GetIntegerv(GL_PACK_ROW_LENGTH, &prev_pack_row_length);
	gl.GetIntegerv(GL_PACK_IMAGE_HEIGHT, &prev_pack_image_height);
	gl.GetIntegerv(GL_PACK_SKIP_ROWS, &prev_pack_skip_rows);
	gl.GetIntegerv(GL_PACK_SKIP_PIXELS, &prev_pack_skip_pixels);
	gl.GetIntegerv(GL_PACK_SKIP_IMAGES, &prev_pack_skip_images);

	GLuint prev_pack_binding = 0;
	gl.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, reinterpret_cast<GLint *>(&prev_pack_binding));

	GLuint prev_binding = 0;
	gl.GetIntegerv(get_binding_for_target(target), reinterpret_cast<GLint *>(&prev_binding));

	// Unset any existing pack buffer so pointer is not interpreted as an offset
	gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);

	// Clear pixel storage modes to defaults (texture downloads can break otherwise)
	gl.PixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
	gl.PixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
	gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
	gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
	gl.PixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
	gl.PixelStorei(GL_PACK_SKIP_ROWS, 0);
	gl.PixelStorei(GL_PACK_SKIP_PIXELS, 0);
	gl.PixelStorei(GL_PACK_SKIP_IMAGES, 0);

	// Bind and download texture data
	gl.BindTexture(target, object);

	GLenum level_target = target;
	if (target == GL_TEXTURE_CUBE_MAP || target == GL_TEXTURE_CUBE_MAP_ARRAY)
	{
		const GLuint face = layer % 6;
		layer /= 6;
		level_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;
	}

	GLenum type, format = convert_upload_format(desc.texture.format, type);

	assert(total_image_size <= static_cast<size_t>(std::numeric_limits<GLsizei>::max()));

	if (box == nullptr)
	{
		assert(layer == 0);

		gl.GetTexImage(target == GL_TEXTURE_CUBE_MAP || target == GL_TEXTURE_CUBE_MAP_ARRAY ? level_target : target, level, format, type, pixels);
	}
	else if (_supports_dsa)
	{
		switch (target)
		{
		case GL_TEXTURE_1D_ARRAY:
			yoffset += layer;
			break;
		case GL_TEXTURE_CUBE_MAP:
		case GL_TEXTURE_CUBE_MAP_ARRAY:
		case GL_TEXTURE_2D_ARRAY:
			zoffset += layer;
			break;
		}

		gl.GetTextureSubImage(object, level, xoffset, yoffset, zoffset, width, height, depth, format, type, static_cast<GLsizei>(total_image_size), pixels);
	}

	// Restore previous state from application
	gl.BindTexture(target, prev_binding);

	gl.BindBuffer(GL_PIXEL_PACK_BUFFER, prev_pack_binding);

	gl.PixelStorei(GL_PACK_LSB_FIRST, prev_pack_lsb);
	gl.PixelStorei(GL_PACK_SWAP_BYTES, prev_pack_swap);
	gl.PixelStorei(GL_PACK_ALIGNMENT, prev_pack_alignment);
	gl.PixelStorei(GL_PACK_ROW_LENGTH, prev_pack_row_length);
	gl.PixelStorei(GL_PACK_IMAGE_HEIGHT, prev_pack_image_height);
	gl.PixelStorei(GL_PACK_SKIP_ROWS, prev_pack_skip_rows);
	gl.PixelStorei(GL_PACK_SKIP_PIXELS, prev_pack_skip_pixels);
	gl.PixelStorei(GL_PACK_SKIP_IMAGES, prev_pack_skip_images);

	return true;
}
void reshade::opengl::device_impl::unmap_texture_region(api::resource resource, uint32_t subresource)
{
	assert(resource.handle != 0);

	size_t hash = 0;
	hash_combine(hash, resource.handle);
	hash_combine(hash, subresource);

	if (const auto it = _map_lookup.find(hash);
		it != _map_lookup.end())
	{
		if (it->second.access != api::map_access::read_only)
			update_texture_region(it->second.data, resource, subresource, &it->second.box);

		delete[] static_cast<uint8_t *>(it->second.data.data);

		_map_lookup.erase(it);
	}
	else
	{
		assert(false);
	}
}

void reshade::opengl::device_impl::update_buffer_region(const void *data, api::resource resource, uint64_t offset, uint64_t size)
{
	assert(resource.handle != 0 && (resource.handle >> 40) == GL_BUFFER);
	assert(offset <= static_cast<uint64_t>(std::numeric_limits<GLintptr>::max()) && size <= static_cast<uint64_t>(std::numeric_limits<GLsizeiptr>::max()));

	const GLuint object = resource.handle & 0xFFFFFFFF;

	if (_supports_dsa)
	{
		gl.NamedBufferSubData(object, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data);
	}
	else
	{
		GLuint prev_binding = 0;
		gl.GetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, reinterpret_cast<GLint *>(&prev_binding));
		gl.BindBuffer(GL_COPY_WRITE_BUFFER, object);

		gl.BufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data);

		gl.BindBuffer(GL_COPY_WRITE_BUFFER, prev_binding);
	}
}
void reshade::opengl::device_impl::update_texture_region(const api::subresource_data &data, api::resource resource, uint32_t subresource, const api::subresource_box *box)
{
	assert(resource.handle != 0);

	const GLenum target = resource.handle >> 40;
	const GLuint object = resource.handle & 0xFFFFFFFF;

	// Get current state
	GLint prev_unpack_lsb = GL_FALSE;
	GLint prev_unpack_swap = GL_FALSE;
	GLint prev_unpack_alignment = 0;
	GLint prev_unpack_row_length = 0;
	GLint prev_unpack_image_height = 0;
	GLint prev_unpack_skip_rows = 0;
	GLint prev_unpack_skip_pixels = 0;
	GLint prev_unpack_skip_images = 0;
	gl.GetIntegerv(GL_UNPACK_LSB_FIRST, &prev_unpack_lsb);
	gl.GetIntegerv(GL_UNPACK_SWAP_BYTES, &prev_unpack_swap);
	gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &prev_unpack_alignment);
	gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_unpack_row_length);
	gl.GetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &prev_unpack_image_height);
	gl.GetIntegerv(GL_UNPACK_SKIP_ROWS, &prev_unpack_skip_rows);
	gl.GetIntegerv(GL_UNPACK_SKIP_PIXELS, &prev_unpack_skip_pixels);
	gl.GetIntegerv(GL_UNPACK_SKIP_IMAGES, &prev_unpack_skip_images);

	GLuint prev_unpack_binding = 0;
	gl.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, reinterpret_cast<GLint *>(&prev_unpack_binding));

	GLuint prev_binding = 0;
	gl.GetIntegerv(get_binding_for_target(target), reinterpret_cast<GLint *>(&prev_binding));

	// Unset any existing unpack buffer so pointer is not interpreted as an offset
	gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	// Clear pixel storage modes to defaults (texture uploads can break otherwise)
	gl.PixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
	gl.PixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
	gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	gl.PixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
	gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	gl.PixelStorei(GL_UNPACK_SKIP_IMAGES, 0);

	// Bind and upload texture data
	gl.BindTexture(target, object);

	const api::resource_desc desc = get_resource_desc(resource);

	const GLuint level = subresource % desc.texture.levels;
	      GLuint layer = subresource / desc.texture.levels;

	GLenum level_target = target;
	if (target == GL_TEXTURE_CUBE_MAP || target == GL_TEXTURE_CUBE_MAP_ARRAY)
	{
		const GLuint face = layer % 6;
		layer /= 6;
		level_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;
	}

	GLuint xoffset, yoffset, zoffset, width, height, depth;
	if (box != nullptr)
	{
		xoffset = box->left;
		yoffset = box->top;
		zoffset = box->front;
		width   = box->width();
		height  = box->height();
		depth   = box->depth();
	}
	else
	{
		xoffset = yoffset = zoffset = 0;
		width   = std::max(1u, desc.texture.width >> level);
		height  = std::max(1u, desc.texture.height >> level);
		depth   = (desc.type == api::resource_type::texture_3d ? std::max(1u, static_cast<uint32_t>(desc.texture.depth_or_layers) >> level) : 1u);
	}

	GLenum type, format = convert_upload_format(desc.texture.format, type);

	const auto row_pitch = api::format_row_pitch(desc.texture.format, width);
	const auto slice_pitch = api::format_slice_pitch(desc.texture.format, row_pitch, height);
	const auto total_image_size = depth * static_cast<size_t>(slice_pitch);

	assert(total_image_size <= static_cast<size_t>(std::numeric_limits<GLsizei>::max()));

	std::vector<uint8_t> temp_pixels;
	const uint8_t *pixels = static_cast<const uint8_t *>(data.data);

	if ((row_pitch != data.row_pitch && height == 1) ||
		(slice_pitch != data.slice_pitch && depth == 1))
	{
		temp_pixels.resize(total_image_size);
		uint8_t *dst_pixels = temp_pixels.data();

		for (size_t z = 0; z < depth; ++z)
			for (size_t y = 0; y < height; ++y, dst_pixels += row_pitch)
				std::memcpy(dst_pixels, pixels + z * data.slice_pitch + y * data.row_pitch, row_pitch);

		pixels = temp_pixels.data();
	}

	switch (level_target)
	{
	case GL_TEXTURE_1D:
		if (type != GL_COMPRESSED_TEXTURE_FORMATS)
			gl.TexSubImage1D(level_target, level, xoffset, width, format, type, pixels);
		else
			gl.CompressedTexSubImage1D(level_target, level, xoffset, width, format, static_cast<GLsizei>(total_image_size), pixels);
		break;
	case GL_TEXTURE_1D_ARRAY:
		yoffset += layer;
		[[fallthrough]];
	case GL_TEXTURE_2D:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
	case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
	case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
	case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
		if (type != GL_COMPRESSED_TEXTURE_FORMATS)
			gl.TexSubImage2D(level_target, level, xoffset, yoffset, width, height, format, type, pixels);
		else
			gl.CompressedTexSubImage2D(level_target, level, xoffset, yoffset, width, height, format, static_cast<GLsizei>(total_image_size), pixels);
		break;
	case GL_TEXTURE_2D_ARRAY:
		zoffset += layer;
		[[fallthrough]];
	case GL_TEXTURE_3D:
		if (type != GL_COMPRESSED_TEXTURE_FORMATS)
			gl.TexSubImage3D(level_target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
		else
			gl.CompressedTexSubImage3D(level_target, level, xoffset, yoffset, zoffset, width, height, depth, format, static_cast<GLsizei>(total_image_size), pixels);
		break;
	}

	// Restore previous state from application
	gl.BindTexture(target, prev_binding);

	gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, prev_unpack_binding);

	gl.PixelStorei(GL_UNPACK_LSB_FIRST, prev_unpack_lsb);
	gl.PixelStorei(GL_UNPACK_SWAP_BYTES, prev_unpack_swap);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, prev_unpack_alignment);
	gl.PixelStorei(GL_UNPACK_ROW_LENGTH, prev_unpack_row_length);
	gl.PixelStorei(GL_UNPACK_IMAGE_HEIGHT, prev_unpack_image_height);
	gl.PixelStorei(GL_UNPACK_SKIP_ROWS, prev_unpack_skip_rows);
	gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, prev_unpack_skip_pixels);
	gl.PixelStorei(GL_UNPACK_SKIP_IMAGES, prev_unpack_skip_images);
}

static bool create_shader_module(GLenum type, const reshade::api::shader_desc &desc, GLuint &shader_object)
{
	shader_object = gl.CreateShader(type);

	if (!(desc.code_size > 4 && *static_cast<const uint32_t *>(desc.code) == 0x07230203)) // Check for SPIR-V magic number
	{
		assert(desc.entry_point == nullptr || std::strcmp(desc.entry_point, "main") == 0);
		assert(desc.spec_constants == 0);

		const auto source = static_cast<const GLchar *>(desc.code);
		const auto source_len = static_cast<GLint>(desc.code_size);
		gl.ShaderSource(shader_object, 1, &source, &source_len);
		gl.CompileShader(shader_object);
	}
	else
	{
		assert(desc.code_size <= static_cast<size_t>(std::numeric_limits<GLsizei>::max()));

		gl.ShaderBinary(1, &shader_object, GL_SPIR_V_BINARY, desc.code, static_cast<GLsizei>(desc.code_size));
		gl.SpecializeShader(shader_object, desc.entry_point != nullptr ? desc.entry_point : "main", desc.spec_constants, desc.spec_constant_ids, desc.spec_constant_values);
	}

	GLint status = GL_FALSE;
	gl.GetShaderiv(shader_object, GL_COMPILE_STATUS, &status);

	if (GL_FALSE == status)
	{
		GLint log_size = 0;
		gl.GetShaderiv(shader_object, GL_INFO_LOG_LENGTH, &log_size);

		if (0 < log_size)
		{
			std::vector<char> log(log_size);
			gl.GetShaderInfoLog(shader_object, log_size, nullptr, log.data());

			LOG(ERROR) << "Failed to compile GLSL shader:\n" << log.data();
		}

		return false;
	}

	return true;
}

bool reshade::opengl::device_impl::create_pipeline(api::pipeline_layout, uint32_t subobject_count, const api::pipeline_subobject *subobjects, api::pipeline *out_handle)
{
	bool is_graphics_pipeline = true;
	std::vector<GLuint> shaders;

	api::pipeline_subobject input_layout_desc = {};
	api::blend_desc blend_desc = {};
	api::rasterizer_desc rasterizer_desc = {};
	api::depth_stencil_desc depth_stencil_desc = {};
	api::primitive_topology topology = api::primitive_topology::triangle_list;
	uint32_t sample_mask = UINT32_MAX;

	GLint status = GL_FALSE;
	const auto impl = new pipeline_impl();

	for (uint32_t i = 0; i < subobject_count; ++i)
	{
		if (subobjects[i].count == 0)
			continue;

		switch (subobjects[i].type)
		{
		case api::pipeline_subobject_type::vertex_shader:
			assert(subobjects[i].count == 1);
			if (static_cast<const api::shader_desc *>(subobjects[i].data)->code_size == 0)
				break;
			if (!create_shader_module(GL_VERTEX_SHADER, *static_cast<const api::shader_desc *>(subobjects[i].data), shaders.emplace_back()))
				goto exit_failure;
			break;
		case api::pipeline_subobject_type::hull_shader:
			assert(subobjects[i].count == 1);
			if (static_cast<const api::shader_desc *>(subobjects[i].data)->code_size == 0)
				break;
			if (!create_shader_module(GL_TESS_CONTROL_SHADER, *static_cast<const api::shader_desc *>(subobjects[i].data), shaders.emplace_back()))
				goto exit_failure;
			break;
		case api::pipeline_subobject_type::domain_shader:
			assert(subobjects[i].count == 1);
			if (static_cast<const api::shader_desc *>(subobjects[i].data)->code_size == 0)
				break;
			if (!create_shader_module(GL_TESS_EVALUATION_SHADER, *static_cast<const api::shader_desc *>(subobjects[i].data), shaders.emplace_back()))
				goto exit_failure;
			break;
		case api::pipeline_subobject_type::geometry_shader:
			assert(subobjects[i].count == 1);
			if (static_cast<const api::shader_desc *>(subobjects[i].data)->code_size == 0)
				break;
			if (!create_shader_module(GL_GEOMETRY_SHADER, *static_cast<const api::shader_desc *>(subobjects[i].data), shaders.emplace_back()))
				goto exit_failure;
			break;
		case api::pipeline_subobject_type::pixel_shader:
			assert(subobjects[i].count == 1);
			if (static_cast<const api::shader_desc *>(subobjects[i].data)->code_size == 0)
				break;
			if (!create_shader_module(GL_FRAGMENT_SHADER, *static_cast<const api::shader_desc *>(subobjects[i].data), shaders.emplace_back()))
				goto exit_failure;
			break;
		case api::pipeline_subobject_type::compute_shader:
			assert(subobjects[i].count == 1);
			if (static_cast<const api::shader_desc *>(subobjects[i].data)->code_size == 0)
				break;
			if (!create_shader_module(GL_COMPUTE_SHADER, *static_cast<const api::shader_desc *>(subobjects[i].data), shaders.emplace_back()))
				goto exit_failure;
			is_graphics_pipeline = false;
			break;
		case api::pipeline_subobject_type::input_layout:
			input_layout_desc = subobjects[i];
			break;
		case api::pipeline_subobject_type::stream_output_state:
			assert(subobjects[i].count == 1);
			goto exit_failure; // Not implemented
		case api::pipeline_subobject_type::blend_state:
			assert(subobjects[i].count == 1);
			blend_desc = *static_cast<const api::blend_desc *>(subobjects[i].data);
			break;
		case api::pipeline_subobject_type::rasterizer_state:
			assert(subobjects[i].count == 1);
			rasterizer_desc = *static_cast<const api::rasterizer_desc *>(subobjects[i].data);
			break;
		case api::pipeline_subobject_type::depth_stencil_state:
			assert(subobjects[i].count == 1);
			depth_stencil_desc = *static_cast<const api::depth_stencil_desc *>(subobjects[i].data);
			break;
		case api::pipeline_subobject_type::primitive_topology:
			assert(subobjects[i].count == 1);
			topology = *static_cast<const api::primitive_topology *>(subobjects[i].data);
			break;
		case api::pipeline_subobject_type::depth_stencil_format:
		case api::pipeline_subobject_type::render_target_formats:
			break; // Ignored
		case api::pipeline_subobject_type::sample_mask:
			assert(subobjects[i].count == 1);
			sample_mask = *static_cast<const uint32_t *>(subobjects[i].data);
			break;
		case api::pipeline_subobject_type::sample_count:
		case api::pipeline_subobject_type::viewport_count:
			assert(subobjects[i].count == 1);
			break;
		case api::pipeline_subobject_type::dynamic_pipeline_states:
			break; // Ignored
		case api::pipeline_subobject_type::max_vertex_count:
			assert(subobjects[i].count == 1);
			break; // Ignored
		default:
			assert(false);
			goto exit_failure;
		}
	}

	impl->program = gl.CreateProgram();

	for (const GLuint shader : shaders)
	{
		gl.AttachShader(impl->program, shader);
	}

	gl.LinkProgram(impl->program);

	for (const GLuint shader : shaders)
	{
		gl.DetachShader(impl->program, shader);
		gl.DeleteShader(shader);
	}

	shaders.clear();

	gl.GetProgramiv(impl->program, GL_LINK_STATUS, &status);

	if (GL_FALSE == status)
	{
		GLint log_size = 0;
		gl.GetProgramiv(impl->program, GL_INFO_LOG_LENGTH, &log_size);

		if (0 < log_size)
		{
			std::vector<char> log(log_size);
			gl.GetProgramInfoLog(impl->program, log_size, nullptr, log.data());

			LOG(ERROR) << "Failed to link GLSL program:\n" << log.data();
		}

		gl.DeleteProgram(impl->program);
		goto exit_failure;
	}

	// There always has to be a VAO for graphics pipelines, even if it is empty
	if (is_graphics_pipeline || input_layout_desc.count != 0)
	{
		gl.GenVertexArrays(1, &impl->vao);

		impl->input_elements.assign(
			static_cast<const api::input_element *>(input_layout_desc.data),
			static_cast<const api::input_element *>(input_layout_desc.data) + input_layout_desc.count);
	}
	else
	{
		impl->vao = 0;
	}

	impl->sample_alpha_to_coverage = blend_desc.alpha_to_coverage_enable;
	impl->logic_op_enable = blend_desc.logic_op_enable[0]; // Logic operation applies to all attachments
	impl->logic_op = convert_logic_op(blend_desc.logic_op[0]);
	std::copy_n(blend_desc.blend_constant, 4, impl->blend_constant);
	for (int i = 0; i < 8; ++i)
	{
		impl->blend_enable[i] = blend_desc.blend_enable[i];
		impl->blend_src[i] = convert_blend_factor(blend_desc.source_color_blend_factor[i]);
		impl->blend_dst[i] = convert_blend_factor(blend_desc.dest_color_blend_factor[i]);
		impl->blend_src_alpha[i] = convert_blend_factor(blend_desc.source_alpha_blend_factor[i]);
		impl->blend_dst_alpha[i] = convert_blend_factor(blend_desc.dest_alpha_blend_factor[i]);
		impl->blend_eq[i] = convert_blend_op(blend_desc.color_blend_op[i]);
		impl->blend_eq_alpha[i] = convert_blend_op(blend_desc.alpha_blend_op[i]);
		impl->color_write_mask[i][0] = (blend_desc.render_target_write_mask[i] & (1 << 0)) != 0;
		impl->color_write_mask[i][1] = (blend_desc.render_target_write_mask[i] & (1 << 1)) != 0;
		impl->color_write_mask[i][2] = (blend_desc.render_target_write_mask[i] & (1 << 2)) != 0;
		impl->color_write_mask[i][3] = (blend_desc.render_target_write_mask[i] & (1 << 3)) != 0;
	}

	impl->polygon_mode = convert_fill_mode(rasterizer_desc.fill_mode);
	impl->cull_mode = convert_cull_mode(rasterizer_desc.cull_mode);
	impl->front_face = rasterizer_desc.front_counter_clockwise ? GL_CCW : GL_CW;
	impl->depth_clamp = !rasterizer_desc.depth_clip_enable;
	impl->scissor_test = rasterizer_desc.scissor_enable;
	impl->multisample_enable = rasterizer_desc.multisample_enable;
	impl->line_smooth_enable = rasterizer_desc.antialiased_line_enable;

	// Polygon offset is not currently implemented
	assert(rasterizer_desc.depth_bias == 0 && rasterizer_desc.depth_bias_clamp == 0 && rasterizer_desc.slope_scaled_depth_bias == 0);

	impl->depth_test = depth_stencil_desc.depth_enable;
	impl->depth_mask = depth_stencil_desc.depth_write_mask;
	impl->depth_func = convert_compare_op(depth_stencil_desc.depth_func);
	impl->stencil_test = depth_stencil_desc.stencil_enable;
	impl->stencil_read_mask = depth_stencil_desc.stencil_read_mask;
	impl->stencil_write_mask = depth_stencil_desc.stencil_write_mask;
	impl->stencil_reference_value = static_cast<GLint>(depth_stencil_desc.stencil_reference_value);
	impl->front_stencil_op_fail = convert_stencil_op(depth_stencil_desc.front_stencil_fail_op);
	impl->front_stencil_op_depth_fail = convert_stencil_op(depth_stencil_desc.front_stencil_depth_fail_op);
	impl->front_stencil_op_pass = convert_stencil_op(depth_stencil_desc.front_stencil_pass_op);
	impl->front_stencil_func = convert_compare_op(depth_stencil_desc.front_stencil_func);
	impl->back_stencil_op_fail = convert_stencil_op(depth_stencil_desc.back_stencil_fail_op);
	impl->back_stencil_op_depth_fail = convert_stencil_op(depth_stencil_desc.back_stencil_depth_fail_op);
	impl->back_stencil_op_pass = convert_stencil_op(depth_stencil_desc.back_stencil_pass_op);
	impl->back_stencil_func = convert_compare_op(depth_stencil_desc.back_stencil_func);

	impl->sample_mask = sample_mask;
	impl->prim_mode = convert_primitive_topology(topology);
	impl->patch_vertices = impl->prim_mode == GL_PATCHES ? static_cast<uint32_t>(topology) - static_cast<uint32_t>(api::primitive_topology::patch_list_01_cp) : 0;

	*out_handle = { reinterpret_cast<uintptr_t>(impl) };
	return true;

exit_failure:
	for (const GLuint shader : shaders)
	{
		gl.DeleteShader(shader);
	}

	delete impl;
	*out_handle = { 0 };
	return false;
}
void reshade::opengl::device_impl::destroy_pipeline(api::pipeline handle)
{
	if (handle.handle == 0)
		return;

	// It is not allowed to destroy application pipeline handles
	assert((handle.handle >> 40) != GL_PROGRAM);

	const auto impl = reinterpret_cast<pipeline_impl *>(handle.handle);

	gl.DeleteProgram(impl->program);
	gl.DeleteVertexArrays(1, &impl->vao);

	delete impl;
}

bool reshade::opengl::device_impl::create_pipeline_layout(uint32_t param_count, const api::pipeline_layout_param *params, api::pipeline_layout *out_handle)
{
	*out_handle = { 0 };

	std::vector<api::descriptor_range> ranges(param_count);

	for (uint32_t i = 0; i < param_count; ++i)
	{
		api::descriptor_range &merged_range = ranges[i];

		switch (params[i].type)
		{
		case api::pipeline_layout_param_type::descriptor_set:
		case api::pipeline_layout_param_type::push_descriptors_ranges:
			if (params[i].descriptor_set.count == 0)
				return false;

			merged_range = params[i].descriptor_set.ranges[0];
			if (merged_range.array_size > 1)
				return false;

			for (uint32_t k = 1; k < params[i].descriptor_set.count; ++i)
			{
				const api::descriptor_range &range = params[i].descriptor_set.ranges[k];

				if (range.type != merged_range.type || range.array_size > 1 || range.dx_register_space != merged_range.dx_register_space)
					return false;

				if (range.binding >= merged_range.binding)
				{
					const uint32_t distance = range.binding - merged_range.binding;

					merged_range.count += distance;
					merged_range.visibility |= range.visibility;
				}
				else
				{
					const uint32_t distance = merged_range.binding - range.binding;

					merged_range.binding = range.binding;
					merged_range.dx_register_index = range.dx_register_index;
					merged_range.count += distance;
					merged_range.visibility |= range.visibility;
				}
			}
			break;
		case api::pipeline_layout_param_type::push_descriptors:
			merged_range = params[i].push_descriptors;
			break;
		case api::pipeline_layout_param_type::push_constants:
			merged_range.binding = params[i].push_constants.binding;
			break;
		}
	}

	const auto impl = new pipeline_layout_impl();
	impl->ranges = std::move(ranges);

	*out_handle = { reinterpret_cast<uintptr_t>(impl) };
	return true;
}
void reshade::opengl::device_impl::destroy_pipeline_layout(api::pipeline_layout handle)
{
	assert(handle != global_pipeline_layout);

	delete reinterpret_cast<pipeline_layout_impl *>(handle.handle);
}

bool reshade::opengl::device_impl::allocate_descriptor_sets(uint32_t count, api::pipeline_layout layout, uint32_t layout_param, api::descriptor_set *out_sets)
{
	const auto layout_impl = reinterpret_cast<const pipeline_layout_impl *>(layout.handle);

	if (layout_impl != nullptr)
	{
		for (uint32_t i = 0; i < count; ++i)
		{
			const auto set_impl = new descriptor_set_impl();
			set_impl->type = layout_impl->ranges[layout_param].type;
			set_impl->count = layout_impl->ranges[layout_param].count;

			switch (set_impl->type)
			{
			case api::descriptor_type::sampler:
			case api::descriptor_type::shader_resource_view:
			case api::descriptor_type::unordered_access_view:
				set_impl->descriptors.resize(set_impl->count * 1);
				break;
			case api::descriptor_type::sampler_with_resource_view:
				set_impl->descriptors.resize(set_impl->count * 2);
				break;
			case api::descriptor_type::constant_buffer:
			case api::descriptor_type::shader_storage_buffer:
				set_impl->descriptors.resize(set_impl->count * 3);
				break;
			default:
				assert(false);
				break;
			}

			out_sets[i] = { reinterpret_cast<uintptr_t>(set_impl) };
		}

		return true;
	}
	else
	{
		for (uint32_t i = 0; i < count; ++i)
		{
			out_sets[i] = { 0 };
		}

		return false;
	}
}
void reshade::opengl::device_impl::free_descriptor_sets(uint32_t count, const api::descriptor_set *sets)
{
	for (uint32_t i = 0; i < count; ++i)
		delete reinterpret_cast<descriptor_set_impl *>(sets[i].handle);
}

void reshade::opengl::device_impl::get_descriptor_pool_offset(api::descriptor_set set, uint32_t binding, uint32_t array_offset, api::descriptor_pool *pool, uint32_t *offset) const
{
	assert(set.handle != 0 && array_offset == 0);

	*pool = { 0 }; // Not implemented
	*offset = binding;
}

void reshade::opengl::device_impl::copy_descriptor_sets(uint32_t count, const api::descriptor_set_copy *copies)
{
	for (uint32_t i = 0; i < count; ++i)
	{
		const api::descriptor_set_copy &copy = copies[i];

		assert(copy.dest_array_offset == 0 && copy.source_array_offset == 0);

		const auto src_set_impl = reinterpret_cast<descriptor_set_impl *>(copy.source_set.handle);
		const auto dst_set_impl = reinterpret_cast<descriptor_set_impl *>(copy.dest_set.handle);

		assert(src_set_impl != nullptr && dst_set_impl != nullptr && src_set_impl->type == dst_set_impl->type);

		switch (src_set_impl->type)
		{
		case api::descriptor_type::sampler:
		case api::descriptor_type::shader_resource_view:
		case api::descriptor_type::unordered_access_view:
			std::memcpy(&dst_set_impl->descriptors[copy.dest_binding * 1], &src_set_impl->descriptors[copy.source_binding * 1], copy.count * sizeof(uint64_t) * 1);
			break;
		case api::descriptor_type::sampler_with_resource_view:
			std::memcpy(&dst_set_impl->descriptors[copy.dest_binding * 2], &src_set_impl->descriptors[copy.source_binding * 2], copy.count * sizeof(uint64_t) * 2);
			break;
		case api::descriptor_type::constant_buffer:
			std::memcpy(&dst_set_impl->descriptors[copy.dest_binding * 3], &src_set_impl->descriptors[copy.source_binding * 3], copy.count * sizeof(uint64_t) * 3);
			break;
		default:
			assert(false);
			break;
		}
	}
}
void reshade::opengl::device_impl::update_descriptor_sets(uint32_t count, const api::descriptor_set_update *updates)
{
	for (uint32_t i = 0; i < count; ++i)
	{
		const api::descriptor_set_update &update = updates[i];

		const auto set_impl = reinterpret_cast<descriptor_set_impl *>(update.set.handle);

		assert(set_impl != nullptr && set_impl->type == update.type);

		// In GLSL targeting OpenGL, if the binding qualifier is used with an array, the first element of the array takes the specified block binding and each subsequent element takes the next consecutive binding point
		// See https://www.khronos.org/registry/OpenGL/specs/gl/GLSLangSpec.4.60.html#layout-qualifiers (chapter 4.4.6)
		// Therefore it is not valid to specify an array offset for a binding
		assert(update.array_offset == 0);

		switch (update.type)
		{
		case api::descriptor_type::sampler:
		case api::descriptor_type::shader_resource_view:
		case api::descriptor_type::unordered_access_view:
			std::memcpy(&set_impl->descriptors[update.binding * 1], update.descriptors, update.count * sizeof(uint64_t) * 1);
			break;
		case api::descriptor_type::sampler_with_resource_view:
			std::memcpy(&set_impl->descriptors[update.binding * 2], update.descriptors, update.count * sizeof(uint64_t) * 2);
			break;
		case api::descriptor_type::constant_buffer:
			std::memcpy(&set_impl->descriptors[update.binding * 3], update.descriptors, update.count * sizeof(uint64_t) * 3);
			break;
		default:
			assert(false);
			break;
		}
	}
}

bool reshade::opengl::device_impl::create_query_pool(api::query_type type, uint32_t size, api::query_pool *out_handle)
{
	*out_handle = { 0 };

	if (type == api::query_type::pipeline_statistics)
		return false;

	const auto impl = new query_pool_impl();
	impl->queries.resize(size);

	gl.GenQueries(static_cast<GLsizei>(size), impl->queries.data());

	const GLenum target = convert_query_type(type);

	// Actually create and associate query objects with the names generated by 'glGenQueries' above
	for (uint32_t i = 0; i < size; ++i)
	{
		if (type == api::query_type::timestamp)
		{
			gl.QueryCounter(impl->queries[i], GL_TIMESTAMP);
		}
		else
		{
			gl.BeginQuery(target, impl->queries[i]);
			gl.EndQuery(target);
		}
	}

	*out_handle = { reinterpret_cast<uintptr_t>(impl) };
	return true;
}
void reshade::opengl::device_impl::destroy_query_pool(api::query_pool handle)
{
	if (handle.handle == 0)
		return;

	const auto impl = reinterpret_cast<query_pool_impl *>(handle.handle);

	gl.DeleteQueries(static_cast<GLsizei>(impl->queries.size()), impl->queries.data());

	delete impl;
}

bool reshade::opengl::device_impl::get_query_pool_results(api::query_pool pool, uint32_t first, uint32_t count, void *results, uint32_t stride)
{
	assert(pool.handle != 0);
	assert(stride >= sizeof(uint64_t));

	const auto impl = reinterpret_cast<query_pool_impl *>(pool.handle);

	for (size_t i = 0; i < count; ++i)
	{
		const GLuint query_object = impl->queries[first + i];

		GLuint available = GL_FALSE;
		gl.GetQueryObjectuiv(query_object, GL_QUERY_RESULT_AVAILABLE, &available);
		if (!available)
			return false;

		gl.GetQueryObjectui64v(query_object, GL_QUERY_RESULT, reinterpret_cast<GLuint64 *>(static_cast<uint8_t *>(results) + i * stride));
	}

	return true;
}

void reshade::opengl::device_impl::set_resource_name(api::resource handle, const char *name)
{
	gl.ObjectLabel((handle.handle >> 40) == GL_BUFFER ? GL_BUFFER : GL_TEXTURE, handle.handle & 0xFFFFFFFF, -1, name);
}
void reshade::opengl::device_impl::set_resource_view_name(api::resource_view handle, const char *name)
{
	if (((handle.handle >> 32) & 0x1) == 0)
		return;

	gl.ObjectLabel(GL_TEXTURE, handle.handle & 0xFFFFFFFF, -1, name);
}
