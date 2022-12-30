/*
 * Copyright (C) 2021 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "d3d9_impl_device.hpp"

void reshade::d3d9::device_impl::flush_immediate_command_list(uint64_t *out_signal, uint64_t *out_fence) const
{
	(void)out_signal;
	(void)out_fence;
}
