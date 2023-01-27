#pragma once

#include <span>
#include <vector>
#include <unordered_map>
#include <DirectXMath.h>
#include "raytracing.h"

//forward declarations
struct RtInstanceData;

struct Material
{
	DirectX::XMVECTOR diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
	DirectX::XMVECTOR specular = { 0.0f, 0.0f, 0.0f, 0.0f };
};

class bvh_manager
{
public:
	struct AttachmentDesc
	{
		reshade::api::resource res;
		reshade::api::resource_type type;
		uint32_t offset = 0; // in elements
		uint32_t elem_offset = 0; // offset inside the stride
		uint32_t count = 0;  // in elements
		uint32_t stride = 0; // in bytes
		reshade::api::format fmt;
		bool view_as_raw = false;
	};

	struct DrawDesc
	{
		reshade::api::device *d3d9device;
		reshade::api::command_list *cmd_list;
		reshade::api::command_queue *cmd_queue;
		const BlasBuildDesc &blas_desc;
		DirectX::XMMATRIX &transform;
		std::span<AttachmentDesc> attachments = {};
		Material material = {};
		bool dynamic = false;
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
	std::pair<scopedresource, scopedresourceview> build_instance_data(reshade::api::command_list *cmd_list);

	std::span<scopedresource> get_bvhs() { return m_bvhs; }
	std::span<reshade::api::rt_instance_desc> get_instances() { return m_instances_flat; }
private:
	void prune_stale_geo();

	template<typename T>
	struct AttachmentT
	{
		struct Elem
		{
			T srv;
			uint32_t offset; // in elements
			uint32_t stride; // in bytes
			uint32_t fmt;
		};
		std::vector<Elem> data;
	};

	struct RtInstance
	{
		DirectX::XMMATRIX transform;
		Material mtrl;
	};

	using ScopedAttachment = AttachmentT<scopedresourceview>;
	using Attachment = AttachmentT<reshade::api::resource_view>;
	using GpuAttachment = AttachmentT<uint32_t>;

	std::vector<bool> m_needs_rebuild;
	std::vector<uint64_t> m_last_rebuild;
	std::vector<BlasBuildDesc> m_geometry;
	std::vector<scopedresource> m_bvhs;
	std::vector<std::vector<RtInstance>> m_instances;
	std::vector<ScopedAttachment> m_attachments;

	std::vector<reshade::api::rt_instance_desc> m_instances_flat;
	std::vector<Attachment> m_attachments_flat;
	std::vector<RtInstanceData> m_instance_data_flat;
	std::unordered_map<uint64_t, uint32_t> m_per_frame_instance_counts;

	uint64_t m_current_draw_stream_hash = 0;
	uint64_t m_frame_id = 0;
};
