#include "RtShared.h"
#include "Raytracing.h"
#include "Color.h"
#include "Material.h"
#include "Sampling.h"


struct Shade
{
	float3 diffuse_radiance;
	float3 specular_radiance;
};

RaytracingAccelerationStructure g_rtScene : register(t0, space0);
StructuredBuffer<RtInstanceAttachments> g_attachments_buffer : register(t0, space1);
StructuredBuffer<RtInstanceData> g_instance_data_buffer : register(t1, space1);

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

uint3 fetchIndices(uint instance_id, uint primitive_id)
{
	RtInstanceAttachments att = g_attachments_buffer[instance_id];

	// we declare the buffer with the right format so it will decode 16 and 32-bit indices
	Buffer<uint> ib = ResourceDescriptorHeap[NonUniformResourceIndex(att.ib.id)];

	uint3 indices = uint3(
		ib[primitive_id * 3 + 0],
		ib[primitive_id * 3 + 1],
		ib[primitive_id * 3 + 2]);

	return indices;
}

float3 fetchNormal(uint instance_id, uint3 indices, float2 baries, float3x3 transform)
{
	RtInstanceAttachments att = g_attachments_buffer[instance_id];

	float3 n;

	// if there isn't a valid normal stream, fallback to triangle normal
	if (att.norm.id == 0x7FFFFFFF)
	{
		ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(att.vb.id)];

		uint stride = att.vb.stride;
		float3 v0 = asfloat(vb.Load3(indices.x * stride + att.vb.offset));
		float3 v1 = asfloat(vb.Load3(indices.y * stride + att.vb.offset));
		float3 v2 = asfloat(vb.Load3(indices.z * stride + att.vb.offset));

		n = normalize(cross((v1 - v0), (v2 - v0)));
	}
	else
	{
		// since vb streams are interleaved, this needs to be a byte address buffer
		ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(att.norm.id)];

		uint stride = att.norm.stride;
		float3 n0 = asfloat(vb.Load3(indices.x * stride + att.norm.offset));
		float3 n1 = asfloat(vb.Load3(indices.y * stride + att.norm.offset));
		float3 n2 = asfloat(vb.Load3(indices.z * stride + att.norm.offset));

		n = n0 * (1.0 - baries.y - baries.x) + n1 * baries.x + n2 * baries.y;
		n = normalize(n);
	}

	n = mul(transform, n);
	return normalize(n);
}

float2 fetchUvs(uint instance_id, uint3 indices, float2 baries)
{
	RtInstanceAttachments att = g_attachments_buffer[instance_id];

	if (att.uv.id == 0x7FFFFFFF)
	{
		return 1.0.xx;
	}

	// since vb streams are interleaved, this needs to be a byte address buffer
	ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(att.uv.id)];

	uint stride = att.uv.stride;
	float2 u0 = asfloat(vb.Load2(indices.x * stride + att.uv.offset));
	float2 u1 = asfloat(vb.Load2(indices.y * stride + att.uv.offset));
	float2 u2 = asfloat(vb.Load2(indices.z * stride + att.uv.offset));

	float2 uv = u0 * (1.0 - baries.y - baries.x) + u1 * baries.x + u2 * baries.y;
	uv = frac(uv);

	return uv;
}

float4 fetchTexture(uint instance_id, float2 uv)
{
	RtInstanceAttachments att = g_attachments_buffer[instance_id];

	if (att.tex0.id == 0x7FFFFFFF)
	{
		return 1.0.xxxx;
	}

	Texture2D<float4> tex0 = ResourceDescriptorHeap[NonUniformResourceIndex(att.tex0.id)];

	uint width, height;
	tex0.GetDimensions(width, height);

	uint2 index = uint2(uv * float2(width, height) + 0.5);

	float4 texcolor = tex0[index];

	return texcolor;
}

Material fetchMaterial(uint instance_id, float3 textureAlbedo)
{
	RtInstanceData data = g_instance_data_buffer[instance_id];

	Material mtrl;
	mtrl.albedo = to_linear_from_srgb(textureAlbedo);
	mtrl.albedo_tint = to_linear_from_srgb(data.diffuse.rgb);
	mtrl.specular = to_linear_from_srgb(data.specular.rgb);
	return mtrl;
}

float3 evalMaterial(Material mtrl, Shade shade)
{
	float3 outIrradiance = 0.0;

	outIrradiance = mtrl.albedo * mtrl.albedo_tint;

	return outIrradiance;
}

Shade shadeSurface(Surface surface)
{
	Shade shade;

	Light light;
	light.dir = g_constants.sunDirection;
	light.color = 1.0.xxx;

	float3 h = normalize(light.dir + surface.view);

	light.n_dot_l = saturate(dot(surface.norm, light.dir));
	light.l_dot_h = saturate(dot(light.dir, h));
	light.n_dot_h = saturate(dot(surface.norm, h));

	const float3 diffuse = diffuse_burley(surface, light);
	const float3 specular = 0.0;

	const float3 light_intensity = light.n_dot_l * light.color;

	shade.diffuse_radiance = light_intensity * diffuse;
	shade.specular_radiance = light_intensity * specular;
	//shade.radiance = dot(surface.norm, g_constants.sunDirection) * 1.0.xxx; //add sun color

	return shade;
}

float3 path_trace(RayDesc ray, uint2 rng)
{
	float3 total_radiance = 0.0;
	float3 weight = 1.0;

	for (uint vertex = 0; vertex < g_constants.pathCount; vertex++)
	{
		RayHit hit = trace_ray_closest(g_rtScene, ray);
		if (hit.hitT < 0.0)
			break;

		const uint instanceId = hit.instanceId;
		const uint primitiveIndex = hit.primitiveId;
		const float2 baries = hit.barycentrics;
		const float3x3 transform = (float3x3)hit.transform;

		// fetch data
		const uint3 indices = fetchIndices(instanceId, primitiveIndex);
		const float2 uvs = fetchUvs(instanceId, indices, baries);
		const float4 texcolor = fetchTexture(instanceId, uvs);
		const float3 normal = fetchNormal(instanceId, indices, baries, transform);
		const Material mtrl = fetchMaterial(instanceId, texcolor.rgb);

		// setup our surface properties
		Surface surface;
		surface.pos = ray.Origin + ray.Direction * hit.hitT;
		surface.norm = normal;
		surface.view = normalize(ray.Origin - surface.pos);
		surface.n_dot_v = dot(surface.norm, surface.view);
		surface.roughness = 0.8; //TODO: fetch roughness

		// do lighting
		Shade shade = shadeSurface(surface);

		// do material (apply albedo + specular)
		float3 irradiance = evalMaterial(mtrl, shade);
		float3 radiance = shade.diffuse_radiance + shade.specular_radiance;

		// launch shadow ray
		{
			ray.Origin = surface.pos + surface.norm * length(surface.pos - ray.Origin) * 0.00001;
			ray.Direction = normalize(g_constants.sunDirection);

			hit = trace_ray_occlusion(g_rtScene, ray);

			if (hit.hitT >= 0.0)
			{
				radiance.rgb = 0.0;
			}
		}

		weight *= irradiance;
		total_radiance += weight * radiance;

		// ray origin already set to the right thing with the shadow ray test
		ray.Direction = sample_hemisphere(pcg2d_rng(rng), surface.norm);
	}

	return total_radiance;
}

[numthreads(8, 8, 1)]
void ray_gen(uint3 tid : SV_DispatchThreadID)
{
	float3 rayorigin = g_constants.viewPos.xyz;
	float3 raydir = 0.0;
	{
		//get far end of the ray
		uint width, height;
		g_rtOutput.GetDimensions(width, height);

		float2 d = (((float2)tid.xy / float2(width, height)) * 2.f - 1.f);
		float4 ndc = float4(d.x, -d.y, 1.0, 1.0);
		float4 worldpos = mul(ndc, g_constants.viewMatrix);
		worldpos.xyz /= worldpos.w;

		raydir = normalize(worldpos.xyz - rayorigin);
	}

	RayDesc ray;
	ray.TMin = 1e-5;
	ray.TMax = 1000.0;
	ray.Origin = rayorigin;
	ray.Direction = raydir;

	if (g_constants.showShaded)
	{
		uint2 seed = uint2(tid.xy) ^ uint2(g_constants.frameIndex.xx << 16);
		float3 radiance = path_trace(ray, seed);
		radiance = to_srgb_from_linear(radiance);
		g_rtOutput[tid.xy] = float4(radiance, 1.0);
	}
	else
	{
		RayHit hit = trace_ray_closest(g_rtScene, ray);

		float4 value = float4(0.0, 0.0, 0.0, 1.0);

		if (hit.hitT >= 0.0)
		{
			uint instanceId = hit.instanceId;
			uint primitiveIndex = hit.primitiveId;
			float2 baries = hit.barycentrics;
			float3x3 transform = (float3x3)hit.transform;

			if (g_constants.showNormal)
			{
				const uint3 indices = fetchIndices(instanceId, primitiveIndex);
				value.rgb = fetchNormal(instanceId, indices, baries, transform);
				value.rgb = value.rgb * 0.5 + 0.5;
			}
			else if (g_constants.showUvs)
			{
				float2 uvs = fetchUvs(instanceId, primitiveIndex, baries);
				value.rgb = float3(uvs, 0.0);
			}
			else if (g_constants.showTexture)
			{
				const uint3 indices = fetchIndices(instanceId, primitiveIndex);
				float2 uvs = fetchUvs(instanceId, indices, baries);

				float4 texcolor = fetchTexture(instanceId, uvs);
				value.rgb = texcolor.rgb;
			}
			else
			{
				value.rgb = instanceIdToColor(instanceId);
			}
		}

		g_rtOutput[tid.xy] = value;
	}
	
}
