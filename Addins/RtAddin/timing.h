#pragma once

#include <stdint.h>

namespace reshade::api
{
	struct device;
	struct command_list;
	struct command_queue;
}

namespace timing
{
	void init(reshade::api::command_queue* cmd_queue, int timer_count);
	void destroy(reshade::api::device *device);
	uint32_t alloc_timer_handle();
	void start_timer(reshade::api::command_list* cmd_list, uint32_t timer);
	void stop_timer(reshade::api::command_list* cmd_list, uint32_t timer);
	float get_timer_value(uint32_t timer);
	void flush(reshade::api::device* device);
	void set_fence(uint64_t fence, uint64_t signal);
}
