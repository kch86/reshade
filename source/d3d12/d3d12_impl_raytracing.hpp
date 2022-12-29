#pragma once

#include "addon.hpp"
#include "reshade_api_pipeline.hpp"
#include "d3d12_impl_type_convert.hpp"


namespace reshade::d3d12
{
	class device_impl;
	auto convert_rt_build_inputs(
		device_impl *d,
		const api::rt_build_acceleration_structure_inputs &input,
		span<D3D12_RAYTRACING_GEOMETRY_DESC> geom_desc_storage) -> D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS;

	auto convert_rt_build_desc(
		device_impl *d,
		const api::rt_build_acceleration_structure_desc &desc,
		span<D3D12_RAYTRACING_GEOMETRY_DESC> geom_desc_storage) -> D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC;

	void convert_rt_post_build_info_array(span<api::rt_acceleration_structure_postbuild_info_desc> input,
		span<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC> output);
}
