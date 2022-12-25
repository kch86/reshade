#pragma once

#include "dxhelpers.h"
#include <reshade_api_resource.hpp>

namespace reshade::api
{
	struct device;
	struct command_list;
	struct command_queue;
	enum class format : uint32_t;
}

struct scopedresource : public reshade::api::resource
{
	using base = reshade::api::resource;
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

	~scopedresource();

	scopedresource(scopedresource &) = delete;
	scopedresource& operator=(scopedresource &) = delete;

	reshade::api::device *_device;
};

void createDxrDevice(reshade::api::device *device);
void testCompilePso(reshade::api::device *device);

struct BvhBuildDesc
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

scopedresource buildBvh(reshade::api::device* d3d9On12Device,
				   reshade::api::command_list *cmdlist,
				   reshade::api::command_queue* cmdqueue,
				   const BvhBuildDesc& desc);

inline const DXGI_FORMAT to_native_d3d12(reshade::api::format value)
{
	return static_cast<DXGI_FORMAT>(value);
}
