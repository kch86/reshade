
#ifndef RT_SHARED_H
#define RT_SHARED_H

#if defined(__cplusplus)
#include <DirectXMath.h>

struct Uint3
{
	uint32_t x, y, z;

};

struct Uint2
{
	uint32_t x, y;
};

using float2 = DirectX::XMFLOAT2;
using float3 = DirectX::XMFLOAT3;
using float4 = DirectX::XMVECTOR;
using float4x4 = DirectX::XMMATRIX;
using float3x4 = DirectX::XMFLOAT3X4;
using uint = uint32_t;
using uint2 = Uint2;
using uint3 = Uint3;
#define row_major

#endif

enum DebugViewEnum : uint
{
	DebugView_None=0,
	DebugView_InstanceId,
	DebugView_Normals,
	DebugView_Uvs,
	DebugView_Texture,
	DebugView_Color,
	DebugView_Motion,
	DebugView_Count,
};

enum InstanceMask : uint
{
	InstanceMask_opaque = 1 << 0,
	InstanceMask_transparent = 1 << 1,
	InstanceMask_alphatest = 1 << 2,

	InstanceMask_opaque_alphatest = (InstanceMask_opaque | InstanceMask_alphatest),
	InstanceMask_all = (InstanceMask_opaque | InstanceMask_transparent | InstanceMask_alphatest),
};
struct RtInstanceAttachElem
{
	uint id;
	uint offset; // in bytes, for buffers this is a multiple of uint
	uint stride; // bytes
	uint format;
};

struct RtInstanceAttachments
{
	RtInstanceAttachElem ib;
	RtInstanceAttachElem vb;
	RtInstanceAttachElem uv;
	RtInstanceAttachElem norm;
	RtInstanceAttachElem tex0;
	RtInstanceAttachElem col;
};

struct RtInstanceData
{
	float4 diffuse;
	float4 specular;
	row_major float3x4 toWorldPrevT;
	float roughness;
	uint flags;
};

inline bool instance_is_opaque(RtInstanceData instance)
{
	return instance.flags == 1;
}

struct RtConstants
{
	float4x4 viewMatrix;

	float4 viewPos;

	DebugViewEnum debugView;
	uint pathCount;
	uint iterCount;
	uint frameIndex;

	float3 sunDirection;
	float sunIntensity;

	float bounceBoost;
	float sunRadius;
	uint transparentEnable;
	uint pad;
};

#endif
