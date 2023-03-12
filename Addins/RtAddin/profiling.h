#pragma once

#include <superluminal/PerformanceAPI.h>
#include <tracy/public/tracy/Tracy.hpp>

#define USE_TRACY 0
#define USE_SL 1

namespace profiling
{
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
#define TRACY_SCOPE(name) ZoneScopedN(name)
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
