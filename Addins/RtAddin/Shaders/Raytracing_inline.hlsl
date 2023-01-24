#ifndef RAYTRACING_HLSL
#define RAYTRACING_HLSL

#include "RtShared.h"

struct AttachElem
{
	uint id;
	uint offset; // uint bytes
	uint stride; // bytes
	uint format;
};

struct Attachments
{
	AttachElem ib;
	AttachElem vb;
	AttachElem uv;
	AttachElem tex0;
};

RaytracingAccelerationStructure g_rtScene : register(t0, space0);
StructuredBuffer<Attachments> g_attachments_buffer : register(t0, space1);

RWTexture2D<float4> g_rtOutput : register(u0);

cbuffer constants : register(b0)
{
	RtConstants g_constants;
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

	return normalize(float3(d.x * aspectRatio * g_constants.fov, -d.y, -1));
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

float2 fetchUvs(uint instance_id, uint primitive_id, float2 barries)
{
	Attachments att = g_attachments_buffer[instance_id];

	if (att.uv.id == 0x7FFFFFFF)
	{
		return 1.0.xx;
	}

	// since vb streams are interleaved, this needs to be a byte address buffer
	ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(att.uv.id)];
	Buffer<uint> ib = ResourceDescriptorHeap[NonUniformResourceIndex(att.ib.id)];

	uint3 indices = uint3(
		ib[primitive_id * 3 + 0],
		ib[primitive_id * 3 + 1],
		ib[primitive_id * 3 + 2]);

	uint stride = att.uv.stride;
	float2 u0 = asfloat(vb.Load2(indices.x * stride + att.uv.offset));
	float2 u1 = asfloat(vb.Load2(indices.y * stride + att.uv.offset));
	float2 u2 = asfloat(vb.Load2(indices.z * stride + att.uv.offset));

	float2 uv = u0 * (1.0 - barries.y - barries.x) + u1 * barries.x + u2 * barries.y;
	uv = frac(uv);

	return uv;
}

float3 fetchTexture(uint instance_id, float2 uv)
{
	Attachments att = g_attachments_buffer[instance_id];

	if (att.tex0.id == 0x7FFFFFFF)
	{
		return 1.0.xxx;
	}

	Texture2D<float4> tex0 = ResourceDescriptorHeap[NonUniformResourceIndex(att.tex0.id)];

	uint width, height;
	tex0.GetDimensions(width, height);

	uint2 index = uint2(uv * float2(width, height) + 0.5);

	return tex0[index].rgb;
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
	raydir = mul(float4(raydir, 0.0), g_constants.viewMatrix).xyz;

	float3 rayorigin = g_constants.viewPos.xyz;

	if (g_constants.usePrebuiltCamMat)
	{
		//get far end of the ray
		float2 d = (((float2)tid.xy / float2(width, height)) * 2.f - 1.f);
		float4 ndc = float4(d.x, -d.y, 1.0, 1.0);
		float4 worldpos = mul(ndc, g_constants.viewMatrix);
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

	bool miss = true;
	if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		if (g_constants.showNormal)
		{
			value.rgb = fetchNormal(query.CommittedInstanceID(),
				query.CommittedPrimitiveIndex(),
				(float3x3)query.CommittedObjectToWorld3x4());
		}
		else if (g_constants.showUvs)
		{
			float2 uvs = fetchUvs(query.CommittedInstanceID(),
				query.CommittedPrimitiveIndex(),
				query.CommittedTriangleBarycentrics());
			value.rgb = float3(uvs, 0.0);
		}
		else if (g_constants.showTexture)
		{
			float2 uvs = fetchUvs(query.CommittedInstanceID(),
								  query.CommittedPrimitiveIndex(),
								  query.CommittedTriangleBarycentrics());

			value.rgb = fetchTexture(query.CommittedInstanceID(), uvs);
		}
		else
		{
			value.rgb = instanceIdToColor(query.CommittedInstanceID());
		}
		miss = false;
	}

	if (!miss)
	{
		ray.Origin = ray.Origin + ray.Direction * query.CommittedRayT();
		ray.Direction = normalize(g_constants.sunDirection);
		ray_flags |= RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
		query.TraceRayInline(g_rtScene, ray_flags, ray_instance_mask, ray);
		query.Proceed();

		if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		{
			value.rgb = 0.0;
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
