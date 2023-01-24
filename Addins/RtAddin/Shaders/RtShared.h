
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

using float3 = DirectX::XMFLOAT3;
using float4 = DirectX::XMVECTOR;
using float4x4 = DirectX::XMMATRIX;
using uint = uint32_t;
using uint3 = Uint3;

#endif

struct RtConstants
{
	float4x4 viewMatrix;

	float4 viewPos;

	float fov;
	uint usePrebuiltCamMat;
	uint showNormal;
	uint showUvs;

	uint showTexture;
	float3 sunDirection;
};

#endif
