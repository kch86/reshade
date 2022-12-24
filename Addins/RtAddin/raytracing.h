#pragma once

#include "dxhelpers.h"

namespace reshade::api
{
	struct device;
	struct command_list;
}

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
	} vb;

	struct
	{
		uint64_t res;
		uint32_t offset;
		uint32_t count;
		uint32_t stride;
	} ib;
};

struct AsBuffers
{
	ComPtr<ID3D12Resource> bvh;
	ComPtr<ID3D12Resource> scratch;
};

AsBuffers buildBvh(reshade::api::command_list *cmdlist, const BvhBuildDesc& desc);
