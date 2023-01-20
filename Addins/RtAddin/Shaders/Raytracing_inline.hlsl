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

struct AttachElem
{
	uint id;
	uint offset; // uint bytes
	uint stride; // bytes
	uint format;
};

struct Attachments
{
	AttachElem vb;
	AttachElem ib;
};

RaytracingAccelerationStructure g_rtScene : register(t0, space0);
Buffer<uint> g_instance_buffer : register(t0, space1);
StructuredBuffer<Attachments> g_attachments_buffer : register(t1, space1);
RWTexture2D<float4> g_rtOutput : register(u0);

cbuffer RtConstants : register(b0)
{
	float4x4 g_viewMatrix;

	float4 g_viewPos;

	float g_fov;
	uint g_usePrebuiltCamMat;
	uint g_useIdBuffer;
	uint g_showNormal;
}

//template <typename T> T load_buffer_elem_t(uint handle, uint byteOffset)
//{
//	//T result = byteBufferUniform(g_bindingsOffset.bindingsOffset).Load<T>(0);
//	T result = ResourceDescriptorHeap[handle].Load<T>(byteOffset);
//	return result;
//}
uint load_buffer_elem(uint handle, uint byteOffset)
{
	Buffer<uint> b = ResourceDescriptorHeap[handle];
	uint result = b.Load(byteOffset);

	return result;
}

uint load_buffer_elem_nonuniform(uint handle, uint byteOffset)
{
	Buffer<uint> b = ResourceDescriptorHeap[NonUniformResourceIndex(handle)];
	uint result = b.Load(byteOffset);

	return result;
}

float3 genRayDir(uint3 tid, float2 dims)
{
	float2 crd = float2(tid.xy);

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

float3 instanceIdToColor(uint id)
{
	if (g_useIdBuffer)
	{
		uint handle = g_instance_buffer[id];
		id = load_buffer_elem_nonuniform(handle, 0);
	}

	return IntToColor(id);
}

float3 fetchNormal(uint instance_id, uint primitive_id, float3x3 transform)
{
	Attachments att = g_attachments_buffer[instance_id];

	// since vb streams are interleaved, this needs to be a byte address buffer
	ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(att.vb.id)];
	Buffer<uint> ib = ResourceDescriptorHeap[NonUniformResourceIndex(att.ib.id)];

	uint3 indices = uint3(
		ib[primitive_id * 3 + 0],
		ib[primitive_id * 3 + 1],
		ib[primitive_id * 3 + 2]);

	uint stride = att.vb.stride;
	float3 v0 = asfloat(vb.Load3(indices.x * stride + att.vb.offset));
	float3 v1 = asfloat(vb.Load3(indices.y * stride + att.vb.offset));
	float3 v2 = asfloat(vb.Load3(indices.z * stride + att.vb.offset));

	float3 n = normalize(cross((v1 - v0), (v2 - v0)));
	n = mul(transform, n);
	return n * 0.5 + 0.5;
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

	if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		if (g_showNormal)
		{
			value.rgb = fetchNormal(query.CommittedInstanceID(),
								    query.CommittedPrimitiveIndex(),
									(float3x3)query.CommittedObjectToWorld3x4());
		}
		else
		{
			value.rgb = instanceIdToColor(query.CommittedInstanceID());
		}		
	}
#else
	float4 value = float4(1.0, 0.2, 1.0, 1.0);
#endif

    // Write the raytraced color to the output texture.
    //g_rtOutput[tid.xy] = float4(1.0, 0.2, 0.2, 1.0);
	g_rtOutput[tid.xy] = value;
}


#endif // RAYTRACING_HLSL
