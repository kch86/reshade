#include "sample_gen.h"
#include "dxhelpers.h"
#include <reshade.hpp>

#pragma warning( push )
#pragma warning( disable : 4146 )
#pragma warning( disable : 4244 )
#pragma warning( disable : 4018 )
#include <stochasticgen/sampling/ssobol.h>
#include <stochasticgen/sampling/ssobol.cpp>
#include <stochasticgen/sampling/bn_utils.h>
#include <stochasticgen/sampling/bn_utils.cpp>
#include <stochasticgen/sampling/shuffling.h>
#include <stochasticgen/sampling/shuffling.cpp>
#pragma warning( pop )

using namespace reshade::api;

namespace sample_gen
{
	std::pair<reshade::api::resource, reshade::api::resource_view>
	gen_sobol_sequence(device *dev, int dim, int sample_count)
	{
		const int total_sample_count = dim * sample_count;
		double *samples_d = new double[total_sample_count];

		sampling::GetStochasticSobolSamples(sample_count, dim, false, sampling::kBestCandidateSamples, true, samples_d);

#if 0
		float *samples = new float[total_sample_count];
		for (int i = 0; i < total_sample_count; i++)
		{
			samples[i] = (float)samples_d[i];
		}
#endif

		resource_desc desc(sizeof(float) * total_sample_count, memory_heap::cpu_to_gpu, resource_usage::shader_resource);

		/*subresource_data data;
		data.data = samples;
		data.row_pitch = (uint32_t)desc.buffer.size;*/

		resource d3d12res;
		ThrowIfFailed(dev->create_resource(
			desc,
			//&data, resource_usage::cpu_access, &d3d12res));
			nullptr, resource_usage::cpu_access, &d3d12res));

#if 1
		void *ptr;
		dev->map_buffer_region(d3d12res, 0, desc.buffer.size, map_access::write_only, &ptr);

		float *samples = (float *)ptr;
		for (int i = 0; i < total_sample_count; i++)
		{
			samples[i] = (float)samples_d[i];
		}
		dev->unmap_buffer_region(d3d12res);
#endif

		resource_view_desc view_desc(format::r32_float, 0, total_sample_count);

		resource_view srv{};
		dev->create_resource_view(d3d12res, resource_usage::shader_resource, view_desc, &srv);

		delete[] samples_d;
		//delete[] samples;

		return { d3d12res, srv };
	}

	void update_sobl_sequence(command_list *cmd_list, reshade::api::resource res)
	{
	}
}

