#include "timing.h"
#include <reshade.hpp>
#include <vector>
#include <d3d12.h>

using namespace reshade::api;

namespace timing
{
	static query_pool s_timer_pool;
	static uint32_t s_timer_count = 0;
	static uint32_t s_timer_alloc_index = 0;
	static double s_gpu_freq = 0;
	static std::vector<uint64_t> s_read_back_data;

	static uint64_t s_signal0, s_signal1;
	static ID3D12Fence *s_fence0 = nullptr;
	static ID3D12Fence *s_fence1 = nullptr;
	HANDLE _fence_event;

	void init(command_queue *cmd_queue, int timer_count)
	{
		cmd_queue->get_device()->create_query_pool(query_type::timestamp, timer_count * 2, &s_timer_pool);
		s_read_back_data.resize(timer_count * 2);
		s_timer_count = timer_count;

		ID3D12CommandQueue *native = reinterpret_cast<ID3D12CommandQueue *>(cmd_queue->get_native());

		uint64_t freq;
		HRESULT hr = native->GetTimestampFrequency(&freq);
		s_gpu_freq = 1.0 / double(freq);

		_fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	}

	void destroy(device *device)
	{
		device->destroy_query_pool(s_timer_pool);
		CloseHandle(_fence_event);
	}

	uint32_t alloc_timer_handle()
	{
		uint32_t timer = s_timer_alloc_index % s_timer_count;
		s_timer_alloc_index++;
		return timer;
	}

	void start_timer(command_list* cmd_list, uint32_t timer)
	{
		cmd_list->end_query(s_timer_pool, query_type::timestamp, timer * 2 + 0, query_flags::none);
	}

	void stop_timer(command_list *cmd_list, uint32_t timer)
	{
		cmd_list->end_query(s_timer_pool, query_type::timestamp, timer * 2 + 1, query_flags::none);
	}

	float get_timer_value(uint32_t timer)
	{
		uint64_t start = s_read_back_data[timer * 2 + 0];
		uint64_t end = s_read_back_data[timer * 2 + 1];

		if (start > end)
		{
			return 0.0f;
		}

		double seconds = (end - start) * s_gpu_freq;
		float ms = float(seconds * 1000.0);
		return ms;
	}

	void flush(command_list *cmd_list)
	{
		if (s_fence1)
		{
			if (FAILED(s_fence1->SetEventOnCompletion(s_signal1, _fence_event)))
			{
				return;
			}
			if (WaitForSingleObject(_fence_event, INFINITE) != WAIT_OBJECT_0)
			{
				return;
			}

			cmd_list->get_device()->get_query_pool_results(s_timer_pool, 0, s_timer_alloc_index * 2, s_read_back_data.data(), sizeof(uint64_t));
			cmd_list->copy_query_pool_results(s_timer_pool, query_type::timestamp, 0, s_timer_alloc_index * 2, resource{ 0 }, 0, sizeof(uint64_t));
		}		
	}

	void set_fence(uint64_t fence, uint64_t signal)
	{
		s_fence1 = s_fence0;
		s_signal1 = s_signal0;
		s_signal0 = signal;
		s_fence0 = reinterpret_cast<ID3D12Fence *>(fence);
	}
}
