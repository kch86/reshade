#pragma once

#include <span>
#include <vector>
#include <unordered_map>
#include <DirectXMath.h>
#include "raytracing.h"

class bvh_manager
{
public:
	struct Attachment
	{
		reshade::api::resource res;
		uint32_t offset;
		uint32_t count;
		uint32_t stride;
		reshade::api::format fmt;
	};

	struct DrawDesc
	{
		reshade::api::device *d3d9device;
		reshade::api::command_list *cmd_list;
		reshade::api::command_queue *cmd_queue;
		const BlasBuildDesc &blas_desc;
		DirectX::XMMATRIX &transform;
		std::span<Attachment> attachments = {};
	};
public:
	bvh_manager() = default;
	~bvh_manager() = default;

	void update();
	void destroy();
	void update_vbs(std::span<const reshade::api::resource> buffers);
	void on_geo_updated(reshade::api::resource res);
	void on_geo_draw(DrawDesc& desc);
	scopedresource build_tlas(DirectX::XMMATRIX *base_transform, reshade::api::command_list *cmd_list, reshade::api::command_queue *cmd_queue);
	std::pair<scopedresource, scopedresourceview> build_attachments(reshade::api::command_list *cmd_list);

	std::span<scopedresource> get_bvhs() { return m_bvhs; }
	std::span<reshade::api::rt_instance_desc> get_instances() { return m_instances_flat; }
private:
	std::vector<BlasBuildDesc> m_geometry;
	std::vector<scopedresource> m_bvhs;
	std::vector<std::vector<DirectX::XMMATRIX>> m_instances;
	std::vector<scopedresourceview> m_attachments;


	std::vector<reshade::api::rt_instance_desc> m_instances_flat;
	std::vector<reshade::api::resource_view> m_attachments_flat;
	std::unordered_map<uint64_t, uint32_t> m_per_frame_instance_counts;

	uint64_t s_current_draw_stream_hash = 0;
};
