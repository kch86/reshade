#pragma once

#include "addon.hpp"
#include "dxhelpers.h"
#include <reshade_api_resource.hpp>

namespace reshade::api
{
	struct device;
	struct command_list;
	struct command_queue;
	enum class format : uint32_t;
}

void doDeferredDeletes();

struct scopedresource : public reshade::api::resource
{
	using base = reshade::api::resource;
	scopedresource() = default;
	scopedresource(reshade::api::device *d, base res)
		: _device(d)
		, base(res)
	{

	}

	scopedresource(scopedresource &&other)
	{
		handle = other.handle;
		_device = other._device;
		other.handle = 0;
	}

	scopedresource &operator=(scopedresource &&other)
	{
		handle = other.handle;
		_device = other._device;
		other.handle = 0;
		return *this;
	}

	~scopedresource()
	{
		free();
	}

	void free();

	scopedresource(scopedresource &) = delete;
	scopedresource& operator=(scopedresource &) = delete;

	reshade::api::device *_device;
};

void createDxrDevice(reshade::api::device *device);
void testCompilePso(reshade::api::device *device);

struct BlasBuildDesc
{
	struct
	{
		uint64_t res;
		uint32_t offset;
		uint32_t count;
		uint32_t stride;
		reshade::api::format fmt;
	} vb;

	struct
	{
		uint64_t res;
		uint32_t offset;
		uint32_t count;
		reshade::api::format fmt;
	} ib;
};

struct TlasInstance
{
	reshade::api::resource bvh;
	float transform[3][4];
};

struct TlasBuildDesc
{
	span<reshade::api::rt_instance_desc> instances;
};


scopedresource buildBlas(reshade::api::device* d3d9On12Device,
				   reshade::api::command_list *cmdlist,
				   reshade::api::command_queue* cmdqueue,
				   const BlasBuildDesc& desc);

scopedresource buildTlas(reshade::api::command_list* cmdlist,
	reshade::api::command_queue *cmdqueue,
	const TlasBuildDesc &desc);

inline const DXGI_FORMAT to_native_d3d12(reshade::api::format value)
{
	return static_cast<DXGI_FORMAT>(value);
}

reshade::api::resource lock_resource(reshade::api::device *device9, reshade::api::command_queue* cmdqueue12, reshade::api::resource d3d9resource);
void unlock_resource(reshade::api::device *device9, uint64_t signal, ID3D12Fence* fence, reshade::api::resource d3d9resource);
