//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#ifndef RAYTRACING_HLSL
#define RAYTRACING_HLSL

#define HLSL
#include "RaytracingHlslCompat.h"

Texture2D<float4> texture : register(t0);

sampler sampler0 : register(s0);


float4 blit_ps(in float2 texcoord : TEXCOORD) : SV_TARGET0
{
	return float4(texture.Sample(sampler0, texcoord).rgb, 1.0);
}


#endif // RAYTRACING_HLSL
