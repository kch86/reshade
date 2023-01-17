#pragma once

#include <span>
#include <vector>
#include <unordered_map>
#include <DirectXMath.h>
#include "raytracing.h"

namespace reshade::api
{
	//struct resource;
}

class bvh_manager
{
public:
	struct DrawDesc
	{
		reshade::api::device *d3d9device;
		reshade::api::command_list *cmd_list;
		reshade::api::command_queue *cmd_queue;
		const BlasBuildDesc &blas_desc;
		DirectX::XMMATRIX &transform;
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

private:
	std::vector<BlasBuildDesc> s_geometry;
	std::vector<scopedresource> s_bvhs;
	std::vector<std::vector<DirectX::XMMATRIX>> s_instances;
	std::unordered_map<uint64_t, uint32_t> s_per_frame_instance_counts;

	uint64_t s_current_draw_stream_hash = 0;
};
