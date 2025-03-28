/*
 * Copyright (C) 2021 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "d3d9_impl_device.hpp"
#include "d3d9_impl_type_convert.hpp"
#include "d3d9_resource_call_vtable.inl"
#include "dll_log.hpp"
#include <algorithm>

const RECT *convert_box_to_rect(const reshade::api::subresource_box *box, RECT &rect)
{
	if (box == nullptr)
		return nullptr;

	rect.left = box->left;
	rect.top = box->top;
	assert(box->front == 0);
	rect.right = box->right;
	rect.bottom = box->bottom;
	assert(box->back == 1);

	return &rect;
}

static inline bool convert_format_internal(reshade::api::format format, D3DFORMAT &internal_format)
{
	if (format == reshade::api::format::r8_typeless || format == reshade::api::format::r8_unorm ||
		format == reshade::api::format::r8g8_typeless || format == reshade::api::format::r8g8_unorm)
	{
		// Use 4-component format, so that unused components are returned as zero and alpha as one (to match behavior from other APIs)
		internal_format = D3DFMT_X8R8G8B8;
		return true;
	}
	if (format == reshade::api::format::r8g8b8a8_typeless || format == reshade::api::format::r8g8b8a8_unorm || format == reshade::api::format::r8g8b8a8_unorm_srgb)
	{
		// Use 'D3DFMT_A8R8G8B8' instead of 'D3DFMT_A8B8G8R8', since the later is not supported well
		// This has to be mitigated in 'update_texture_region' by flipping RGBA to BGRA
		internal_format = D3DFMT_A8R8G8B8;
		return true;
	}

	return internal_format != D3DFMT_UNKNOWN;
}

reshade::d3d9::device_impl::device_impl(IDirect3DDevice9 *device) :
	api_object_impl(device), _caps(), _cp(), _backup_state(device)
{
	_orig->GetDirect3D(&_d3d);
	_orig->GetDeviceCaps(&_caps);
	_orig->GetCreationParameters(&_cp);

	// Maximum simultaneous number of render targets is typically 4 in D3D9
	_caps.NumSimultaneousRTs = std::min(_caps.NumSimultaneousRTs, 8ul);

#if RESHADE_ADDON
	load_addons();
#endif

	on_init();
}
reshade::d3d9::device_impl::~device_impl()
{
	on_reset();

#if RESHADE_ADDON
	unload_addons();
#endif
}

void reshade::d3d9::device_impl::on_init()
{
	// Create state block used for resource copying
	HRESULT hr = _orig->BeginStateBlock();
	if (SUCCEEDED(hr))
	{
		_orig->SetPixelShader(nullptr);
		_orig->SetVertexShader(nullptr);
		_orig->SetRenderState(D3DRS_ZENABLE, FALSE);
		_orig->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
		_orig->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		_orig->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		_orig->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		_orig->SetRenderState(D3DRS_FOGENABLE, FALSE);
		_orig->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		_orig->SetRenderState(D3DRS_CLIPPING, FALSE);
		_orig->SetRenderState(D3DRS_LIGHTING, FALSE);
		_orig->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
		_orig->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
		_orig->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
		_orig->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		_orig->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
		_orig->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		_orig->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		_orig->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		_orig->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		_orig->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
		_orig->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		_orig->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		_orig->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		_orig->SetSamplerState(0, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
		_orig->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		_orig->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		_orig->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		_orig->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);
		_orig->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
		_orig->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

		const D3DMATRIX identity_matrix = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f
		};
		_orig->SetTransform(D3DTS_VIEW, &identity_matrix);
		_orig->SetTransform(D3DTS_PROJECTION, &identity_matrix);
		_orig->SetTransform(D3DTS_WORLD, &identity_matrix);

		// For some reason rendering below water acts up in Source Engine games if the active clip plane is not cleared to zero before executing any draw calls ...
		// Also copying with a fullscreen triangle rather than a quad of two triangles solves some artifacts that otherwise occur in the second triangle there as well. Not sure what is going on ...
		const float zero_clip_plane[4] = { 0, 0, 0, 0 };
		_orig->SetClipPlane(0, zero_clip_plane);

		// Drawing the texture fails sometimes in Star Wars: Republic Commando if the unused stages are not unset
		for (DWORD i = 1; i < _caps.MaxSimultaneousTextures; ++i)
			_orig->SetTexture(i, nullptr);

		hr = _orig->EndStateBlock(&_copy_state);
	}

	if (FAILED(hr))
	{
		LOG(ERROR) << "Failed to create copy pipeline!";
	}

#if RESHADE_ADDON
	invoke_addon_event<addon_event::init_device>(this);

	const api::pipeline_layout_param global_pipeline_layout_params[8] = {
		/* s# */ api::descriptor_range { 0, 0, 0, 4, api::shader_stage::vertex, 1, api::descriptor_type::sampler_with_resource_view }, // Vertex shaders only support 4 sampler slots (D3DVERTEXTEXTURESAMPLER0 - D3DVERTEXTEXTURESAMPLER3)
		/* s# */ api::descriptor_range { 0, 0, 0, _caps.MaxSimultaneousTextures, api::shader_stage::pixel, 1, api::descriptor_type::sampler_with_resource_view },
		// See https://docs.microsoft.com/windows/win32/direct3dhlsl/dx9-graphics-reference-asm-vs-registers-vs-3-0
		/* vs_3_0 c# */ api::constant_range { 0, 0, 0, _caps.MaxVertexShaderConst * 4, api::shader_stage::vertex },
		/* vs_3_0 i# */ api::constant_range { 0, 0, 0, 16 * 4, api::shader_stage::vertex },
		/* vs_3_0 b# */ api::constant_range { 0, 0, 0, 16 * 1, api::shader_stage::vertex },
		// See https://docs.microsoft.com/windows/win32/direct3dhlsl/dx9-graphics-reference-asm-ps-registers-ps-3-0
		/* ps_3_0 c# */ api::constant_range { 0, 0, 0, 224 * 4, api::shader_stage::pixel },
		/* ps_3_0 i# */ api::constant_range { 0, 0, 0,  16 * 4, api::shader_stage::pixel },
		/* ps_3_0 b# */ api::constant_range { 0, 0, 0,  16 * 1, api::shader_stage::pixel },
	};
	invoke_addon_event<addon_event::init_pipeline_layout>(this, static_cast<uint32_t>(std::size(global_pipeline_layout_params)), global_pipeline_layout_params, global_pipeline_layout);

	invoke_addon_event<addon_event::init_command_list>(this);
	invoke_addon_event<addon_event::init_command_queue>(this);
#endif
}
void reshade::d3d9::device_impl::on_reset()
{
#if RESHADE_ADDON
	// Force add-ons to release all resources associated with this device before performing reset
	invoke_addon_event<addon_event::destroy_command_queue>(this);
	invoke_addon_event<addon_event::destroy_command_list>(this);

	for (DWORD i = 0; i < _caps.MaxSimultaneousTextures; ++i)
		_orig->SetTexture(i, nullptr);
	for (DWORD i = 0; i < _caps.MaxStreams; ++i)
		_orig->SetStreamSource(0, nullptr, 0, 0);
	_orig->SetIndices(nullptr);

	for (DWORD i = 0; i < _caps.NumSimultaneousRTs; ++i)
		_orig->SetRenderTarget(i, nullptr);
	// Release reference to the potentially replaced auto depth-stencil resource
	_orig->SetDepthStencilSurface(nullptr);

	invoke_addon_event<addon_event::destroy_pipeline_layout>(this, global_pipeline_layout);

	invoke_addon_event<addon_event::destroy_device>(this);
#endif

	_copy_state.reset();
	_default_input_stream.reset();
	_default_input_layout.reset();
}

bool reshade::d3d9::device_impl::check_capability(api::device_caps capability) const
{
	switch (capability)
	{
	case api::device_caps::compute_shader:
	case api::device_caps::geometry_shader:
	case api::device_caps::hull_and_domain_shader:
	case api::device_caps::logic_op:
	case api::device_caps::dual_source_blend:
	case api::device_caps::independent_blend: // There is 'D3DPMISCCAPS_INDEPENDENTWRITEMASKS', but it only applies to the render target write mask
		return false;
	case api::device_caps::fill_mode_non_solid:
		return true;
	case api::device_caps::conservative_rasterization:
		return false;
	case api::device_caps::bind_render_targets_and_depth_stencil:
		return true;
	case api::device_caps::multi_viewport:
		return false;
	case api::device_caps::partial_push_constant_updates:
	case api::device_caps::partial_push_descriptor_updates:
		return true;
	case api::device_caps::draw_instanced:
	case api::device_caps::draw_or_dispatch_indirect:
	case api::device_caps::copy_buffer_region:
	case api::device_caps::copy_buffer_to_texture:
		return false;
	case api::device_caps::blit:
	case api::device_caps::resolve_region:
		return true;
	case api::device_caps::copy_query_pool_results:
	case api::device_caps::sampler_compare:
		return false;
	case api::device_caps::sampler_anisotropic:
	case api::device_caps::sampler_with_resource_view:
	case api::device_caps::shared_resource:
		return true;
	case api::device_caps::shared_resource_nt_handle:
	default:
		return false;
	}
}
bool reshade::d3d9::device_impl::check_format_support(api::format format, api::resource_usage usage) const
{
	if ((usage & api::resource_usage::unordered_access) != 0)
		return false;

	DWORD d3d_usage = 0;
	convert_resource_usage_to_d3d_usage(usage, d3d_usage);

	const D3DFORMAT d3d_format = convert_format(format);
	return d3d_format != D3DFMT_UNKNOWN && SUCCEEDED(_d3d->CheckDeviceFormat(_cp.AdapterOrdinal, _cp.DeviceType, D3DFMT_X8R8G8B8, d3d_usage, D3DRTYPE_TEXTURE, d3d_format));
}

bool reshade::d3d9::device_impl::create_sampler(const api::sampler_desc &desc, api::sampler *out_handle)
{
	// Comparison sampling is not supported in D3D9
	if ((static_cast<uint32_t>(desc.filter) & 0x80) != 0)
	{
		*out_handle = { 0 };
		return false;
	}

	const auto impl = new sampler_impl();
	impl->state[D3DSAMP_ADDRESSU] = static_cast<DWORD>(desc.address_u);
	impl->state[D3DSAMP_ADDRESSV] = static_cast<DWORD>(desc.address_v);
	impl->state[D3DSAMP_ADDRESSW] = static_cast<DWORD>(desc.address_w);
	impl->state[D3DSAMP_BORDERCOLOR] = D3DCOLOR_COLORVALUE(desc.border_color[0], desc.border_color[1], desc.border_color[2], desc.border_color[3]);
	impl->state[D3DSAMP_MAGFILTER] = ((static_cast<DWORD>(desc.filter) & 0x0C) >> 2) + 1;
	impl->state[D3DSAMP_MINFILTER] = ((static_cast<DWORD>(desc.filter) & 0x30) >> 4) + 1;
	impl->state[D3DSAMP_MIPFILTER] = ((static_cast<DWORD>(desc.filter) & 0x03)     ) + 1;
	impl->state[D3DSAMP_MIPMAPLODBIAS] = *reinterpret_cast<const DWORD *>(&desc.mip_lod_bias);
	impl->state[D3DSAMP_MAXMIPLEVEL] = desc.min_lod > 0 ? static_cast<DWORD>(desc.min_lod) : 0;
	impl->state[D3DSAMP_MAXANISOTROPY] = static_cast<DWORD>(desc.max_anisotropy);

	if (desc.filter == api::filter_mode::anisotropic)
	{
		impl->state[D3DSAMP_MINFILTER] = D3DTEXF_ANISOTROPIC;
		impl->state[D3DSAMP_MAGFILTER] = D3DTEXF_ANISOTROPIC;
	}

	*out_handle = { reinterpret_cast<uintptr_t>(impl) };
	return true;
}
void reshade::d3d9::device_impl::destroy_sampler(api::sampler handle)
{
	delete reinterpret_cast<sampler_impl *>(handle.handle);
}

bool reshade::d3d9::device_impl::create_resource(const api::resource_desc &desc, const api::subresource_data *initial_data, api::resource_usage, api::resource *out_handle, HANDLE *shared_handle)
{
	*out_handle = { 0 };

	const bool is_shared = (desc.flags & api::resource_flags::shared) != 0;
	if (is_shared)
	{
		// NT handles are not supported
		if (shared_handle == nullptr || (desc.flags & reshade::api::resource_flags::shared_nt_handle) != 0)
			return false;
	}

	switch (desc.type)
	{
		case api::resource_type::buffer:
		{
			// Direct3D 9 only supports separate vertex and index buffers
			switch (desc.usage & (api::resource_usage::vertex_buffer | api::resource_usage::index_buffer | api::resource_usage::constant_buffer))
			{
				case api::resource_usage::index_buffer:
				{
					D3DINDEXBUFFER_DESC internal_desc = {};
					convert_resource_desc(desc, internal_desc);
					internal_desc.Format = D3DFMT_INDEX16; // TODO: The index format of the index buffer is hardcoded here, which is rather unfortunate ...

					if (com_ptr<IDirect3DIndexBuffer9> object;
						SUCCEEDED(_orig->CreateIndexBuffer(internal_desc.Size, internal_desc.Usage, internal_desc.Format, internal_desc.Pool, &object, shared_handle)))
					{
						*out_handle = to_handle(object.release());

						if (initial_data != nullptr)
						{
							update_buffer_region(initial_data->data, *out_handle, 0, desc.buffer.size);
						}
						return true;
					}
					break;
				}
				case api::resource_usage::vertex_buffer:
				{
					D3DVERTEXBUFFER_DESC internal_desc = {};
					convert_resource_desc(desc, internal_desc);

					if (com_ptr<IDirect3DVertexBuffer9> object;
						SUCCEEDED(_orig->CreateVertexBuffer(internal_desc.Size, internal_desc.Usage, internal_desc.FVF, internal_desc.Pool, &object, shared_handle)))
					{
						*out_handle = to_handle(object.release());

						if (initial_data != nullptr)
						{
							update_buffer_region(initial_data->data, *out_handle, 0, desc.buffer.size);
						}
						return true;
					}
					break;
				}
			}
			break;
		}
		case api::resource_type::texture_1d:
		case api::resource_type::texture_2d:
		{
			// Array or multisample textures are not supported in Direct3D 9
			if ((desc.texture.depth_or_layers != 1 && (desc.flags & api::resource_flags::cube_compatible) == 0) || desc.texture.samples != 1)
				break;

			UINT levels = 0;
			D3DSURFACE_DESC internal_desc = {};
			convert_resource_desc(desc, internal_desc, &levels, nullptr, _caps);

			if (!convert_format_internal(desc.texture.format, internal_desc.Format))
				break;

			if ((desc.flags & api::resource_flags::cube_compatible) == 0)
			{
				if (com_ptr<IDirect3DTexture9> object;
					SUCCEEDED(_orig->CreateTexture(internal_desc.Width, internal_desc.Height, levels, internal_desc.Usage, internal_desc.Format, internal_desc.Pool, &object, shared_handle)))
				{
					*out_handle = to_handle(object.release());

					if (initial_data != nullptr)
					{
						for (uint32_t subresource = 0; subresource < desc.texture.levels; ++subresource)
							update_texture_region(initial_data[subresource], *out_handle, subresource, nullptr);
					}
					return true;
				}
			}
			else
			{
				assert(internal_desc.Width == internal_desc.Height);

				if (com_ptr<IDirect3DCubeTexture9> object;
					SUCCEEDED(_orig->CreateCubeTexture(internal_desc.Width, levels, internal_desc.Usage, internal_desc.Format, internal_desc.Pool, &object, shared_handle)))
				{
					*out_handle = to_handle(object.release());

					if (initial_data != nullptr)
					{
						for (uint32_t subresource = 0; subresource < static_cast<uint32_t>(desc.texture.depth_or_layers) * desc.texture.levels; ++subresource)
							update_texture_region(initial_data[subresource], *out_handle, subresource, nullptr);
					}
					return true;
				}
			}
			break;
		}
		case api::resource_type::texture_3d:
		{
			// 3D textures can never have multisampling
			if (desc.texture.samples != 1)
				break;

			UINT levels = 0;
			D3DVOLUME_DESC internal_desc = {};
			convert_resource_desc(desc, internal_desc, &levels, _caps);

			if (!convert_format_internal(desc.texture.format, internal_desc.Format))
				break;

			if (com_ptr<IDirect3DVolumeTexture9> object;
				SUCCEEDED(_orig->CreateVolumeTexture(internal_desc.Width, internal_desc.Height, internal_desc.Depth, levels, internal_desc.Usage, internal_desc.Format, internal_desc.Pool, &object, shared_handle)))
			{
				*out_handle = to_handle(object.release());

				if (initial_data != nullptr)
				{
					for (uint32_t subresource = 0; subresource < static_cast<uint32_t>(desc.texture.depth_or_layers) * desc.texture.levels; ++subresource)
						update_texture_region(initial_data[subresource], *out_handle, subresource, nullptr);
				}
				return true;
			}
			break;
		}
		case api::resource_type::surface:
		{
			// Surfaces can never be arrays or have mipmap levels
			if (desc.texture.depth_or_layers != 1 || desc.texture.levels != 1)
				break;

			assert(initial_data == nullptr);

			switch (desc.usage & (api::resource_usage::depth_stencil | api::resource_usage::render_target))
			{
				case api::resource_usage::depth_stencil:
				case api::resource_usage::depth_stencil_read:
				case api::resource_usage::depth_stencil_write:
				{
					D3DSURFACE_DESC internal_desc = {};
					convert_resource_desc(desc, internal_desc, nullptr, nullptr, _caps);
					assert(internal_desc.Usage == D3DUSAGE_DEPTHSTENCIL);

					if (com_ptr<IDirect3DSurface9> object;
						SUCCEEDED((desc.usage & api::resource_usage::shader_resource) != 0 ?
							create_surface_replacement(internal_desc, &object, shared_handle) :
							_orig->CreateDepthStencilSurface(internal_desc.Width, internal_desc.Height, internal_desc.Format, internal_desc.MultiSampleType, internal_desc.MultiSampleQuality, FALSE, &object, shared_handle)))
					{
						*out_handle = { reinterpret_cast<uintptr_t>(object.release()) };
						return true;
					}
					break;
				}
				case api::resource_usage::render_target:
				{
					BOOL lockable = FALSE;
					D3DSURFACE_DESC internal_desc = {};
					convert_resource_desc(desc, internal_desc, nullptr, &lockable, _caps);
					assert(internal_desc.Usage == D3DUSAGE_RENDERTARGET);

					if (com_ptr<IDirect3DSurface9> object;
						SUCCEEDED((desc.usage & api::resource_usage::shader_resource) != 0 ?
							create_surface_replacement(internal_desc, &object, shared_handle) :
							_orig->CreateRenderTarget(internal_desc.Width, internal_desc.Height, internal_desc.Format, internal_desc.MultiSampleType, internal_desc.MultiSampleQuality, lockable, &object, shared_handle)))
					{
						*out_handle = { reinterpret_cast<uintptr_t>(object.release()) };
						return true;
					}
					break;
				}
			}
			break;
		}
	}

	return false;
}
void reshade::d3d9::device_impl::destroy_resource(api::resource handle)
{
	if (handle.handle != 0)
		reinterpret_cast<IUnknown *>(handle.handle)->Release();
}

reshade::api::resource_desc reshade::d3d9::device_impl::get_resource_desc(api::resource resource) const
{
	assert(resource.handle != 0);

	const auto object = reinterpret_cast<IDirect3DResource9 *>(resource.handle);

	switch (IDirect3DResource9_GetType(object))
	{
		case D3DRTYPE_SURFACE:
		{
			D3DSURFACE_DESC internal_desc;
			IDirect3DSurface9_GetDesc(static_cast<IDirect3DSurface9 *>(object), &internal_desc);

			return convert_resource_desc(internal_desc, 1, FALSE, _caps);
		}
		case D3DRTYPE_TEXTURE:
		{
			D3DSURFACE_DESC internal_desc;
			IDirect3DTexture9_GetLevelDesc(static_cast<IDirect3DTexture9 *>(object), 0, &internal_desc);
			internal_desc.Type = D3DRTYPE_TEXTURE;

			const UINT levels = IDirect3DTexture9_GetLevelCount(static_cast<IDirect3DTexture9 *>(object));

			return convert_resource_desc(internal_desc, levels, FALSE, _caps);
		}
		case D3DRTYPE_VOLUMETEXTURE:
		{
			D3DVOLUME_DESC internal_desc;
			IDirect3DVolumeTexture9_GetLevelDesc(static_cast<IDirect3DVolumeTexture9 *>(object), 0, &internal_desc);
			internal_desc.Type = D3DRTYPE_VOLUMETEXTURE;

			const UINT levels = IDirect3DVolumeTexture9_GetLevelCount(static_cast<IDirect3DVolumeTexture9 *>(object));

			return convert_resource_desc(internal_desc, levels);
		}
		case D3DRTYPE_CUBETEXTURE:
		{
			D3DSURFACE_DESC internal_desc;
			IDirect3DCubeTexture9_GetLevelDesc(static_cast<IDirect3DCubeTexture9 *>(object), 0, &internal_desc);
			internal_desc.Type = D3DRTYPE_CUBETEXTURE;

			const UINT levels = IDirect3DCubeTexture9_GetLevelCount(static_cast<IDirect3DCubeTexture9 *>(object));

			return convert_resource_desc(internal_desc, levels, FALSE, _caps);
		}
		case D3DRTYPE_VERTEXBUFFER:
		{
			D3DVERTEXBUFFER_DESC internal_desc;
			IDirect3DVertexBuffer9_GetDesc(static_cast<IDirect3DVertexBuffer9 *>(object), &internal_desc);

			return convert_resource_desc(internal_desc);
		}
		case D3DRTYPE_INDEXBUFFER:
		{
			D3DINDEXBUFFER_DESC internal_desc;
			IDirect3DIndexBuffer9_GetDesc(static_cast<IDirect3DIndexBuffer9 *>(object), &internal_desc);

			return convert_resource_desc(internal_desc);
		}
	}

	assert(false); // Not implemented
	return api::resource_desc {};
}

bool reshade::d3d9::device_impl::create_resource_view(api::resource resource, api::resource_usage usage_type, const api::resource_view_desc &desc, api::resource_view *out_handle)
{
	*out_handle = { 0 };

	if (resource.handle == 0)
		return false;

	const auto object = reinterpret_cast<IDirect3DResource9 *>(resource.handle);

	D3DFORMAT  view_format = convert_format(desc.format);

	// Set the first bit in the handle to indicate whether this view is using a sRGB format
	const bool set_srgb_bit =
		desc.format == api::format::r8g8b8a8_unorm_srgb || desc.format == api::format::r8g8b8x8_unorm_srgb ||
		desc.format == api::format::b8g8r8a8_unorm_srgb || desc.format == api::format::b8g8r8x8_unorm_srgb ||
		desc.format == api::format::bc1_unorm_srgb || desc.format == api::format::bc2_unorm_srgb || desc.format == api::format::bc3_unorm_srgb;
	// This requires the resource pointers to be at least 2-byte aligned, to ensure the first bit is always unused otherwise
	static_assert(alignof(IDirect3DResource9) >= 2);

	switch (IDirect3DResource9_GetType(object))
	{
		case D3DRTYPE_SURFACE:
		{
			assert(desc.type == api::resource_view_type::texture_2d || desc.type == api::resource_view_type::texture_2d_multisample);
			assert(desc.texture.first_layer == 0 && (desc.texture.layer_count == 1 || desc.texture.layer_count == UINT32_MAX));

			if (usage_type == api::resource_usage::depth_stencil || usage_type == api::resource_usage::render_target)
			{
				if (desc.texture.first_level != 0 || desc.texture.level_count != 1)
					break;

				D3DSURFACE_DESC internal_desc;
				if (FAILED(IDirect3DSurface9_GetDesc(static_cast<IDirect3DSurface9 *>(object), &internal_desc)))
					break;

				if (!convert_format_internal(desc.format, view_format) || internal_desc.Format != view_format)
					break;

				object->AddRef();
				{
					*out_handle = { reinterpret_cast<uintptr_t>(object) | (set_srgb_bit ? 1ull : 0) };
					return true;
				}
			}
			break;
		}
		case D3DRTYPE_TEXTURE:
		{
			assert(desc.type == api::resource_view_type::texture_2d || desc.type == api::resource_view_type::texture_2d_multisample);
			assert(desc.texture.first_layer == 0 && (desc.texture.layer_count == 1 || desc.texture.layer_count == UINT32_MAX));

			if (usage_type == api::resource_usage::depth_stencil || usage_type == api::resource_usage::render_target)
			{
				if (desc.texture.level_count != 1)
					break;

				D3DSURFACE_DESC internal_desc;
				if (FAILED(IDirect3DTexture9_GetLevelDesc(static_cast<IDirect3DTexture9 *>(object), desc.texture.first_level, &internal_desc)))
					break;

				if (!convert_format_internal(desc.format, view_format) || internal_desc.Format != view_format)
					break;

				if (com_ptr<IDirect3DSurface9> surface;
					SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(static_cast<IDirect3DTexture9 *>(object), desc.texture.first_level, &surface)))
				{
					*out_handle = { reinterpret_cast<uintptr_t>(surface.release()) | (set_srgb_bit ? 1ull : 0) };
					return true;
				}
			}
			else if (usage_type == api::resource_usage::shader_resource && desc.texture.first_level == 0)
			{
				D3DSURFACE_DESC internal_desc;
				if (FAILED(IDirect3DTexture9_GetLevelDesc(static_cast<IDirect3DTexture9 *>(object), 0, &internal_desc)))
					break;

				if (!convert_format_internal(desc.format, view_format) || internal_desc.Format != view_format)
					break;

				object->AddRef();
				{
					*out_handle = { reinterpret_cast<uintptr_t>(object) | (set_srgb_bit ? 1ull : 0) };
					return true;
				}
			}
			break;
		}
		case D3DRTYPE_VOLUMETEXTURE:
		{
			if (usage_type == api::resource_usage::shader_resource && desc.texture.first_level == 0 && desc.texture.first_layer == 0)
			{
				D3DVOLUME_DESC internal_desc;
				if (FAILED(IDirect3DVolumeTexture9_GetLevelDesc(static_cast<IDirect3DVolumeTexture9 *>(object), 0, &internal_desc)))
					break;

				if (!convert_format_internal(desc.format, view_format) || internal_desc.Format != view_format)
					break;

				object->AddRef();
				{
					*out_handle = { reinterpret_cast<uintptr_t>(object) | (set_srgb_bit ? 1ull : 0) };
					return true;
				}
			}
			break;
		}
		case D3DRTYPE_CUBETEXTURE:
		{
			if (usage_type == api::resource_usage::depth_stencil || usage_type == api::resource_usage::render_target)
			{
				assert(desc.type == api::resource_view_type::texture_2d || desc.type == api::resource_view_type::texture_2d_multisample);

				if (desc.texture.level_count != 1 || desc.texture.layer_count != 1)
					break;

				D3DSURFACE_DESC internal_desc;
				if (FAILED(IDirect3DCubeTexture9_GetLevelDesc(static_cast<IDirect3DCubeTexture9 *>(object), desc.texture.first_level, &internal_desc)))
					break;

				if (!convert_format_internal(desc.format, view_format) || internal_desc.Format != view_format)
					break;

				if (com_ptr<IDirect3DSurface9> surface;
					SUCCEEDED(IDirect3DCubeTexture9_GetCubeMapSurface(static_cast<IDirect3DCubeTexture9 *>(object), static_cast<D3DCUBEMAP_FACES>(desc.texture.first_layer), desc.texture.first_level, &surface)))
				{
					*out_handle = { reinterpret_cast<uintptr_t>(surface.release()) | (set_srgb_bit ? 1ull : 0) };
					return true;
				}
			}
			else if (usage_type == api::resource_usage::shader_resource && desc.texture.first_level == 0 && desc.texture.first_layer == 0)
			{
				assert(desc.type == api::resource_view_type::texture_cube);

				D3DSURFACE_DESC internal_desc;
				if (FAILED(IDirect3DCubeTexture9_GetLevelDesc(static_cast<IDirect3DCubeTexture9 *>(object), 0, &internal_desc)))
					break;

				if (!convert_format_internal(desc.format, view_format) || internal_desc.Format != view_format)
					break;

				object->AddRef();
				{
					*out_handle = { reinterpret_cast<uintptr_t>(object) | (set_srgb_bit ? 1ull : 0) };
					return true;
				}
			}
			break;
		}
	}

	return false;
}
void reshade::d3d9::device_impl::destroy_resource_view(api::resource_view handle)
{
	if (handle.handle != 0)
		reinterpret_cast<IUnknown *>(handle.handle & ~1ull)->Release();
}

reshade::api::resource reshade::d3d9::device_impl::get_resource_from_view(api::resource_view view) const
{
	assert(view.handle != 0);

	const auto object = reinterpret_cast<IUnknown *>(view.handle & ~1ull);

	// Get container in case this is a surface resource
	if (com_ptr<IDirect3DSurface9> surface;
		SUCCEEDED(object->QueryInterface(&surface)))
	{
		if (com_ptr<IDirect3DResource9> resource;
			SUCCEEDED(surface->GetContainer(IID_PPV_ARGS(&resource))))
			return to_handle(resource.get());
	}
	else if (com_ptr<IDirect3DVolume9> volume;
		SUCCEEDED(object->QueryInterface(&volume)))
	{
		if (com_ptr<IDirect3DResource9> resource;
			SUCCEEDED(volume->GetContainer(IID_PPV_ARGS(&resource))))
			return to_handle(resource.get());
	}

	// If unable to get container, just return the resource directly
	return to_handle(static_cast<IDirect3DResource9 *>(object));
}
reshade::api::resource reshade::d3d9::device_impl::get_resource_from_view(api::resource_view view, uint32_t *out_subresource, uint32_t *out_levels) const
{
	*out_subresource = 0;
	if (out_levels != nullptr)
		*out_levels = 1;

	const auto object = reinterpret_cast<IUnknown *>(view.handle & ~1ull);

	const auto resource = reinterpret_cast<IDirect3DResource9 *>(get_resource_from_view(view).handle);

	switch (IDirect3DResource9_GetType(resource))
	{
		case D3DRTYPE_TEXTURE:
		{
			for (DWORD level = 0, levels = IDirect3DTexture9_GetLevelCount(static_cast<IDirect3DTexture9 *>(resource)); level < levels; ++level)
			{
				com_ptr<IDirect3DSurface9> surface;
				if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(static_cast<IDirect3DTexture9 *>(resource), level, &surface)) && surface == static_cast<IDirect3DSurface9 *>(object))
				{
					*out_subresource = level;
					if (out_levels != nullptr)
						*out_levels = levels;
					break;
				}
			}
			break;
		}
		case D3DRTYPE_VOLUMETEXTURE:
		{
			for (DWORD level = 0, levels = IDirect3DVolumeTexture9_GetLevelCount(static_cast<IDirect3DVolumeTexture9 *>(resource)); level < levels; ++level)
			{
				com_ptr<IDirect3DVolume9> volume;
				if (SUCCEEDED(IDirect3DVolumeTexture9_GetVolumeLevel(static_cast<IDirect3DVolumeTexture9 *>(resource), level, &volume)) && volume == static_cast<IDirect3DVolume9 *>(object))
				{
					*out_subresource = level;
					if (out_levels != nullptr)
						*out_levels = levels;
					break;
				}
			}
			break;
		}
		case D3DRTYPE_CUBETEXTURE:
		{
			for (DWORD level = 0, levels = IDirect3DCubeTexture9_GetLevelCount(static_cast<IDirect3DCubeTexture9 *>(resource)); level < levels; ++level)
			{
				for (D3DCUBEMAP_FACES face = D3DCUBEMAP_FACE_POSITIVE_X; face <= D3DCUBEMAP_FACE_NEGATIVE_Z; face = static_cast<D3DCUBEMAP_FACES>(face + 1))
				{
					com_ptr<IDirect3DSurface9> surface;
					if (SUCCEEDED(IDirect3DCubeTexture9_GetCubeMapSurface(static_cast<IDirect3DCubeTexture9 *>(resource), face, level, &surface)) && surface == static_cast<IDirect3DSurface9 *>(object))
					{
						*out_subresource = level + face * levels;
						if (out_levels != nullptr)
							*out_levels = levels;
						break;
					}
				}
			}
			break;
		}
	}

	return to_handle(resource);
}
reshade::api::resource_view_desc reshade::d3d9::device_impl::get_resource_view_desc(api::resource_view view) const
{
	assert(view.handle != 0);

	// This does not work if the handle points to a 'IDirect3DVolume9' object (since that doesn't inherit from 'IDirect3DResource9')
	const auto object = reinterpret_cast<IDirect3DResource9 *>(view.handle & ~1ull);

	// Check whether the first bit in the handle is set, indicating whether this view is using a sRGB format
	const bool set_srgb_bit = (view.handle & 1ull) != 0;

	switch (IDirect3DResource9_GetType(object))
	{
		case D3DRTYPE_SURFACE:
		{
			uint32_t subresource, levels;
			get_resource_from_view(view, &subresource, &levels);

			D3DSURFACE_DESC internal_desc;
			IDirect3DSurface9_GetDesc(static_cast<IDirect3DSurface9 *>(object), &internal_desc);

			return api::resource_view_desc(api::format_to_default_typed(convert_format(internal_desc.Format), set_srgb_bit), subresource % levels, 1, subresource / levels, 1);
		}
		case D3DRTYPE_TEXTURE:
		{
			D3DSURFACE_DESC internal_desc;
			IDirect3DTexture9_GetLevelDesc(static_cast<IDirect3DTexture9 *>(object), 0, &internal_desc);

			return api::resource_view_desc(api::format_to_default_typed(convert_format(internal_desc.Format), set_srgb_bit), 0, UINT32_MAX, 0, UINT32_MAX);
		}
		case D3DRTYPE_VOLUMETEXTURE:
		{
			D3DVOLUME_DESC internal_desc;
			IDirect3DVolumeTexture9_GetLevelDesc(static_cast<IDirect3DVolumeTexture9 *>(object), 0, &internal_desc);

			return api::resource_view_desc(api::format_to_default_typed(convert_format(internal_desc.Format), set_srgb_bit), 0, UINT32_MAX, 0, UINT32_MAX);
		}
		case D3DRTYPE_CUBETEXTURE:
		{
			D3DSURFACE_DESC internal_desc;
			IDirect3DCubeTexture9_GetLevelDesc(static_cast<IDirect3DCubeTexture9 *>(object), 0, &internal_desc);

			return api::resource_view_desc(api::format_to_default_typed(convert_format(internal_desc.Format), set_srgb_bit), 0, UINT32_MAX, 0, UINT32_MAX);
		}
	}

	assert(false); // Not implemented
	return api::resource_view_desc();
}

bool reshade::d3d9::device_impl::map_buffer_region(api::resource resource, uint64_t offset, uint64_t size, api::map_access access, void **out_data)
{
	if (out_data == nullptr)
		return false;

	assert(resource.handle != 0);
	assert(offset <= std::numeric_limits<UINT>::max() && (size == UINT64_MAX || size <= std::numeric_limits<UINT>::max()));

	const auto object = reinterpret_cast<IDirect3DResource9 *>(resource.handle);

	switch (IDirect3DResource9_GetType(object))
	{
		case D3DRTYPE_VERTEXBUFFER:
		{
			return SUCCEEDED(IDirect3DVertexBuffer9_Lock(
				static_cast<IDirect3DVertexBuffer9 *>(object),
				static_cast<UINT>(offset),
				size != UINT64_MAX ? static_cast<UINT>(size) : 0,
				out_data,
				convert_access_flags(access)));
		}
		case D3DRTYPE_INDEXBUFFER:
		{
			return SUCCEEDED(IDirect3DIndexBuffer9_Lock(
				static_cast<IDirect3DIndexBuffer9 *>(object),
				static_cast<UINT>(offset),
				size != UINT64_MAX ? static_cast<UINT>(size) : 0,
				out_data,
				convert_access_flags(access)));
		}
	}

	assert(false); // Not implemented
	return false;
}
void reshade::d3d9::device_impl::unmap_buffer_region(api::resource resource)
{
	assert(resource.handle != 0);

	const auto object = reinterpret_cast<IDirect3DResource9 *>(resource.handle);

	switch (IDirect3DResource9_GetType(object))
	{
		case D3DRTYPE_VERTEXBUFFER:
		{
			IDirect3DVertexBuffer9_Unlock(static_cast<IDirect3DVertexBuffer9 *>(object));
			return;
		}
		case D3DRTYPE_INDEXBUFFER:
		{
			IDirect3DIndexBuffer9_Unlock(static_cast<IDirect3DIndexBuffer9 *>(object));
			return;
		}
	}

	assert(false); // Not implemented
}
bool reshade::d3d9::device_impl::map_texture_region(api::resource resource, uint32_t subresource, const api::subresource_box *box, api::map_access access, api::subresource_data *out_data)
{
	if (out_data == nullptr)
		return false;

	out_data->data = nullptr;
	out_data->row_pitch = 0;
	out_data->slice_pitch = 0;

	assert(resource.handle != 0);

	const auto object = reinterpret_cast<IDirect3DResource9 *>(resource.handle);

	switch (IDirect3DResource9_GetType(object))
	{
		case D3DRTYPE_SURFACE:
		{
			assert(subresource == 0);

			RECT rect;
			D3DLOCKED_RECT locked_rect;
			if (SUCCEEDED(IDirect3DSurface9_LockRect(static_cast<IDirect3DSurface9 *>(object), &locked_rect, convert_box_to_rect(box, rect), convert_access_flags(access))))
			{
				out_data->data = locked_rect.pBits;
				out_data->row_pitch = locked_rect.Pitch;
				return true;
			}
			return false;
		}
		case D3DRTYPE_TEXTURE:
		{
			RECT rect;
			D3DLOCKED_RECT locked_rect;
			if (SUCCEEDED(IDirect3DTexture9_LockRect(static_cast<IDirect3DTexture9 *>(object), subresource, &locked_rect, convert_box_to_rect(box, rect), convert_access_flags(access))))
			{
				out_data->data = locked_rect.pBits;
				out_data->row_pitch = locked_rect.Pitch;
				return true;
			}
			return false;
		}
		case D3DRTYPE_VOLUMETEXTURE:
		{
			D3DLOCKED_BOX locked_box;
			if (SUCCEEDED(IDirect3DVolumeTexture9_LockBox(static_cast<IDirect3DVolumeTexture9 *>(object), subresource, &locked_box, reinterpret_cast<const D3DBOX *>(box), convert_access_flags(access))))
			{
				out_data->data = locked_box.pBits;
				out_data->row_pitch = locked_box.RowPitch;
				out_data->slice_pitch = locked_box.SlicePitch;
				return true;
			}
			return false;
		}
		case D3DRTYPE_CUBETEXTURE:
		{
			const UINT levels = IDirect3DCubeTexture9_GetLevelCount(static_cast<IDirect3DCubeTexture9 *>(object));

			RECT rect;
			D3DLOCKED_RECT locked_rect;
			if (SUCCEEDED(IDirect3DCubeTexture9_LockRect(static_cast<IDirect3DCubeTexture9 *>(object), static_cast<D3DCUBEMAP_FACES>(subresource / levels), subresource % levels, &locked_rect, convert_box_to_rect(box, rect), convert_access_flags(access))))
			{
				out_data->data = locked_rect.pBits;
				out_data->row_pitch = locked_rect.Pitch;
				return true;
			}
			return false;
		}
	}

	assert(false); // Not implemented
	return false;
}
void reshade::d3d9::device_impl::unmap_texture_region(api::resource resource, uint32_t subresource)
{
	assert(resource.handle != 0);

	const auto object = reinterpret_cast<IDirect3DResource9 *>(resource.handle);

	switch (IDirect3DResource9_GetType(object))
	{
		case D3DRTYPE_SURFACE:
		{
			assert(subresource == 0);

			IDirect3DSurface9_UnlockRect(static_cast<IDirect3DSurface9 *>(object));
			return;
		}
		case D3DRTYPE_TEXTURE:
		{
			IDirect3DTexture9_UnlockRect(static_cast<IDirect3DTexture9 *>(object), subresource);
			return;
		}
		case D3DRTYPE_VOLUMETEXTURE:
		{
			IDirect3DVolumeTexture9_UnlockBox(static_cast<IDirect3DVolumeTexture9 *>(object), subresource);
			return;
		}
		case D3DRTYPE_CUBETEXTURE:
		{
			const UINT levels = IDirect3DCubeTexture9_GetLevelCount(static_cast<IDirect3DCubeTexture9 *>(object));

			IDirect3DCubeTexture9_UnlockRect(static_cast<IDirect3DCubeTexture9 *>(object), static_cast<D3DCUBEMAP_FACES>(subresource / levels), subresource % levels);
			return;
		}
	}

	assert(false); // Not implemented
}

void reshade::d3d9::device_impl::update_buffer_region(const void *data, api::resource resource, uint64_t offset, uint64_t size)
{
	assert(resource.handle != 0);
	assert(offset <= std::numeric_limits<UINT>::max() && size <= std::numeric_limits<UINT>::max());

	const auto object = reinterpret_cast<IDirect3DResource9 *>(resource.handle);

	switch (IDirect3DResource9_GetType(object))
	{
		case D3DRTYPE_VERTEXBUFFER:
		{
			void *mapped_ptr;
			if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(static_cast<IDirect3DVertexBuffer9 *>(object), static_cast<UINT>(offset), static_cast<UINT>(size), &mapped_ptr, 0)))
			{
				std::memcpy(mapped_ptr, data, static_cast<size_t>(size));
				IDirect3DVertexBuffer9_Unlock(static_cast<IDirect3DVertexBuffer9 *>(object));
			}
			return;
		}
		case D3DRTYPE_INDEXBUFFER:
		{
			void *mapped_ptr;
			if (SUCCEEDED(IDirect3DIndexBuffer9_Lock(static_cast<IDirect3DIndexBuffer9 *>(object), static_cast<UINT>(offset), static_cast<UINT>(size), &mapped_ptr, 0)))
			{
				std::memcpy(mapped_ptr, data, static_cast<size_t>(size));
				IDirect3DIndexBuffer9_Unlock(static_cast<IDirect3DIndexBuffer9 *>(object));
			}
			return;
		}
	}

	assert(false); // Not implemented
}
void reshade::d3d9::device_impl::update_texture_region(const api::subresource_data &data, api::resource resource, uint32_t subresource, const api::subresource_box *box)
{
	assert(resource.handle != 0);

	const auto object = reinterpret_cast<IDirect3DResource9 *>(resource.handle);

	switch (IDirect3DResource9_GetType(object))
	{
		case D3DRTYPE_TEXTURE:
		{
			// Get D3D texture format
			// Note: This fails for any mipmap level but the first one for textures with D3DUSAGE_AUTOGENMIPMAP, since in that case the D3D runtime does not have surfaces for those
			D3DSURFACE_DESC desc;
			if (FAILED(IDirect3DTexture9_GetLevelDesc(static_cast<IDirect3DTexture9 *>(object), subresource, &desc)))
				return;

			const UINT width = (box != nullptr) ? box->width() : desc.Width;
			const UINT height = (box != nullptr) ? box->height() : desc.Height;
			const bool use_systemmem_texture = IDirect3DTexture9_GetLevelCount(static_cast<IDirect3DTexture9 *>(object)) == 1 && box == nullptr;

			com_ptr<IDirect3DTexture9> intermediate;
			if (FAILED(_orig->CreateTexture(width, height, 1, use_systemmem_texture ? 0 : D3DUSAGE_DYNAMIC, desc.Format, use_systemmem_texture ? D3DPOOL_SYSTEMMEM : D3DPOOL_DEFAULT, &intermediate, nullptr)))
			{
				LOG(ERROR) << "Failed to create upload buffer!";
				LOG(DEBUG) << "> Details: Width = " << width << ", Height = " << height << ", Levels = " << "1" << ", Usage = " << (use_systemmem_texture ? "0" : "D3DUSAGE_DYNAMIC") << ", Format = " << desc.Format;
				return;
			}

			D3DLOCKED_RECT locked_rect;
			if (FAILED(IDirect3DTexture9_LockRect(intermediate.get(), 0, &locked_rect, static_cast<const RECT *>(nullptr), 0)))
				return;
			auto mapped_data = static_cast<uint8_t *>(locked_rect.pBits);
			auto upload_data = static_cast<const uint8_t *>(data.data);

			// If format is one of these two, assume they were overwritten by 'convert_format_internal', so handle them accordingly
			// TODO: Maybe store the original format as user data in the resource to avoid this hack?
			if (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8)
			{
				for (uint32_t y = 0; y < height; ++y, mapped_data += locked_rect.Pitch, upload_data += data.row_pitch)
				{
					switch (data.row_pitch / width)
					{
					case 1: // This is likely actually a r8 texture
						for (uint32_t x = 0, i = 0; x < width * 4; x += 4, i += 1)
							mapped_data[x + 0] = 0, // Set green and blue channel to zero
							mapped_data[x + 1] = 0,
							mapped_data[x + 2] = upload_data[i],
							mapped_data[x + 3] = 0xFF;
						break;
					case 2: // This is likely actually a r8g8 texture
						for (uint32_t x = 0, i = 0; x < width * 4; x += 4, i += 2)
							mapped_data[x + 0] = 0, // Set blue channel to zero
							mapped_data[x + 1] = upload_data[i + 1],
							mapped_data[x + 2] = upload_data[i + 0],
							mapped_data[x + 3] = 0xFF;
						break;
					case 4: // This is likely actually a r8g8b8a8 texture
					default:
						for (uint32_t x = 0, i = 0; x < width * 4; x += 4, i += 4)
							mapped_data[x + 0] = upload_data[i + 2], // Flip RGBA input to BGRA
							mapped_data[x + 1] = upload_data[i + 1],
							mapped_data[x + 2] = upload_data[i + 0],
							mapped_data[x + 3] = upload_data[i + 3];
						break;
					}
				}
			}
			else
			{
				for (uint32_t y = 0; y < height; ++y, mapped_data += locked_rect.Pitch, upload_data += data.row_pitch)
				{
					std::memcpy(mapped_data, upload_data, std::min(data.row_pitch, static_cast<uint32_t>(locked_rect.Pitch)));
				}
			}

			IDirect3DTexture9_UnlockRect(intermediate.get(), 0);

			if (use_systemmem_texture)
			{
				assert(subresource == 0);

				_orig->UpdateTexture(intermediate.get(), static_cast<IDirect3DTexture9 *>(object));
			}
			else
			{
				RECT dst_rect;

				com_ptr<IDirect3DSurface9> src_surface;
				IDirect3DTexture9_GetSurfaceLevel(intermediate.get(), 0, &src_surface);
				com_ptr<IDirect3DSurface9> dst_surface;
				IDirect3DTexture9_GetSurfaceLevel(static_cast<IDirect3DTexture9 *>(object), subresource, &dst_surface);

				_orig->StretchRect(src_surface.get(), nullptr, dst_surface.get(), convert_box_to_rect(box, dst_rect), D3DTEXF_NONE);
			}
			return;
		}
		case D3DRTYPE_VOLUMETEXTURE:
		{
			// TODO: Implement texture upload for 3D textures
			LOG(ERROR) << "Texture upload is not implemented for 3D textures in D3D9!";
			break;
		}
		case D3DRTYPE_CUBETEXTURE:
		{
			// TODO: Implement texture upload for cube textures
			LOG(ERROR) << "Texture upload is not implemented for cube textures in D3D9!";
			break;
		}
	}

	assert(false); // Not implemented
}

bool reshade::d3d9::device_impl::create_pipeline(api::pipeline_layout, uint32_t subobject_count, const api::pipeline_subobject *subobjects, api::pipeline *out_handle)
{
	if (subobject_count == 1)
	{
		switch (subobjects->type)
		{
		case api::pipeline_subobject_type::vertex_shader:
			assert(subobjects->count == 1);
			return create_vertex_shader(*static_cast<const api::shader_desc *>(subobjects->data), out_handle);
		case api::pipeline_subobject_type::pixel_shader:
			assert(subobjects->count == 1);
			return create_pixel_shader(*static_cast<const api::shader_desc *>(subobjects->data), out_handle);
		case api::pipeline_subobject_type::input_layout:
			return create_input_layout(subobjects->count, static_cast<const api::input_element *>(subobjects->data), out_handle);
		default:
			*out_handle = { 0 };
			return false;
		}
	}

	com_ptr<IDirect3DVertexShader9> vertex_shader;
	com_ptr<IDirect3DPixelShader9> pixel_shader;
	com_ptr<IDirect3DVertexDeclaration9> input_layout;
	api::blend_desc blend_state;
	api::rasterizer_desc rasterizer_state;
	api::depth_stencil_desc depth_stencil_state;
	api::primitive_topology topology = api::primitive_topology::triangle_list;
	uint32_t sample_mask = UINT32_MAX;
	uint32_t max_vertices = 3;

	for (uint32_t i = 0; i < subobject_count; ++i)
	{
		if (subobjects[i].count == 0)
			continue;

		api::pipeline temp;

		switch (subobjects[i].type)
		{
		case api::pipeline_subobject_type::vertex_shader:
			assert(subobjects[i].count == 1);
			if (static_cast<const api::shader_desc *>(subobjects[i].data)->code_size == 0)
				break;
			if (!create_vertex_shader(*static_cast<const api::shader_desc *>(subobjects[i].data), &temp))
				goto exit_failure;
			vertex_shader = com_ptr<IDirect3DVertexShader9>(reinterpret_cast<IDirect3DVertexShader9 *>(temp.handle), true);
			break;
		case api::pipeline_subobject_type::hull_shader:
		case api::pipeline_subobject_type::domain_shader:
		case api::pipeline_subobject_type::geometry_shader:
			assert(subobjects[i].count == 1);
			if (static_cast<const api::shader_desc *>(subobjects[i].data)->code_size == 0)
				break;
			goto exit_failure; // Not supported
		case api::pipeline_subobject_type::pixel_shader:
			assert(subobjects[i].count == 1);
			if (static_cast<const api::shader_desc *>(subobjects[i].data)->code_size == 0)
				break;
			if (!create_pixel_shader(*static_cast<const api::shader_desc *>(subobjects[i].data), &temp))
				goto exit_failure;
			pixel_shader = com_ptr<IDirect3DPixelShader9>(reinterpret_cast<IDirect3DPixelShader9 *>(temp.handle), true);
			break;
		case api::pipeline_subobject_type::compute_shader:
			assert(subobjects[i].count == 1);
			if (static_cast<const api::shader_desc *>(subobjects[i].data)->code_size == 0)
				break;
			goto exit_failure; // Not supported
		case api::pipeline_subobject_type::input_layout:
			assert(subobjects[i].count <= MAXD3DDECLLENGTH);
			if (!create_input_layout(subobjects[i].count, static_cast<const api::input_element *>(subobjects[i].data), &temp))
				goto exit_failure;
			input_layout = com_ptr<IDirect3DVertexDeclaration9>(reinterpret_cast<IDirect3DVertexDeclaration9 *>(temp.handle), true);
			break;
		case api::pipeline_subobject_type::stream_output_state:
			assert(subobjects[i].count == 1);
			goto exit_failure; // Not implemented
		case api::pipeline_subobject_type::blend_state:
			assert(subobjects[i].count == 1);
			blend_state = *static_cast<const api::blend_desc *>(subobjects[i].data);
			if (blend_state.alpha_to_coverage_enable || blend_state.logic_op_enable[0])
				goto exit_failure;
			break;
		case api::pipeline_subobject_type::rasterizer_state:
			assert(subobjects[i].count == 1);
			rasterizer_state = *static_cast<const api::rasterizer_desc *>(subobjects[i].data);
			if (rasterizer_state.conservative_rasterization)
				goto exit_failure;
			break;
		case api::pipeline_subobject_type::depth_stencil_state:
			assert(subobjects[i].count == 1);
			depth_stencil_state = *static_cast<const api::depth_stencil_desc *>(subobjects[i].data);
			break;
		case api::pipeline_subobject_type::primitive_topology:
			assert(subobjects[i].count == 1);
			topology = *static_cast<const api::primitive_topology *>(subobjects[i].data);
			if (topology > api::primitive_topology::triangle_fan)
				goto exit_failure;
			break;
		case api::pipeline_subobject_type::depth_stencil_format:
		case api::pipeline_subobject_type::render_target_formats:
			break; // Ignored
		case api::pipeline_subobject_type::sample_mask:
			assert(subobjects[i].count == 1);
			sample_mask = *static_cast<const uint32_t *>(subobjects[i].data);
			break;
		case api::pipeline_subobject_type::sample_count:
			assert(subobjects[i].count == 1);
			break; // Ignored
		case api::pipeline_subobject_type::viewport_count:
			assert(subobjects[i].count == 1);
			if (*static_cast<const uint32_t *>(subobjects[i].data) > 1)
				goto exit_failure;
			break;
		case api::pipeline_subobject_type::dynamic_pipeline_states:
			break; // Ignored
		case api::pipeline_subobject_type::max_vertex_count:
			max_vertices = *static_cast<const uint32_t *>(subobjects[i].data);
			break;
		default:
			assert(false);
			goto exit_failure;
		}
	}

	if (FAILED(_orig->BeginStateBlock()))
		goto exit_failure;

	_orig->SetVertexShader(vertex_shader.get());
	_orig->SetPixelShader(pixel_shader.get());

	if (input_layout != nullptr)
	{
		_orig->SetVertexDeclaration(input_layout.get());
	}
	else
	{
		// Resize vertex buffer which holds vertex indices when necessary
		D3DVERTEXBUFFER_DESC default_input_stream_desc;
		if (_default_input_stream == nullptr || (SUCCEEDED(_default_input_stream->GetDesc(&default_input_stream_desc)) && default_input_stream_desc.Size < (max_vertices * sizeof(float))))
		{
			_default_input_stream.reset();

			if (FAILED(_orig->CreateVertexBuffer(max_vertices * sizeof(float), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &_default_input_stream, nullptr)))
			{
				LOG(ERROR) << "Failed to create default input stream!";
				goto exit_failure;
			}

			if (float *data;
				SUCCEEDED(IDirect3DVertexBuffer9_Lock(_default_input_stream.get(), 0, max_vertices * sizeof(float), reinterpret_cast<void **>(&data), 0)))
			{
				for (UINT i = 0; i < max_vertices; ++i)
					data[i] = static_cast<float>(i);
				IDirect3DVertexBuffer9_Unlock(_default_input_stream.get());
			}
		}

		// Create input layout for vertex buffer which holds vertex indices
		if (_default_input_layout == nullptr)
		{
			const D3DVERTEXELEMENT9 declaration[] = {
				{ 0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
				D3DDECL_END()
			};

			if (FAILED(_orig->CreateVertexDeclaration(declaration, &_default_input_layout)))
			{
				LOG(ERROR) << "Failed to create default vertex declaration!";
				goto exit_failure;
			}
		}

		// Setup default input (so that vertex shaders can get the vertex IDs)
		_orig->SetStreamSource(0, _default_input_stream.get(), 0, sizeof(float));
		_orig->SetVertexDeclaration(_default_input_layout.get());
	}

	_orig->SetRenderState(D3DRS_ZENABLE, depth_stencil_state.depth_enable);
	_orig->SetRenderState(D3DRS_FILLMODE, convert_fill_mode(rasterizer_state.fill_mode));
	_orig->SetRenderState(D3DRS_ZWRITEENABLE, depth_stencil_state.depth_write_mask);
	_orig->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	_orig->SetRenderState(D3DRS_LASTPIXEL, TRUE);
	_orig->SetRenderState(D3DRS_SRCBLEND, convert_blend_factor(blend_state.source_color_blend_factor[0]));
	_orig->SetRenderState(D3DRS_DESTBLEND, convert_blend_factor(blend_state.dest_color_blend_factor[0]));
	_orig->SetRenderState(D3DRS_CULLMODE, convert_cull_mode(rasterizer_state.cull_mode, rasterizer_state.front_counter_clockwise));
	_orig->SetRenderState(D3DRS_ZFUNC, convert_compare_op(depth_stencil_state.depth_func));
	_orig->SetRenderState(D3DRS_DITHERENABLE, FALSE);
	_orig->SetRenderState(D3DRS_ALPHABLENDENABLE, blend_state.blend_enable[0]);
	_orig->SetRenderState(D3DRS_FOGENABLE, FALSE);
	_orig->SetRenderState(D3DRS_STENCILENABLE, depth_stencil_state.stencil_enable);
	_orig->SetRenderState(D3DRS_STENCILZFAIL,
		convert_stencil_op(rasterizer_state.front_counter_clockwise ? depth_stencil_state.back_stencil_depth_fail_op : depth_stencil_state.front_stencil_depth_fail_op));
	_orig->SetRenderState(D3DRS_STENCILFAIL,
		convert_stencil_op(rasterizer_state.front_counter_clockwise ? depth_stencil_state.back_stencil_fail_op : depth_stencil_state.front_stencil_fail_op));
	_orig->SetRenderState(D3DRS_STENCILPASS,
		convert_stencil_op(rasterizer_state.front_counter_clockwise ? depth_stencil_state.back_stencil_pass_op : depth_stencil_state.front_stencil_pass_op));
	_orig->SetRenderState(D3DRS_STENCILFUNC,
		convert_compare_op(rasterizer_state.front_counter_clockwise ? depth_stencil_state.back_stencil_func : depth_stencil_state.front_stencil_func));
	_orig->SetRenderState(D3DRS_STENCILREF, depth_stencil_state.stencil_reference_value);
	_orig->SetRenderState(D3DRS_STENCILMASK, depth_stencil_state.stencil_read_mask);
	_orig->SetRenderState(D3DRS_STENCILWRITEMASK, depth_stencil_state.stencil_write_mask);
	_orig->SetRenderState(D3DRS_CLIPPING, rasterizer_state.depth_clip_enable);
	_orig->SetRenderState(D3DRS_LIGHTING, FALSE);
	_orig->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
	_orig->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
	_orig->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, rasterizer_state.multisample_enable);
	_orig->SetRenderState(D3DRS_MULTISAMPLEMASK, sample_mask);
	_orig->SetRenderState(D3DRS_COLORWRITEENABLE, blend_state.render_target_write_mask[0]);
	_orig->SetRenderState(D3DRS_BLENDOP, convert_blend_op(blend_state.color_blend_op[0]));
	_orig->SetRenderState(D3DRS_SCISSORTESTENABLE, rasterizer_state.scissor_enable);
	_orig->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *reinterpret_cast<const DWORD *>(&rasterizer_state.slope_scaled_depth_bias));
	_orig->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, rasterizer_state.antialiased_line_enable);
	_orig->SetRenderState(D3DRS_ENABLEADAPTIVETESSELLATION, FALSE);
	_orig->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE,
		rasterizer_state.cull_mode == api::cull_mode::none && (
			depth_stencil_state.front_stencil_fail_op != depth_stencil_state.back_stencil_fail_op ||
			depth_stencil_state.front_stencil_depth_fail_op != depth_stencil_state.back_stencil_depth_fail_op ||
			depth_stencil_state.front_stencil_pass_op != depth_stencil_state.back_stencil_pass_op ||
			depth_stencil_state.front_stencil_func != depth_stencil_state.back_stencil_func));
	_orig->SetRenderState(D3DRS_CCW_STENCILZFAIL,
		convert_stencil_op(rasterizer_state.front_counter_clockwise ? depth_stencil_state.front_stencil_depth_fail_op : depth_stencil_state.back_stencil_depth_fail_op));
	_orig->SetRenderState(D3DRS_CCW_STENCILFAIL,
		convert_stencil_op(rasterizer_state.front_counter_clockwise ? depth_stencil_state.front_stencil_fail_op : depth_stencil_state.back_stencil_fail_op));
	_orig->SetRenderState(D3DRS_CCW_STENCILPASS,
		convert_stencil_op(rasterizer_state.front_counter_clockwise ? depth_stencil_state.front_stencil_pass_op : depth_stencil_state.back_stencil_pass_op));
	_orig->SetRenderState(D3DRS_CCW_STENCILFUNC,
		convert_compare_op(rasterizer_state.front_counter_clockwise ? depth_stencil_state.front_stencil_func : depth_stencil_state.back_stencil_func));
	_orig->SetRenderState(D3DRS_COLORWRITEENABLE1, blend_state.render_target_write_mask[1]);
	_orig->SetRenderState(D3DRS_COLORWRITEENABLE2, blend_state.render_target_write_mask[2]);
	_orig->SetRenderState(D3DRS_COLORWRITEENABLE3, blend_state.render_target_write_mask[3]);
	_orig->SetRenderState(D3DRS_BLENDFACTOR, D3DCOLOR_COLORVALUE(blend_state.blend_constant[0], blend_state.blend_constant[1], blend_state.blend_constant[2], blend_state.blend_constant[3]));
	_orig->SetRenderState(D3DRS_DEPTHBIAS, *reinterpret_cast<const DWORD *>(&rasterizer_state.depth_bias));
	_orig->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
	_orig->SetRenderState(D3DRS_SRCBLENDALPHA, convert_blend_factor(blend_state.source_alpha_blend_factor[0]));
	_orig->SetRenderState(D3DRS_DESTBLENDALPHA, convert_blend_factor(blend_state.dest_alpha_blend_factor[0]));
	_orig->SetRenderState(D3DRS_BLENDOPALPHA, convert_blend_op(blend_state.alpha_blend_op[0]));

	if (com_ptr<IDirect3DStateBlock9> state_block;
		SUCCEEDED(_orig->EndStateBlock(&state_block)))
	{
		const auto impl = new pipeline_impl();
		impl->prim_type = convert_primitive_topology(topology);
		impl->state_block = std::move(state_block);

		// Set first bit to identify this as a 'pipeline_impl' handle for 'destroy_pipeline'
		static_assert(alignof(pipeline_impl) >= 2);

		*out_handle = { reinterpret_cast<uintptr_t>(impl) | 1 };
		return true;
	}

exit_failure:
	*out_handle = { 0 };
	return false;
}
bool reshade::d3d9::device_impl::create_input_layout(uint32_t count, const api::input_element *desc, api::pipeline *out_handle)
{
	static_assert(alignof(IDirect3DVertexDeclaration9) >= 2);

	std::vector<D3DVERTEXELEMENT9> internal_elements;
	convert_input_layout_desc(count, desc, internal_elements);

	if (com_ptr<IDirect3DVertexDeclaration9> object;
		internal_elements.size() == 1 || // Avoid vertex declaration creation if the input layout is empty (it always contains at least a single 'D3DDECL_END' element)
		SUCCEEDED(_orig->CreateVertexDeclaration(internal_elements.data(), &object)))
	{
		*out_handle = to_handle(object.release());
		return true;
	}
	else
	{
		*out_handle = { 0 };
		return false;
	}
}
bool reshade::d3d9::device_impl::create_vertex_shader(const api::shader_desc &desc, api::pipeline *out_handle)
{
	static_assert(alignof(IDirect3DVertexShader9) >= 2);

	assert(desc.spec_constants == 0);

	if (com_ptr<IDirect3DVertexShader9> object;
		SUCCEEDED(_orig->CreateVertexShader(static_cast<const DWORD *>(desc.code), &object)))
	{
		*out_handle = to_handle(object.release());
		return true;
	}
	else
	{
		*out_handle = { 0 };
		return false;
	}
}
bool reshade::d3d9::device_impl::create_pixel_shader(const api::shader_desc &desc, api::pipeline *out_handle)
{
	static_assert(alignof(IDirect3DPixelShader9) >= 2);

	assert(desc.spec_constants == 0);

	if (com_ptr<IDirect3DPixelShader9> object;
		SUCCEEDED(_orig->CreatePixelShader(static_cast<const DWORD *>(desc.code), &object)))
	{
		*out_handle = to_handle(object.release());
		return true;
	}
	else
	{
		*out_handle = { 0 };
		return false;
	}
}
void reshade::d3d9::device_impl::destroy_pipeline(api::pipeline handle)
{
	if (handle.handle & 1)
		delete reinterpret_cast<pipeline_impl *>(handle.handle ^ 1);
	else if (handle.handle != 0)
		reinterpret_cast<IUnknown *>(handle.handle)->Release();
}

bool reshade::d3d9::device_impl::create_pipeline_layout(uint32_t param_count, const api::pipeline_layout_param *params, api::pipeline_layout *out_handle)
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
			if (merged_range.array_size > 1 || merged_range.dx_register_space != 0)
				return false;

			for (uint32_t k = 1; k < params[i].descriptor_set.count; ++k)
			{
				const api::descriptor_range &range = params[i].descriptor_set.ranges[k];

				if (range.type != merged_range.type || range.array_size > 1 || range.dx_register_space != merged_range.dx_register_space)
					return false;

				if (range.binding >= merged_range.binding)
				{
					const uint32_t distance = range.binding - merged_range.binding;

					if ((range.dx_register_index - merged_range.dx_register_index) != distance)
						return false;

					merged_range.count += distance;
					merged_range.visibility |= range.visibility;
				}
				else
				{
					const uint32_t distance = merged_range.binding - range.binding;

					if ((merged_range.dx_register_index - range.dx_register_index) != distance)
						return false;

					merged_range.binding = range.binding;
					merged_range.dx_register_index = range.dx_register_index;
					merged_range.count += distance;
					merged_range.visibility |= range.visibility;
				}
			}
			break;
		case api::pipeline_layout_param_type::push_descriptors:
			merged_range = params[i].push_descriptors;
			if (merged_range.dx_register_space != 0)
				return false;
			break;
		case api::pipeline_layout_param_type::push_constants:
			merged_range.dx_register_index = params[i].push_constants.dx_register_index;
			merged_range.dx_register_space = params[i].push_constants.dx_register_space;
			if (merged_range.dx_register_space != 0)
				return false;
			break;
		}
	}

	const auto impl = new pipeline_layout_impl();
	impl->ranges = std::move(ranges);

	*out_handle = { reinterpret_cast<uintptr_t>(impl) };
	return true;
}
void reshade::d3d9::device_impl::destroy_pipeline_layout(api::pipeline_layout handle)
{
	assert(handle != global_pipeline_layout);

	delete reinterpret_cast<pipeline_layout_impl *>(handle.handle);
}

bool reshade::d3d9::device_impl::allocate_descriptor_sets(uint32_t count, api::pipeline_layout layout, uint32_t layout_param, api::descriptor_set *out_sets)
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
				set_impl->descriptors.resize(set_impl->count * 1);
				break;
			case api::descriptor_type::sampler_with_resource_view:
				set_impl->descriptors.resize(set_impl->count * 2);
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
void reshade::d3d9::device_impl::free_descriptor_sets(uint32_t count, const api::descriptor_set *sets)
{
	for (uint32_t i = 0; i < count; ++i)
		delete reinterpret_cast<descriptor_set_impl *>(sets[i].handle);
}

void reshade::d3d9::device_impl::get_descriptor_pool_offset(api::descriptor_set set, uint32_t binding, uint32_t array_offset, api::descriptor_pool *pool, uint32_t *offset) const
{
	assert(set.handle != 0 && array_offset == 0);

	*pool = { 0 }; // Not implemented
	*offset = binding;
}

void reshade::d3d9::device_impl::copy_descriptor_sets(uint32_t count, const api::descriptor_set_copy *copies)
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
			std::memcpy(&dst_set_impl->descriptors[copy.dest_binding * 1], &src_set_impl->descriptors[copy.source_binding * 1], copy.count * sizeof(uint64_t) * 1);
			break;
		case api::descriptor_type::sampler_with_resource_view:
			std::memcpy(&dst_set_impl->descriptors[copy.dest_binding * 2], &src_set_impl->descriptors[copy.source_binding * 2], copy.count * sizeof(uint64_t) * 2);
			break;
		default:
			assert(false);
			break;
		}
	}
}
void reshade::d3d9::device_impl::update_descriptor_sets(uint32_t count, const api::descriptor_set_update *updates)
{
	for (uint32_t i = 0; i < count; ++i)
	{
		const api::descriptor_set_update &update = updates[i];

		assert(update.array_offset == 0);

		const auto set_impl = reinterpret_cast<descriptor_set_impl *>(update.set.handle);

		assert(set_impl != nullptr && set_impl->type == update.type);

		switch (update.type)
		{
		case api::descriptor_type::sampler:
		case api::descriptor_type::shader_resource_view:
			std::memcpy(&set_impl->descriptors[update.binding * 1], update.descriptors, update.count * sizeof(uint64_t) * 1);
			break;
		case api::descriptor_type::sampler_with_resource_view:
			std::memcpy(&set_impl->descriptors[update.binding * 2], update.descriptors, update.count * sizeof(uint64_t) * 2);
			break;
		default:
			assert(false);
			break;
		}
	}
}

bool reshade::d3d9::device_impl::create_query_pool(api::query_type type, uint32_t size, api::query_pool *out_handle)
{
	const auto impl = new query_pool_impl();
	impl->type = type;
	impl->queries.resize(size);

	const D3DQUERYTYPE internal_type = convert_query_type(type);

	for (uint32_t i = 0; i < size; ++i)
	{
		if (FAILED(_orig->CreateQuery(internal_type, &impl->queries[i])))
		{
			delete impl;

			*out_handle = { 0 };
			return false;
		}
	}

	*out_handle = { reinterpret_cast<uintptr_t>(impl) };
	return true;
}
void reshade::d3d9::device_impl::destroy_query_pool(api::query_pool handle)
{
	delete reinterpret_cast<query_pool_impl *>(handle.handle);
}

bool reshade::d3d9::device_impl::get_query_pool_results(api::query_pool pool, uint32_t first, uint32_t count, void *results, uint32_t stride)
{
	assert(pool.handle != 0);

	const auto impl = reinterpret_cast<query_pool_impl *>(pool.handle);

	for (size_t i = 0; i < count; ++i)
	{
		if (impl->queries[first + i]->GetData(static_cast<uint8_t *>(results) + i * stride, stride, 0) != S_OK)
			return false;

		if (impl->type == api::query_type::timestamp)
		{
			assert(stride >= sizeof(uint64_t));
			// D3D9 timestamp queries seem to always have a resolution of 10ns (D3DQUERYTYPE_TIMESTAMPFREQ returns 100000000), so convert that to nanoseconds for consistency with other APIs
			*reinterpret_cast<uint64_t *>(static_cast<uint8_t *>(results) + i * stride) *= 10;
		}
	}

	return true;
}

HRESULT reshade::d3d9::device_impl::create_surface_replacement(const D3DSURFACE_DESC &desc, IDirect3DSurface9 **out_surface, HANDLE *out_shared_handle)
{
	// Cannot create multisampled textures
	if (desc.MultiSampleType != D3DMULTISAMPLE_NONE)
		return D3DERR_INVALIDCALL;

	// Surface will hold a reference to the created texture and keep it alive
	com_ptr<IDirect3DTexture9> texture;

	HRESULT hr = _orig->CreateTexture(desc.Width, desc.Height, 1, desc.Usage, desc.Format, desc.Pool, &texture, out_shared_handle);
	if (SUCCEEDED(hr))
		hr = IDirect3DTexture9_GetSurfaceLevel(texture.get(), 0, out_surface);

	return hr;
}
