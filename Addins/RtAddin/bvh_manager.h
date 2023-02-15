#pragma once

#include <span>
#include <vector>
#include <unordered_map>
#include <DirectXMath.h>
#include "raytracing.h"
#include "Shaders/RtShared.h"

//forward declarations
struct RtInstanceData;

struct Material
{
	DirectX::XMVECTOR diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
	DirectX::XMVECTOR specular = { 1.0f, 1.0f, 1.0f, 1.0f };
	float roughness = 0.8f;
	MaterialType type = MaterialType::Material_Standard;
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
		bool is_static = false;
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
	template<typename T>
	struct AttachmentT
	{
		struct Elem
		{
			T srv;
			reshade::api::resource orig_res;
			uint32_t offset; // in elements
			uint32_t stride; // in bytes
			uint32_t fmt;
		};
		std::vector<Elem> data;
	};

	struct RtInstance
	{
		DirectX::XMMATRIX transform;
		DirectX::XMMATRIX prev_transform;
		Material mtrl;
	};

	struct GeometryState
	{
		uint32_t last_visible;
		uint32_t last_rebuild;
		bool needs_rebuild;
		bool dynamic;
	};

	using ScopedAttachment = AttachmentT<scopedresourceview>;
	using Attachment = AttachmentT<reshade::api::resource_view>;
	using GpuAttachment = AttachmentT<uint32_t>;

	void prune_stale_geo();
	ScopedAttachment build_attachment(reshade::api::command_list *cmd_list, std::span<AttachmentDesc> attachments);
	bool attachment_is_dirty(const ScopedAttachment &stored, std::span<AttachmentDesc> attachments);

	std::vector<GeometryState> m_geo_state;
	std::vector<BlasBuildDesc> m_geometry;
	std::vector<scopedresource> m_bvhs;
	std::vector<std::vector<RtInstance>> m_instances;
	std::vector<ScopedAttachment> m_attachments;

	std::vector<reshade::api::rt_instance_desc> m_instances_flat;
	std::vector<Attachment> m_attachments_flat;
	std::vector<RtInstanceData> m_instance_data_flat;
	std::unordered_map<uint64_t, uint32_t> m_per_frame_instance_counts;

	uint64_t m_current_draw_stream_hash = 0;
	uint32_t m_frame_id = 0;
};
