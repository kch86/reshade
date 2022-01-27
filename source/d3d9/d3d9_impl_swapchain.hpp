/*
 * Copyright (C) 2014 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#pragma once

#include "runtime.hpp"
#include "d3d9_impl_state_block.hpp"

namespace reshade::d3d9
{
	class device_impl;

	class swapchain_impl : public api::api_object_impl<IDirect3DSwapChain9 *, runtime>
	{
	public:
		swapchain_impl(device_impl *device, IDirect3DSwapChain9 *swapchain);
		~swapchain_impl();

		api::resource get_back_buffer(uint32_t index) final;

		uint32_t get_back_buffer_count() const final { return 1; }
		uint32_t get_current_back_buffer_index() const final { return 0; }

		bool on_init(const D3DPRESENT_PARAMETERS &pp);
		void on_reset();

		void on_present();

#if RESHADE_FX
		void render_effects(api::command_list *cmd_list, api::resource_view rtv, api::resource_view rtv_srgb) final;
#endif

	private:
		state_block _app_state;
		com_ptr<IDirect3DSurface9> _backbuffer;
	};
}
