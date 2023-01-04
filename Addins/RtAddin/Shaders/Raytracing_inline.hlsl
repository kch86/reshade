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
//    "CBV( b0 ),"                                           // Scene constants
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


RaytracingAccelerationStructure Scene : register(t0, space0);
RWTexture2D<float4> RenderTarget : register(u0);

float3 genRayDir(uint3 tid, float2 dims)
{
	float2 crd = float2(tid.xy);
	//float2 dims = float2(launchDim.xy);

	float2 d = ((crd / dims) * 2.f - 1.f);
	float aspectRatio = dims.x / dims.y;

	return normalize(float3(d.x * aspectRatio, -d.y, -1));
}

[numthreads(8, 8, 1)]
void ray_gen(uint3 tid : SV_DispatchThreadID)
{
    //trace
#if 1

	uint width, height;
	RenderTarget.GetDimensions(width, height);

	 // a. Configure
	//RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
	RayQuery<RAY_FLAG_NONE> query;

	uint ray_flags = 0; // Any this ray requires in addition those above.
	uint ray_instance_mask = 0xffffffff;

	// b. Initialize  - hardwired here to deliver minimal sample code.
	RayDesc ray;
	ray.TMin = 1e-5f;
	ray.TMax = 1e10f;
	ray.Origin = float3(0, 0, -1);
	ray.Direction = genRayDir(tid, float2(width, height));
	query.TraceRayInline(Scene, ray_flags, ray_instance_mask, ray);

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
#else
	float4 value = float4(1.0, 0.2, 1.0, 1.0);
#endif

    // Write the raytraced color to the output texture.
    //RenderTarget[tid.xy] = float4(1.0, 0.2, 0.2, 1.0);
	RenderTarget[tid.xy] = value;
}


#endif // RAYTRACING_HLSL
