#pragma once

#include <superluminal/PerformanceAPI.h>

namespace profiling
{
#define PROFILE_BEGIN(name) PerformanceAPI_BeginEvent(name, nullptr, PERFORMANCEAPI_DEFAULT_COLOR)
#define PROFILE_BEGIN_DATA(name, data) PerformanceAPI_BeginEvent(name, data, PERFORMANCEAPI_DEFAULT_COLOR)
#define PROFILE_END() PerformanceAPI_EndEvent()

#define PROFILE_SCOPE(name) PERFORMANCEAPI_INSTRUMENT(name)
#define PROFILE_SCOPE_DATA(name, data) PERFORMANCEAPI_INSTRUMENT_DATA(name, data)
}
