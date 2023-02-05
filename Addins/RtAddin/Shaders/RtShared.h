
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
};

struct RtInstanceData
{
	float4 diffuse;
	float4 specular;
	row_major float3x4 toWorldPrevT;
	float roughness;
};

struct RtConstants
{
	float4x4 viewMatrix;

	float4 viewPos;

	uint showNormal;
	uint showUvs;
	uint showTexture;
	uint showShaded;

	float3 sunDirection;
	float sunIntensity;

	uint pathCount;
	uint iterCount;
	uint frameIndex;
	float bounceBoost;

	uint showMotionVec;
	float sunRadius;
	uint2 pad;
};

#endif
