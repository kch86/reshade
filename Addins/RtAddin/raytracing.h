#pragma once

#include "dxhelpers.h"

namespace reshade::api
{
	struct device;
	struct command_list;
	enum class format : uint32_t;
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

struct AsBuffers
{
	ComPtr<ID3D12Resource> bvh;
	ComPtr<ID3D12Resource> scratch;
};

AsBuffers buildBvh(reshade::api::command_list *cmdlist, const BvhBuildDesc& desc);
