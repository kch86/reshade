#pragma once

#include "dxhelpers.h"
#include <reshade_api_resource.hpp>

namespace reshade::api
{
	struct device;
	struct command_list;
}

namespace sample_gen
{
	std::pair<reshade::api::resource, reshade::api::resource_view>
	gen_sobol_sequence(reshade::api::device *dev, int dim, int sample_count);

	void
	update_sobl_sequence(reshade::api::command_list *cmd_list, reshade::api::resource res);
}

