#pragma once

#include <superluminal/PerformanceAPI.h>
#include <tracy/public/tracy/Tracy.hpp>

#define USE_TRACY 1
#define USE_SL 1

namespace profiling
{
#define PROFILE_CAT_IMPL(a, b) a##b	
#define PROFILE_CAT(a, b) PROFILE_CAT_IMPL(a, b)	
#define PROFILE_UNIQUE_IDENTIFIER(a) PROFILE_CAT(a, __LINE__)

#define PROFILE_BEGIN(name) PerformanceAPI_BeginEvent(name, nullptr, PERFORMANCEAPI_DEFAULT_COLOR)
#define PROFILE_BEGIN_DATA(name, data) PerformanceAPI_BeginEvent(name, data, PERFORMANCEAPI_DEFAULT_COLOR)
#define PROFILE_END() PerformanceAPI_EndEvent()

#if USE_SL
#define SL_SCOPE(name) PERFORMANCEAPI_INSTRUMENT(name)
#define SL_SCOPE_DATA(name, data) PERFORMANCEAPI_INSTRUMENT_DATA(name, data)
#define SL_FRAME_END
#else
#define SL_SCOPE(name)
#define SL_SCOPE_DATA(name, data)
#define SL_FRAME_END
#endif

#if USE_TRACY
#define TRACY_SCOPE(name) ZoneNamedN(PROFILE_UNIQUE_IDENTIFIER(_profiler_var_), name, true)
#define TRACY_FRAME_END FrameMark
#else
#define TRACY_SCOPE(name)
#define TRACY_FRAME_END
#endif

#define PROFILE_SCOPE(name)	\
	SL_SCOPE(name)			\
	TRACY_SCOPE(name)

#define PROFILE_SCOPE_DATA(name, data)	\
	SL_SCOPE_DATA(name, data)			\
	TRACY_SCOPE(name)

#define PROFILE_END_FRAME()	\
	SL_FRAME_END			\
	TRACY_FRAME_END
}
