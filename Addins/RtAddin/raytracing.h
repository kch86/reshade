#pragma once

#include "addon.hpp"
#include "dxhelpers.h"
#include <reshade_api_resource.hpp>
#include <span>

namespace reshade::api
{
	struct device;
	struct command_list;
	struct command_queue;
	struct rt_instance_desc;
	enum class format : uint32_t;
}

void doDeferredDeletes();
void deferDestroyHandle(reshade::api::device *device, reshade::api::resource res);
void deferDestroyHandle(reshade::api::device *device, reshade::api::resource_view view);

template<class T> 
struct delayFreedHandle
{
	delayFreedHandle() = default;
	delayFreedHandle(reshade::api::device *d, T res)
		: _device(d)
		, _handle(res)
	{

	}

	delayFreedHandle(delayFreedHandle<T> &&other)
	{
		_handle = other._handle;
		_device = other._device;
		other._handle.handle = 0;
		other._device = nullptr;
	}

	delayFreedHandle<T> &operator=(delayFreedHandle<T> &&other)
	{
		_handle = other._handle;
		_device = other._device;
		other._handle.handle = 0;
		other._device = nullptr;
		return *this;
	}

	~delayFreedHandle()
	{
		free();
	}

	void free()
	{
		if (_handle.handle)
		{
			deferDestroyHandle(_device, _handle);
		}
	}

	T& handle() { return _handle;  }

private:

	delayFreedHandle(delayFreedHandle<T> &) = delete;
	delayFreedHandle<T> &operator=(delayFreedHandle<T> &) = delete;

	T _handle = { 0 };
	reshade::api::device *_device = nullptr;
};

using scopedresource = delayFreedHandle<reshade::api::resource>;
using scopedresourceview = delayFreedHandle<reshade::api::resource_view>;

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
	std::span<reshade::api::rt_instance_desc> instances;
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
