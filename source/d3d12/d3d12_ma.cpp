#include <d3d12.h>
#include <D3D12MemAlloc.h>

#ifdef UINT64
#undef UINT64
#define UINT64 uint64_t
#endif

#pragma warning( push )
#pragma warning( disable : 4100 )
#include <D3D12MemAlloc.cpp>
#pragma warning( pop )
