/*
 * Copyright (C) 2021 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "vulkan_impl_command_list_immediate.hpp"

namespace reshade::vulkan
{
	class command_queue_impl : public api::api_object_impl<VkQueue, api::command_queue>
	{
	public:
		command_queue_impl(device_impl *device, uint32_t queue_family_index, const VkQueueFamilyProperties &queue_family, VkQueue queue);
		~command_queue_impl();

		api::device *get_device() final;

		api::command_queue_type get_type() const final;

		void wait_idle() const final;

		void flush_immediate_command_list() const { flush_immediate_command_list(nullptr, nullptr); }
		void flush_immediate_command_list(uint64_t *out_signal, uint64_t *out_fence) const final;
		void flush_immediate_command_list(VkSemaphore *wait_semaphores, uint32_t &num_wait_semaphores) const;

		api::command_list *get_immediate_command_list() final { return _immediate_cmd_list; }

		void begin_debug_event(const char *label, const float color[4]) final;
		void end_debug_event() final;
		void insert_debug_marker(const char *label, const float color[4]) final;

	private:
		device_impl *const _device_impl;
		command_list_immediate_impl *_immediate_cmd_list = nullptr;
		const VkQueueFlags _queue_flags;
	};
}
