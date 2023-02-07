#include "RtShared.h"
#include "Color.h"
#include "Material.h"
#include "Sampling.h"
#include "Raytracing.h"

struct Shade
{
	float3 diffuse_radiance;
	float3 specular_radiance;
	float3 specular_reflectance;
	float3 diffuse_reflectance;
	float attenuation;
};

struct ShadeRayResult
{
	Surface surface;
	Shade shade;
	Material mtrl;
	float3 radiance;
};

RaytracingAccelerationStructure g_rtScene : register(t0, space0);
StructuredBuffer<RtInstanceAttachments> g_attachments_buffer : register(t0, space1);
StructuredBuffer<RtInstanceData> g_instance_data_buffer : register(t1, space1);

RWTexture2D<float4> g_rtOutput : register(u0);
RWTexture2D<float> g_hitHistory : register(u1);

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

float3 fetchGeometryNormal(uint instance_id, uint3 indices, float3x3 transform)
{
	RtInstanceAttachments att = g_attachments_buffer[instance_id];

	float3 n;

	ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(att.vb.id)];

	uint stride = att.vb.stride;
	float3 v0 = asfloat(vb.Load3(indices.x * stride + att.vb.offset));
	float3 v1 = asfloat(vb.Load3(indices.y * stride + att.vb.offset));
	float3 v2 = asfloat(vb.Load3(indices.z * stride + att.vb.offset));

	n = normalize(cross((v1 - v0), (v2 - v0)));

	n = mul(transform, n);
	return normalize(n);
}

float3 fetchShadingNormal(uint instance_id, uint3 indices, float2 baries, float3x3 transform, float3 geomNormal)
{
	RtInstanceAttachments att = g_attachments_buffer[instance_id];

	float3 n;

	// if there isn't a valid normal stream, fallback to triangle normal
	if (att.norm.id == 0x7FFFFFFF)
	{
		n = geomNormal;
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
		n = mul(transform, n);
		n = normalize(n);
	}

	return n;
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

Material fetchMaterial(uint instance_id, float2 uv)
{
	RtInstanceData data = g_instance_data_buffer[instance_id];

	float4 textureAlbedo = fetchTexture(instance_id, uv);

	const float3 baseColorTint = to_linear_from_srgb(data.diffuse.rgb);

	float3 combined_base = to_linear_from_srgb(textureAlbedo.rgb) * baseColorTint;

	// some textures have zero color as their color which is not physically accurate
	//combined_base = max(0.05, combined_base);

	Material mtrl;
	mtrl.tint = baseColorTint;
	mtrl.base_color = combined_base;
	mtrl.emissive = 0.0;
	mtrl.metalness = 0.0;
	mtrl.opaque = instance_is_opaque(data);

	// there's a bug somewhere with either 0.0 or 1.0 roughness
	// clamp to [.1, .9]
	mtrl.roughness = clamp(data.roughness.x, 0.05, 0.95);
	return mtrl;
}

void fetchMaterialAndSurface(RayDesc ray, RayHit hit, out Material mtrl, out Surface surface)
{
	const uint instanceId = hit.instanceId;
	const uint primitiveIndex = hit.primitiveId;
	const float2 baries = hit.barycentrics;
	const float3x3 transform = (float3x3)hit.transform;

	const uint3 indices = fetchIndices(instanceId, primitiveIndex);
	const float2 uvs = fetchUvs(instanceId, indices, baries);
	const float3 geomNormal = fetchGeometryNormal(instanceId, indices, transform);
	const float3 shadingNormal = fetchShadingNormal(instanceId, indices, baries, transform, geomNormal);

	mtrl = fetchMaterial(instanceId, uvs);

	surface.pos = get_ray_hitpoint(ray, hit);
	surface.geom_normal = geomNormal;
	surface.shading_normal = shadingNormal;
}

float3 get_ray_origin_offset(RayDesc ray, Surface surface)
{
	return get_ray_origin_offset(ray, surface.pos, surface.geom_normal);
}

Shade shadeSurface(Surface surface, Material mtrl, float3 V)
{
	Light light;
	light.dir = g_constants.sunDirection;
	light.color = float3(1.0.xxx) * g_constants.sunIntensity;

	float NoL = dot(surface.shading_normal, light.dir);

	Brdf brdf;
	brdf.V = V;
	brdf.L = light.dir;
	brdf.N = surface.shading_normal;
	brdf.H = normalize(brdf.L + brdf.V);
	brdf.n_dot_l = saturate(NoL);
	brdf.l_dot_h = saturate(dot(brdf.L, brdf.H));
	brdf.n_dot_h = saturate(dot(brdf.N, brdf.H));
	brdf.n_dot_v = dot(brdf.N, brdf.V);
	brdf.roughness = mtrl.roughness;
	brdf.alpha = pow2(brdf.roughness);
	brdf.alpha2 = pow2(brdf.alpha);
	brdf.diffuse_reflectance = diffuse_reflectance(mtrl.base_color, mtrl.metalness);
	brdf.specular_f0 = specular_f0(mtrl.base_color, mtrl.metalness);

	const float3 spec_f90 = specular_f90(brdf.specular_f0);
	brdf.F = f_schlick(brdf.specular_f0, spec_f90, brdf.l_dot_h);

	const float diffuse = diffuse_brdf_burley(brdf);
	const float3 specular = specular_brdf_ggx(brdf);

	const float3 light_intensity = brdf.n_dot_l * light.color;

	Shade shade;
	shade.diffuse_reflectance = brdf.diffuse_reflectance;
	shade.specular_reflectance = brdf.F;
	shade.diffuse_radiance = light_intensity * diffuse * shade.diffuse_reflectance;
	shade.specular_radiance = light_intensity * specular;
	shade.attenuation = NoL;

	return shade;
}

ShadeRayResult shade_ray(RayDesc ray, RayHit hit, Material mtrl, Surface surface, inout uint2 rng, bool shadow = true)
{
	const float3 to_view = normalize(ray.Origin - surface.pos);

	// do lighting
	Shade shade = shadeSurface(surface, mtrl, to_view);

	float3 radiance = shade.diffuse_radiance + shade.specular_radiance;

	// launch shadow ray
	if(shade.attenuation > 0.0 && shadow)
	{
		float3x3 basis = create_basis_fast(g_constants.sunDirection); // TODO: move to CB
		const float2 xy = pcg2d_rng(rng) * g_constants.sunRadius;
		float3 sunDirection = normalize(basis[0] * xy.x + basis[1] * xy.y + basis[2]);

		ray.Direction = normalize(sunDirection);
		ray.Origin = get_ray_origin_offset(ray, surface);

		hit = trace_ray_occlusion(g_rtScene, ray);

		if (hit.hitT >= 0.0)
		{
			radiance.rgb = 0.0;
		}
	}

	ShadeRayResult result;
	result.radiance = radiance;
	result.shade = shade;
	result.surface = surface;
	result.mtrl = mtrl;

	return result;
}

ShadeRayResult shade_ray(RayDesc ray, RayHit hit, inout uint2 rng)
{
	// fetch data
	Material mtrl;
	Surface surface;
	fetchMaterialAndSurface(ray, hit, mtrl, surface);

	return shade_ray(ray, hit, mtrl, surface, rng);
}

#define SAMPLE_SPEUCULAR_GGX 1
float3 get_indirect_ray(Surface surface, Material mtrl, Shade shade, float3 V, inout float3 throughput, inout uint2 rng)
{
	// do multiple importance sampling
	// choose spec or diffuse ray based on estimated specular strength

	float3 ray_dir = 0.0;
#if SAMPLE_SPEUCULAR_GGX
	const float brdf_prob = estimate_specular_probability_ross(mtrl, surface.shading_normal, V);

	if (pcg2d_rng(rng).x < brdf_prob)
	{
		//specular ray
		throughput *= rcp(brdf_prob);

		// do anisotropic ray generation
		SpecularRay spec_ray = get_specular_ray_vndf_ggx(pcg2d_rng(rng), mtrl, surface.shading_normal, V);
		throughput *= spec_ray.weight;
		ray_dir = spec_ray.dir;
	}
	else
	{
		//diffuse ray
		throughput *= rcp(1.0 - brdf_prob);
		throughput *= shade.diffuse_reflectance; //weight by NoL and 1-F?
		ray_dir = get_diffuse_ray(pcg2d_rng(rng), surface.geom_normal);
	}
#else
	const float brdf_prob = estimate_specular_probability_simple(mtrl);

	ray_dir = get_diffuse_ray(pcg2d_rng(rng), surface.geom_normal);
	if (pcg2d_rng(rng).x < brdf_prob)
	{
		//specular ray
		throughput *= rcp(brdf_prob);

		// do anisotropic ray generation
		SpecularRay spec_ray = get_specular_ray_simple(V, surface.shading_normal, ray_dir, mtrl.roughness, shade.specular_reflectance);
		throughput *= spec_ray.weight;
		ray_dir = spec_ray.dir;
	}
	else
	{
		//diffuse ray
		throughput *= rcp(1.0 - brdf_prob);
		throughput *= shade.diffuse_reflectance; //weight by NoL and 1-F?
	}
#endif

	return ray_dir;
}

#define EXTRACT_PRIMARY 1
float3 path_trace(RayDesc ray, ShadeRayResult primaryShade, inout uint2 rng)
{
#if EXTRACT_PRIMARY
	float3 total_radiance = primaryShade.radiance + primaryShade.mtrl.emissive;
	float3 throughput = 1.0;

	float bounce_boost = 1.0;

	ShadeRayResult shade = primaryShade;

	for (uint vertex = 1; vertex < g_constants.pathCount; vertex++)
	{
		const float3 V = -ray.Direction;

		ray.Origin = get_ray_origin_offset(ray, shade.surface);
		ray.Direction = get_indirect_ray(shade.surface, shade.mtrl, shade.shade, V, throughput, rng);

		RayHit hit = trace_ray_closest_opaque(g_rtScene, ray);
		if (hit.hitT < 0.0)
		{
			//TODO: sample a skybox
			total_radiance += throughput * float3(0.1, 0.1, 0.25) * 1.0;
			break;
		}

		shade = shade_ray(ray, hit, rng);
		total_radiance += throughput * shade.mtrl.emissive;

		bounce_boost += g_constants.bounceBoost * vertex;
		total_radiance += throughput * shade.radiance * bounce_boost;

		// russian roulette. if expected lum is low, randomly kill
		// even though not all paths take this, it does help perf
		{
			const float kill = get_luminance(throughput);
			if (pcg2d_rng(rng).x > kill)
				break;

			// mul throughput by the pdf of the decision
			throughput *= rcp(kill);
		}
	}
#else
	float3 total_radiance = 0.0;
	float3 throughput = 1.0;

	float bounce_boost = 1.0;

	for (uint vertex = 0; vertex < g_constants.pathCount; vertex++)
	{
		RayHit hit = trace_ray_closest_opaque(g_rtScene, ray);
		if (hit.hitT < 0.0)
		{
			//TODO: sample a skybox
			total_radiance += throughput * float3(0.1, 0.1, 0.25) * 1.0;
			break;
		}

		ShadeRayResult shade = shade_ray(ray, hit, rng);
		total_radiance += throughput * shade.mtrl.emissive;

		bounce_boost += g_constants.bounceBoost * vertex;
		total_radiance += throughput * shade.radiance * bounce_boost;

		if (vertex > 1)
		{
			const float kill = get_luminance(throughput);
			if (pcg2d_rng(rng).x > kill)
				break;

			// throughput by the pdf of the decision
			throughput *= rcp(kill);
		}

		const float3 V = -ray.Direction;
		ray.Origin = get_ray_origin_offset(ray, shade.surface);
		ray.Direction = get_indirect_ray(shade.surface, shade.mtrl, shade.shade, V, throughput, rng);
	}
#endif

	return total_radiance;
}

struct Visitor
{
	static bool visit(RayHit hit)
	{
		RtInstanceData data = g_instance_data_buffer[hit.instanceId];
		float opacity = data.diffuse.a;

		//const float3 baseColorTint = to_linear_from_srgb(data.diffuse.rgb);

		if (opacity > 0.25)
		{
			return true;
		}

		return false;
	}
};

float3 trace_transparent(RayDesc ray, ShadeRayResult primaryShade, bool is_reflection, uint additional_bounces, inout uint2 rng)
{
	float3 total_radiance = 0.0;
	float3 transmittance = 1.0;
	float bounce_boost = 1.0;

	const float ior_air = 1.00029;
	const float ior_glass = 1.55;
	const float eta = ior_air / ior_glass;
	const float glass_thickness = 0.003;
	const float3 glass_tint = saturate(primaryShade.mtrl.tint * 2.0);

	ShadeRayResult shade = primaryShade;

	const uint total_pathcount = g_constants.pathCount + additional_bounces;
	for (uint vertex = 0; vertex < total_pathcount; vertex++)
	{
		const float3 V = -ray.Direction;
		float NoV = abs(dot(shade.surface.shading_normal, V));
		float F = f_dialectric(eta, NoV);

		if (vertex == 0)
			transmittance *= is_reflection ? F : 1.0 - F;
		else
			is_reflection = pcg2d_rng(rng).x < F;

		float3 N = is_reflection ? shade.surface.shading_normal : -shade.surface.shading_normal;
		ray.Origin = get_ray_origin_offset(ray, shade.surface.pos, N);
		ray.Direction = reflect(-V, shade.surface.shading_normal);

		if (!is_reflection)
		{
			// glas is a single, double sided surface
			ray.Direction = refract(-V, shade.surface.shading_normal, eta);

			float cosa = abs(dot(ray.Direction, shade.surface.shading_normal));
			float d = glass_thickness / max(cosa, 0.05);

			// for glass close to a surface, this could go under the beneath surface
			ray.Origin += ray.Direction * d;

			ray.Direction = refract(ray.Direction, shade.surface.shading_normal, 1.0 / eta);

			transmittance *= glass_tint;
		}
		
		// ignore transparent, else all
		const uint instance_mask = vertex == total_pathcount - 1 && !is_reflection ? 1 : 3;

		RayHit hit = trace_ray_closest_any_t<Visitor>(g_rtScene, ray, instance_mask);
		if (hit.hitT < 0.0)
		{
			break;
		}

		fetchMaterialAndSurface(ray, hit, shade.mtrl, shade.surface);

		if (shade.mtrl.opaque)
		{
			// this shadow ray needs to ignore backfacing
			ShadeRayResult shade_local = shade_ray(ray, hit, shade.mtrl, shade.surface, rng, false);

			// need better ambient estimate....
			const float3 ambient = 0.5 * shade.mtrl.base_color;
			float3 L = shade_local.radiance + shade_local.mtrl.emissive + ambient;

			total_radiance += transmittance * L;
			break;
		}
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
		float3 radiance = 0.0;

		// send a primary ray, ignoring all transparent geo
		RayHit hit = trace_ray_closest_opaque(g_rtScene, ray);

		float3 mv = 0.0;
		if (hit.hitT >= 0.0)
		{
			ShadeRayResult shade = shade_ray(ray, hit, seed);

			for (int i = 0; i < g_constants.iterCount; i++)
			{
				radiance += path_trace(ray, shade, seed);
			}
			radiance /= float(g_constants.iterCount);

			const RtInstanceData data = g_instance_data_buffer[hit.instanceId];
			const float3 hitpos = get_ray_hitpoint(ray, hit);
			const float3 prev_hitpos = mul(data.toWorldPrevT, float4(hitpos, 1.0));
			mv = hitpos - prev_hitpos;
		}
		else
		{
			//sample sky?
		}

		//trace transparent
#if 1
		if(g_constants.transparentEnable)
		{
			RayHit primaryHit = hit;
			if (primaryHit.hitT < 0.0)
			{
				primaryHit.hitT = 1000.0;
			}

			RayDesc trans_ray = ray;
			trans_ray.TMax = primaryHit.hitT;

			// trace transparent only
			RayHit trans_hit = trace_ray_closest_any_t<Visitor>(g_rtScene, trans_ray, 2);
			if (trans_hit.hitT >= 0.0 && trans_hit.hitT <= hit.hitT)
			{
				ShadeRayResult shade = (ShadeRayResult)0;
				fetchMaterialAndSurface(trans_ray, trans_hit, shade.mtrl, shade.surface);

				float3 transparent = 0.0;

				shade = shade_ray(trans_ray, trans_hit, shade.mtrl, shade.surface, seed);
				transparent += shade.radiance;
				transparent += trace_transparent(trans_ray, shade, true, 2, seed);
				transparent += trace_transparent(trans_ray, shade, false, 2, seed);

				/*if (any(transparent) >= 5.0)
					transparent = 1.0;
				else if (any(transparent) >= 4.0)
					transparent = float3(1, 0, 0);
				else if (any(transparent) >= 3.0)
					transparent = float3(1, 0, 1);
				else if (any(transparent) >= 2.0)
					transparent = float3(0, 0, 1);
				else if (any(transparent) >= 1.0)
					transparent = float3(0, 1, 0);
				else
					transparent = float3(1, 1, 0);*/

				// replace the radiance with our radiance
				radiance = transparent;
				hit = trans_hit;
			}			
		}
#endif

		const float prevHitT = g_hitHistory[tid.xy];

		const bool hitInvalidate = abs(hit.hitT - prevHitT) > 0.1;
		const bool motionInvalidate = dot(mv, mv) > 0.1;
		const bool reset = g_constants.frameIndex == 0 || motionInvalidate || hitInvalidate;

		float4 prevRadiance = g_rtOutput[tid.xy];
		float weight = reset ? 1.0f : 1.0f / (1.0f + (1.0f / prevRadiance.a));
		radiance = lerp(prevRadiance.rgb, radiance, weight);

		g_rtOutput[tid.xy] = float4(radiance, weight);
		g_hitHistory[tid.xy] = hit.hitT;
	}
	else
	{
		RayHit hit = trace_ray_closest_opaque(g_rtScene, ray);

		float4 value = float4(0.0, 0.0, 0.0, 1.0);

		if (hit.hitT >= 0.0)
		{
			const uint instanceId = hit.instanceId;
			const uint primitiveIndex = hit.primitiveId;
			const float2 baries = hit.barycentrics;
			const float3x3 transform = (float3x3)hit.transform;

			if (g_constants.showNormal)
			{
				const uint3 indices = fetchIndices(instanceId, primitiveIndex);
				const float3 geomNormal = fetchGeometryNormal(instanceId, indices, transform);
				value.rgb = fetchShadingNormal(instanceId, indices, baries, transform, geomNormal);
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
			else if (g_constants.showMotionVec)
			{
				const RtInstanceData data = g_instance_data_buffer[instanceId];

				const float3 hitpos = get_ray_hitpoint(ray, hit);
				const float3 prev_hitpos = mul(data.toWorldPrevT, float4(hitpos, 1.0));
				const float3 mv = hitpos - prev_hitpos;

				value.rgb = abs(mv) / 2.0;
			}
			else
			{
				value.rgb = instanceIdToColor(instanceId);
			}
		}

		g_rtOutput[tid.xy] = value;
	}
	
}
