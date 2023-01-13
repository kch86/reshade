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
//
//// Subobjects definitions at library scope. 
//GlobalRootSignature MyGlobalRootSignature =
//{
//    "DescriptorTable( UAV( u0 ) ),"                        // Output texture
//    "SRV( t0 ),"                                           // Acceleration structure
//    "CBV( b0 ),"                                           // g_rtScene constants
//    "DescriptorTable( SRV( t1, numDescriptors = 2 ) )"     // Static index and vertex buffers.
//};
//
//LocalRootSignature MyLocalRootSignature = 
//{
//    "RootConstants( num32BitConstants = 4, b1 )"           // Cube constants        
//};
//
//TriangleHitGroup MyHitGroup =
//{
//    "",                     // AnyHit
//    "MyClosestHitShader",   // ClosestHit
//};
//
//SubobjectToExportsAssociation  MyLocalRootSignatureAssociation =
//{
//    "MyLocalRootSignature",  // subobject name
//    "MyHitGroup"             // export association 
//};
//
//RaytracingShaderConfig  MyShaderConfig =
//{
//    16, // max payload size
//    8   // max attribute size
//};
//
//RaytracingPipelineConfig MyPipelineConfig =
//{
//    1 // max trace recursion depth
//};


RaytracingAccelerationStructure g_rtScene : register(t0);
RWTexture2D<float4> g_rtOutput : register(u0);

cbuffer RtConstants : register(b0)
{
	float4x4 g_viewMatrix;

	float4 g_viewPos;

	float g_fov;
	uint g_usePrebuiltCamMat;
	uint2 pad0;
}

float3 genRayDir(uint3 tid, float2 dims)
{
	float2 crd = float2(tid.xy);
	//float2 dims = float2(launchDim.xy);

	float2 d = ((crd / dims) * 2.f - 1.f);
	float aspectRatio = dims.x / dims.y;

	return normalize(float3(d.x * aspectRatio * g_fov, -d.y, -1));
}

uint MurmurMix(uint Hash)
{
	Hash ^= Hash >> 16;
	Hash *= 0x85ebca6b;
	Hash ^= Hash >> 13;
	Hash *= 0xc2b2ae35;
	Hash ^= Hash >> 16;
	return Hash;
}

float3 IntToColor(uint Index)
{
	uint Hash = MurmurMix(Index);

	float3 Color = float3
	(
		(Hash >> 0) & 255,
		(Hash >> 8) & 255,
		(Hash >> 16) & 255
	);

	return Color * (1.0f / 255.0f);
}

float3 hsv2rgb(float h, float s, float v)
{
	float3 _HSV = float3(h, s, v);
	_HSV.x = fmod(100.0 + _HSV.x, 1.0);                                       // Ensure [0,1[

	float   HueSlice = 6.0 * _HSV.x;                                            // In [0,6[
	float   HueSliceInteger = floor(HueSlice);
	float   HueSliceInterpolant = HueSlice - HueSliceInteger;                   // In [0,1[ for each hue slice

	float3  TempRGB = float3(_HSV.z * (1.0 - _HSV.y),
		_HSV.z * (1.0 - _HSV.y * HueSliceInterpolant),
		_HSV.z * (1.0 - _HSV.y * (1.0 - HueSliceInterpolant)));

	// The idea here to avoid conditions is to notice that the conversion code can be rewritten:
	//    if      ( var_i == 0 ) { R = V         ; G = TempRGB.z ; B = TempRGB.x }
	//    else if ( var_i == 2 ) { R = TempRGB.x ; G = V         ; B = TempRGB.z }
	//    else if ( var_i == 4 ) { R = TempRGB.z ; G = TempRGB.x ; B = V     }
	// 
	//    else if ( var_i == 1 ) { R = TempRGB.y ; G = V         ; B = TempRGB.x }
	//    else if ( var_i == 3 ) { R = TempRGB.x ; G = TempRGB.y ; B = V     }
	//    else if ( var_i == 5 ) { R = V         ; G = TempRGB.x ; B = TempRGB.y }
	//
	// This shows several things:
	//  . A separation between even and odd slices
	//  . If slices (0,2,4) and (1,3,5) can be rewritten as basically being slices (0,1,2) then
	//      the operation simply amounts to performing a "rotate right" on the RGB components
	//  . The base value to rotate is either (V, B, R) for even slices or (G, V, R) for odd slices
	//
	float   IsOddSlice = fmod(HueSliceInteger, 2.0);                          // 0 if even (slices 0, 2, 4), 1 if odd (slices 1, 3, 5)
	float   ThreeSliceSelector = 0.5 * (HueSliceInteger - IsOddSlice);          // (0, 1, 2) corresponding to slices (0, 2, 4) and (1, 3, 5)

	float3  ScrollingRGBForEvenSlices = float3(_HSV.z, TempRGB.zx);           // (V, Temp Blue, Temp Red) for even slices (0, 2, 4)
	float3  ScrollingRGBForOddSlices = float3(TempRGB.y, _HSV.z, TempRGB.x);  // (Temp Green, V, Temp Red) for odd slices (1, 3, 5)
	float3  ScrollingRGB = lerp(ScrollingRGBForEvenSlices, ScrollingRGBForOddSlices, IsOddSlice);

	float   IsNotFirstSlice = saturate(ThreeSliceSelector);                   // 1 if NOT the first slice (true for slices 1 and 2)
	float   IsNotSecondSlice = saturate(ThreeSliceSelector - 1.0);              // 1 if NOT the first or second slice (true only for slice 2)

	return  lerp(ScrollingRGB.xyz, lerp(ScrollingRGB.zxy, ScrollingRGB.yzx, IsNotSecondSlice), IsNotFirstSlice);
}

float3 instanceIdToColor(uint id)
{
	/*float r = id < 256 ? 255 - id : id < 512 ? 0 : id - 512;
	float g = id < 256 ? id : id < 512 ? 512 - id : 0;
	float b = id < 256 ? 0 : id < 512 ? id - 256 : 768 - id;

	return float3(r, g, b) / 255.0;*/

	/*float hue = ((id * 13) % 360) / 360.0;
	return hsv2rgb(hue, 0.7, 0.99);*/
	return IntToColor(id);
}

[numthreads(8, 8, 1)]
void ray_gen(uint3 tid : SV_DispatchThreadID)
{
    //trace
#if 1

	uint width, height;
	g_rtOutput.GetDimensions(width, height);

	 // a. Configure
	//RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
	RayQuery<RAY_FLAG_FORCE_OPAQUE> query;

	uint ray_flags = 0; // Any this ray requires in addition those above.
	uint ray_instance_mask = 0xffffffff;

	float3 raydir = genRayDir(tid, float2(width, height));
	raydir = mul(float4(raydir, 0.0), g_viewMatrix).xyz;

	float3 rayorigin = g_viewPos.xyz;

	if (g_usePrebuiltCamMat)
	{
		/*{
			float4 ndc = float4(0.0.xx, 0.0, 1.0);
			float4 worldpos = mul(ndc, g_viewMatrix);
			worldpos.xyz /= worldpos.w;

			rayorigin = worldpos.xyz;
		}*/

		//get far end of the ray
		float2 d = (((float2)tid.xy / float2(width, height)) * 2.f - 1.f);
		float4 ndc = float4(d.x, -d.y, 1.0, 1.0);
		float4 worldpos = mul(ndc, g_viewMatrix);
		worldpos.xyz /= worldpos.w;

		raydir = normalize(worldpos.xyz - rayorigin);
	}

	// b. Initialize  - hardwired here to deliver minimal sample code.
	RayDesc ray;
	ray.TMin = 1e-5f;
	ray.TMax = 1e10f;
	ray.Origin = rayorigin;
	ray.Direction = raydir;

	query.TraceRayInline(g_rtScene, ray_flags, ray_instance_mask, ray);

	// c. Cast 

	// Proceed() is where behind-the-scenes traversal happens, including the heaviest of any driver inlined code.
	// In this simplest of scenarios, Proceed() only needs to be called once rather than a loop.
	// Based on the template specialization above, traversal completion is guaranteed.

	float4 value = float4(1.0, 0.2, 1.0, 1.0);

	const uint MaxIter = 512;
	uint iter = 0;
	while (query.Proceed() && iter < MaxIter)
	{
		// d. Examine and act on the result of the traversal.

		if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		{
			// TODO: Grab ray parameters & sample accordingly.

			/* ShadeMyTriangleHit(
				query.CommittedInstanceIndex(),
				query.CommittedPrimitiveIndex(),
				query.CommittedGeometryIndex(),
				query.CommittedRayT(),
				query.CommittedTriangleBarycentrics(),
				query.CommittedTriangleFrontFace() );*/

			value = float4(0, 1, 0, 1);
			//TODO: call commit hit here
			break;
		}
		iter++;
	}

	if (iter > 0)
	{
		value = 1.0;
	}
	else if (iter == 0)
	{
		value = float4(0, 0, 0, 1);
	}

	/*if (g_viewMatrix[0][0] == 1.0 && g_viewMatrix[1][1] == 1.0 && g_viewMatrix[2][2])
	{
		value = float4(1, 0, 0, 1);
	}*/

	if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		value.rgb = instanceIdToColor(query.CommittedInstanceID());
	}
#else
	float4 value = float4(1.0, 0.2, 1.0, 1.0);
#endif

    // Write the raytraced color to the output texture.
    //g_rtOutput[tid.xy] = float4(1.0, 0.2, 0.2, 1.0);
	g_rtOutput[tid.xy] = value;
}


#endif // RAYTRACING_HLSL
