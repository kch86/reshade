/*
 * Copyright (C) 2021 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "addon_manager.hpp"
#include "d3d9_impl_state_block.hpp"

namespace reshade::d3d9
{
	class device_impl : public api::api_object_impl<IDirect3DDevice9 *, api::device, api::command_queue, api::command_list>
	{
		friend class swapchain_impl;

	public:
		explicit device_impl(IDirect3DDevice9 *device);
		~device_impl();

		api::device_api get_api() const final { return api::device_api::d3d9; }

		bool check_capability(api::device_caps capability) const final;
		bool check_format_support(api::format format, api::resource_usage usage) const final;

		bool create_sampler(const api::sampler_desc &desc, api::sampler *out_handle) final;
		void destroy_sampler(api::sampler handle) final;

		bool create_resource(const api::resource_desc &desc, const api::subresource_data *initial_data, api::resource_usage initial_state, api::resource *out_handle, HANDLE *shared_handle = nullptr) final;
		void destroy_resource(api::resource handle) final;

		api::resource_desc get_resource_desc(api::resource resource) const final;

		bool create_resource_view(api::resource resource, api::resource_usage usage_type, const api::resource_view_desc &desc, api::resource_view *out_handle) final;
		void destroy_resource_view(api::resource_view handle) final;

		api::resource get_resource_from_view(api::resource_view view) const final;
		api::resource get_resource_from_view(api::resource_view view, uint32_t *out_subresource, uint32_t *out_levels = nullptr) const;
		api::resource_view_desc get_resource_view_desc(api::resource_view view) const final;

		bool map_buffer_region(api::resource resource, uint64_t offset, uint64_t size, api::map_access access, void **out_data) final;
		void unmap_buffer_region(api::resource resource) final;
		bool map_texture_region(api::resource resource, uint32_t subresource, const api::subresource_box *box, api::map_access access, api::subresource_data *out_data) final;
		void unmap_texture_region(api::resource resource, uint32_t subresource) final;

		void update_buffer_region(const void *data, api::resource resource, uint64_t offset, uint64_t size) final;
		void update_texture_region(const api::subresource_data &data, api::resource resource, uint32_t subresource, const api::subresource_box *box) final;

		bool create_pipeline(api::pipeline_layout layout, uint32_t subobject_count, const api::pipeline_subobject *subobjects, api::pipeline *out_handle) final;
		bool create_input_layout(uint32_t count, const api::input_element *desc, api::pipeline *out_handle);
		bool create_vertex_shader(const api::shader_desc &desc, api::pipeline *out_handle);
		bool create_pixel_shader(const api::shader_desc &desc, api::pipeline *out_handle);
		void destroy_pipeline(api::pipeline handle) final;

		bool create_pipeline_layout(uint32_t param_count, const api::pipeline_layout_param *params, api::pipeline_layout *out_handle) final;
		void destroy_pipeline_layout(api::pipeline_layout handle) final;

		bool allocate_descriptor_sets(uint32_t count, api::pipeline_layout layout, uint32_t layout_param, api::descriptor_set *out_sets) final;
		void free_descriptor_sets(uint32_t count, const api::descriptor_set *sets) final;

		void get_descriptor_pool_offset(api::descriptor_set set, uint32_t binding, uint32_t array_offset, api::descriptor_pool *out_pool, uint32_t *out_offset) const final;

		void copy_descriptor_sets(uint32_t count, const api::descriptor_set_copy *copies) final;
		void update_descriptor_sets(uint32_t count, const api::descriptor_set_update *updates) final;

		bool create_query_pool(api::query_type type, uint32_t size, api::query_pool *out_handle) final;
		void destroy_query_pool(api::query_pool handle) final;

		bool get_query_pool_results(api::query_pool heap, uint32_t first, uint32_t count, void *results, uint32_t stride) final;

		void set_resource_name(api::resource, const char *) final {}
		void set_resource_view_name(api::resource_view, const char *) final {}

		api::device *get_device() final { return this; }

		api::command_queue_type get_type() const final { return api::command_queue_type::graphics | api::command_queue_type::copy; }

		void wait_idle() const final { /* no-op */ }

		void flush_immediate_command_list(uint64_t *out_signal, uint64_t *out_fence) const final;

		api::command_list *get_immediate_command_list() final { return this; }

		void barrier(uint32_t, const api::resource *, const api::resource_usage *, const api::resource_usage *) final { /* no-op */ }

		void begin_render_pass(uint32_t count, const api::render_pass_render_target_desc *rts, const api::render_pass_depth_stencil_desc *ds) final;
		void end_render_pass() final;
		void bind_render_targets_and_depth_stencil(uint32_t count, const api::resource_view *rtvs, api::resource_view dsv) final;

		void bind_pipeline(api::pipeline_stage stages, api::pipeline pipeline) final;
		void bind_pipeline_states(uint32_t count, const api::dynamic_state *states, const uint32_t *values) final;
		void bind_viewports(uint32_t first, uint32_t count, const api::viewport *viewports) final;
		void bind_scissor_rects(uint32_t first, uint32_t count, const api::rect *rects) final;

		void push_constants(api::shader_stage stages, api::pipeline_layout layout, uint32_t layout_param, uint32_t first, uint32_t count, const void *values) final;
		void push_descriptors(api::shader_stage stages, api::pipeline_layout layout, uint32_t layout_param, const api::descriptor_set_update &update) final;
		void bind_descriptor_sets(api::shader_stage stages, api::pipeline_layout layout, uint32_t first, uint32_t count, const api::descriptor_set *sets) final;

		void bind_index_buffer(api::resource buffer, uint64_t offset, uint32_t index_size) final;
		void bind_vertex_buffers(uint32_t first, uint32_t count, const api::resource *buffers, const uint64_t *offsets, const uint32_t *strides) final;
		void bind_stream_output_buffers(uint32_t first, uint32_t count, const api::resource *buffers, const uint64_t *offsets, const uint64_t *max_sizes) final;

		void draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) final;
		void draw_indexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) final;
		void dispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) final;
		void draw_or_dispatch_indirect(api::indirect_command type, api::resource buffer, uint64_t offset, uint32_t draw_count, uint32_t stride) final;

		void copy_resource(api::resource source, api::resource dest) final;
		void copy_buffer_region(api::resource source, uint64_t source_offset, api::resource dest, uint64_t dest_offset, uint64_t size) final;
		void copy_buffer_to_texture(api::resource source, uint64_t source_offset, uint32_t row_length, uint32_t slice_height, api::resource dest, uint32_t dest_subresource, const api::subresource_box *dest_box) final;
		void copy_texture_region(api::resource source, uint32_t source_subresource, const api::subresource_box *source_box, api::resource dest, uint32_t dest_subresource, const api::subresource_box *dest_box, api::filter_mode filter) final;
		void copy_texture_to_buffer(api::resource source, uint32_t source_subresource, const api::subresource_box *source_box, api::resource dest, uint64_t dest_offset, uint32_t row_length, uint32_t slice_height) final;
		void resolve_texture_region(api::resource source, uint32_t source_subresource, const api::subresource_box *source_box, api::resource dest, uint32_t dest_subresource, int32_t dest_x, int32_t dest_y, int32_t dest_z, api::format format) final;

		void clear_depth_stencil_view(api::resource_view dsv, const float *depth, const uint8_t *stencil, uint32_t rect_count, const api::rect *rects) final;
		void clear_render_target_view(api::resource_view rtv, const float color[4], uint32_t rect_count, const api::rect *rects) final;
		void clear_unordered_access_view_uint(api::resource_view uav, const uint32_t values[4], uint32_t rect_count, const api::rect *rects) final;
		void clear_unordered_access_view_float(api::resource_view uav, const float values[4], uint32_t rect_count, const api::rect *rects) final;

		void generate_mipmaps(api::resource_view srv) final;

		void begin_query(api::query_pool pool, api::query_type type, uint32_t index) final;
		void end_query(api::query_pool pool, api::query_type type, uint32_t index, api::query_flags flags) final;
		void copy_query_pool_results(api::query_pool pool, api::query_type type, uint32_t first, uint32_t count, api::resource dest, uint64_t dest_offset, uint32_t stride) final;

		void begin_debug_event(const char *label, const float color[4]) final;
		void end_debug_event() final;
		void insert_debug_marker(const char *label, const float color[4]) final;

	protected:
		void on_init();
		void on_reset();

		HRESULT create_surface_replacement(const D3DSURFACE_DESC &desc, IDirect3DSurface9 **out_surface, HANDLE *out_shared_handle = nullptr);

		D3DPRIMITIVETYPE _current_prim_type = static_cast<D3DPRIMITIVETYPE>(0);
		IDirect3DVertexBuffer9 *_current_stream_output = nullptr;
		UINT _current_stream_output_offset = 0;

		D3DCAPS9 _caps = {};
		D3DDEVICE_CREATION_PARAMETERS _cp = {};
		com_ptr<IDirect3D9> _d3d;

	private:
		state_block _backup_state;
		com_ptr<IDirect3DStateBlock9> _copy_state;
		com_ptr<IDirect3DVertexBuffer9> _default_input_stream;
		com_ptr<IDirect3DVertexDeclaration9> _default_input_layout;
	};
}
