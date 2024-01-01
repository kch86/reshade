#include "RtShared.h"
#include "Color.h"
#include "Material.h"
#include "Sampling.h"
#include "Raytracing.h"

#define EXTRACT_PRIMARY 1
#define SAMPLE_SPEUCULAR_GGX 1
#define INTEGRATED_TRANSMISSION 1
#define OPAQUE_TRANSPARENCY 1

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

sampler g_cube_sampler : register(s0, space0);

RaytracingAccelerationStructure g_rtScene : register(t0, space0);
StructuredBuffer<RtInstanceAttachments> g_attachments_buffer : register(t0, space1);
StructuredBuffer<RtInstanceData> g_instance_data_buffer : register(t1, space1);
TextureCube<float3> g_spec_cube : register(t2, space1);
Buffer<float> g_sample_buffer : register(t3, space1);

RWTexture2D<float4> g_rtOutput : register(u0);
RWTexture2D<float2> g_hitHistory : register(u1);

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

uint3 fetchIndices(RtInstanceAttachments att, uint primitive_id)
{
	// we declare the buffer with the right format so it will decode 16 and 32-bit indices
	Buffer<uint> ib = ResourceDescriptorHeap[NonUniformResourceIndex(att.ib.id)];

	uint3 indices = uint3(
		ib[primitive_id * 3 + 0 + att.ib.offset], //offset for the ib is in elements
		ib[primitive_id * 3 + 1 + att.ib.offset],
		ib[primitive_id * 3 + 2 + att.ib.offset]);

	return indices;
}

float3 fetchGeometryNormal(RtInstanceAttachments att, uint3 indices, float3x3 transform)
{
	float3 n;

	ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(att.vb.id)];

	uint stride = att.vb.stride;
	float3 v0 = vb.Load<float3>(indices.x * stride + att.vb.offset);
	float3 v1 = vb.Load<float3>(indices.y * stride + att.vb.offset);
	float3 v2 = vb.Load<float3>(indices.z * stride + att.vb.offset);

	n = normalize(cross((v1 - v0), (v2 - v0)));

	n = mul(transform, n);
	return normalize(n);
}

float3 fetchShadingNormal(RtInstanceAttachments att, uint3 indices, float2 baries, float3x3 transform, float3 geomNormal)
{
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
		float3 n0 = vb.Load<float3>(indices.x * stride + att.norm.offset);
		float3 n1 = vb.Load<float3>(indices.y * stride + att.norm.offset);
		float3 n2 = vb.Load<float3>(indices.z * stride + att.norm.offset);

		n = n0 * (1.0 - baries.y - baries.x) + n1 * baries.x + n2 * baries.y;
		n = mul(transform, n);
		n = normalize(n);
	}

	return n;
}

float2 fetchUvs(RtInstanceAttachments att, uint3 indices, float2 baries)
{
	if (att.uv.id == 0x7FFFFFFF)
	{
		return 1.0.xx;
	}

	// since vb streams are interleaved, this needs to be a byte address buffer
	ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(att.uv.id)];

	uint stride = att.uv.stride;
	float2 u0 = vb.Load<float2>(indices.x * stride + att.uv.offset);
	float2 u1 = vb.Load<float2>(indices.y * stride + att.uv.offset);
	float2 u2 = vb.Load<float2>(indices.z * stride + att.uv.offset);

	float2 uv = u0 * (1.0 - baries.y - baries.x) + u1 * baries.x + u2 * baries.y;
	uv = frac(uv);

	return uv;
}

float4 fetchTexture(RtInstanceAttachments att, float2 uv)
{
	if (att.tex0.id == 0x7FFFFFFF)
	{
		return 1.0.xxxx;
	}

	Texture2D<float4> tex0 = ResourceDescriptorHeap[NonUniformResourceIndex(att.tex0.id)];

	uint width, height;
	tex0.GetDimensions(width, height);

	uint2 index = uint2(uv * float2(width, height) + 0.5);

	float4 texcolor = tex0[index];
	texcolor.rgb = to_linear_from_srgb(texcolor.rgb);

	return texcolor;
}

float4 fetchVertexColor(RtInstanceAttachments att, uint3 indices, float2 baries)
{
	if (att.col.id == 0x7FFFFFFF)
	{
		return 0.0.xxxx;
	}

	// since vb streams are interleaved, this needs to be a byte address buffer
	ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(att.col.id)];

	float4 c = 1.0;

	uint stride = att.col.stride;
	if (att.col.format == 87)
	{
		float4 c0 = bgra_unorm_to_float(vb.Load(indices.x * stride + att.col.offset));
		float4 c1 = bgra_unorm_to_float(vb.Load(indices.y * stride + att.col.offset));
		float4 c2 = bgra_unorm_to_float(vb.Load(indices.z * stride + att.col.offset));

		c = saturate(c0 * (1.0 - baries.y - baries.x) + c1 * baries.x + c2 * baries.y);
	}
	else
	{
		float3 c0 = vb.Load<float3>(indices.x * stride + att.col.offset);
		float3 c1 = vb.Load<float3>(indices.y * stride + att.col.offset);
		float3 c2 = vb.Load<float3>(indices.z * stride + att.col.offset);

		c.rgb = saturate(c0 * (1.0 - baries.y - baries.x) + c1 * baries.x + c2 * baries.y);
	}
	
	return c;
}

MaterialType fetchVertexMtrl(RtInstanceAttachments att, uint3 indices, float2 baries)
{
	if (att.mtrl.id == 0x7FFFFFFF)
	{
		return Material_Standard;
	}

	// since vb streams are interleaved, this needs to be a byte address buffer
	ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(att.mtrl.id)];

	float4 c = 1.0;

	// don't support vertex material blending for now
	// verts have 8-bit data packed into 32-bits
	// so divide vert index by 4 and then shift remainder into the uint
	// mod 4 * 8 since we want to shift by 8-bit groups
	uint stride = att.mtrl.stride;
	const uint packedMtrl = vb.Load<uint>((indices.x / 4) * stride + att.mtrl.offset);
	const uint mtrl = (packedMtrl >> ((indices.x % 4) * 8)) & 0xf;

	return (MaterialType)mtrl;
}

Material fetchMaterial(uint instance_id, float2 uv, uint3 indices, float2 baries)
{
	RtInstanceData data = g_instance_data_buffer[instance_id];
	RtInstanceAttachments att = g_attachments_buffer[instance_id];

	const bool is_opaque = instance_is_opaque(data);

	float4 textureAlbedo = fetchTexture(att, uv);

	// not sure what to do with this yet
	// they seem to use this for baked gi

	const float3 baseColorTint = to_linear_from_srgb(data.diffuse.rgb);

	if (data.mtrl == Material_Headlight)
	{
		//tex is used to cancel out the diffuse term...

		// only do this for headlight glass
		float opacity = pow(textureAlbedo.a * data.diffuse.a, 5.0);
		textureAlbedo.rgb = lerp(textureAlbedo.rgb, baseColorTint, opacity);
	}

	float3 combined_base = textureAlbedo.rgb * baseColorTint;

	float opacity = data.diffuse.a * textureAlbedo.a;

	// some textures have zero color as their color which is not physically accurate
	//combined_base = max(0.05, combined_base);

	Material mtrl;
	mtrl.tint = baseColorTint;
	mtrl.base_color = combined_base;
	mtrl.emissive = 0.0;// emissive;
	mtrl.metalness = 0.0;
	mtrl.opacity = opacity;
	mtrl.opaque = instance_is_opaque(data);
	mtrl.type = data.mtrl;

	float roughness = data.roughness.x;

	if (mtrl.opaque)
	{
		// this usually helps roughness
		// the ground uses alpha for puddles, but the cars seem to benefit too

		// don't let the stard enviro get too glossy
		float roughness_scale = textureAlbedo.a;
		roughness_scale = data.mtrl == Material_Standard ? max(roughness_scale, 0.1) : roughness_scale;
		roughness *= roughness_scale;
	}
	else
	{
		// only do this for glass?
		roughness = lerp(roughness, 1.0, mtrl.opacity);
	}

	// there's a bug somewhere with either 0.0 or 1.0 roughness
	// clamp to [.1, .9]
	mtrl.roughness = clamp(roughness, 0.05, 0.95);

	return mtrl;
}

void fetchMaterialAndSurface(RayDesc ray, RayHit hit, out Material mtrl, out Surface surface)
{
	const uint instanceId = hit.instanceId;
	const uint primitiveIndex = hit.primitiveId;
	const float2 baries = hit.barycentrics;
	const float3x3 transform = (float3x3)hit.transform;

	RtInstanceAttachments att = g_attachments_buffer[instanceId];
	const uint3 indices = fetchIndices(att, primitiveIndex);
	const float2 uvs = fetchUvs(att, indices, baries);
	const float3 geomNormal = fetchGeometryNormal(att, indices, transform);
	const float3 shadingNormal = fetchShadingNormal(att, indices, baries, transform, geomNormal);

	mtrl = fetchMaterial(instanceId, uvs, indices, baries);

	surface.pos = get_ray_hitpoint(ray, hit);

	// using committedTriangleFrontFace is causing weird compilation issues on amd, calculate backface here
	const bool backface = dot(ray.Direction, geomNormal) < 0.0;
	surface.geom_normal = backface ? geomNormal : -geomNormal;
	surface.shading_normal = backface ? shadingNormal : -shadingNormal;
}

float3 get_ray_origin_offset(RayDesc ray, Surface surface)
{
	return get_ray_origin_offset(ray, surface.pos, surface.geom_normal);
}

uint get_ray_mask()
{
	#if INTEGRATED_TRANSMISSION || OPAQUE_TRANSPARENCY
		return InstanceMask_all;
	#else
		return InstanceMask_opaque_alphatest;
	#endif
}

struct Visitor
{
	static Rng rng;
	static RayCandidate visit(RayDesc ray, RayHit hit)
	{
		// adding this is a repro
		/*if (g_constants.debugView != DebugView_None)
			return true;*/

#if INTEGRATED_TRANSMISSION == 0 && OPAQUE_TRANSPARENCY == 0
		if (g_constants.transparentEnable == false)
			return RayCandidate::init( true, 1.0 );
#endif

		RtInstanceData data = g_instance_data_buffer[hit.instanceId];

#if OPAQUE_TRANSPARENCY
		const bool opaque = instance_is_opaque(data);
#else
		const bool opaque = true;
#endif

		Material mtrl;
		Surface surface;
		fetchMaterialAndSurface(ray, hit, mtrl, surface);
		surface = (Surface)0; //we won't use this so hint to the compiler to discard

		const float opacity = mtrl.opacity;

		if (opaque)
		{
			if (opacity > 0.0)
			{
				return RayCandidate::init( true, 1.0 );
			}
		}
		else
		{
#if INTEGRATED_TRANSMISSION
			// if glass or headlight just make sure we're not 100% transparent
			if (data.mtrl == Material_Glass || data.mtrl == Material_Headlight)
			{
				if (opacity > 0.0)
				{
					return RayCandidate::init( true, 1.0 );
				}
			}
			else
#endif
			{
				// else treat as opaque and roll the dice
				if (opacity > rng.gen().x)
				{
					return RayCandidate::init( true, opacity );
				}
			}
		}		

		return RayCandidate::init( false, opacity );
	}
};
Rng Visitor::rng;

struct VisitorShadow
{
	static RayCandidate visit(RayDesc ray, RayHit hit)
	{
		RtInstanceData data = g_instance_data_buffer[hit.instanceId];

		Material mtrl;
		Surface surface;
		fetchMaterialAndSurface(ray, hit, mtrl, surface);
		surface = (Surface)0; //we won't use this so hint to the compiler to discard

		const float opacity = mtrl.opacity;

		if (opacity > 0.0)
		{
			return RayCandidate::init( true, 1.0 );
		}

		return RayCandidate::init( false, 1.0 );
	}
};

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

ShadeRayResult shade_ray(RayDesc ray, RayHit hit, Material mtrl, Surface surface, inout Rng rng, bool shadow = true)
{
	const float3 to_view = normalize(ray.Origin - surface.pos);

	// do lighting
	Shade shade = shadeSurface(surface, mtrl, to_view);

	float3 radiance = shade.diffuse_radiance + shade.specular_radiance;

	// launch shadow ray
	if(shade.attenuation > 0.0 && shadow)
	{
		float3x3 basis = create_basis_fast(g_constants.sunDirection); // TODO: move to CB
		const float2 xy = rng.gen() * g_constants.sunRadius;
		float3 sunDirection = normalize(basis[0] * xy.x + basis[1] * xy.y + basis[2]);

		ray.Direction = normalize(sunDirection);
		ray.Origin = get_ray_origin_offset(ray, surface);

		hit = trace_ray_occlusion_any<VisitorShadow>(g_rtScene, ray, InstanceMask_opaque_alphatest);

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

ShadeRayResult shade_ray(RayDesc ray, RayHit hit, inout Rng rng)
{
	// fetch data
	Material mtrl;
	Surface surface;
	fetchMaterialAndSurface(ray, hit, mtrl, surface);

	return shade_ray(ray, hit, mtrl, surface, rng);
}

float3 get_sky(float3 dir)
{
	const float3 high = float3(0.1, 0.1, 0.25);
	const float3 low = float3(0.2, 0.2, 0.25);
	return lerp(low, high, saturate(dir.z)) * 1.0;
}

float3 get_indirect_ray(Surface surface, Material mtrl, Shade shade, float3 V, inout float3 throughput, inout Rng rng)
{
	// do multiple importance sampling
	// choose spec or diffuse ray based on estimated specular strength

	float3 ray_dir = 0.0;
#if SAMPLE_SPEUCULAR_GGX
	const float brdf_prob = estimate_specular_probability_ross(mtrl, surface.shading_normal, V);

	if (rng.gen().x < brdf_prob)
	{
		//specular ray
		throughput *= rcp(brdf_prob);

		// do anisotropic ray generation
		SpecularRay spec_ray = get_specular_ray_vndf_ggx(rng.gen(), mtrl, surface.shading_normal, V);
		throughput *= spec_ray.weight;
		ray_dir = spec_ray.dir;
	}
	else
	{
		//diffuse ray
		throughput *= rcp(1.0 - brdf_prob);
		throughput *= shade.diffuse_reflectance; //weight by NoL and 1-F?
		ray_dir = get_diffuse_ray(rng.gen(), surface.geom_normal);
	}
#else
	const float brdf_prob = estimate_specular_probability_simple(mtrl);

	ray_dir = get_diffuse_ray(rng.gen(), surface.geom_normal);
	if (rng.gen().x < brdf_prob)
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

RayDesc get_refraction_ray(RayDesc ray, Surface surface, Material mtrl, float3 V, inout float3 throughput, inout Rng rng)
{
	const float ior_air = 1.00029;
	const float ior_glass = 1.55;
	const float eta = ior_air / ior_glass;
	const float glass_thickness = 0.003;
	const float3 glass_tint = saturate(mtrl.tint * 2.0 + 0.2);

	// glass is a single, double sided surface
	ray.Direction = refract(-V, surface.shading_normal, eta);

	float cosa = abs(dot(ray.Direction, surface.shading_normal));
	//float d = min(0.01, glass_thickness / max(cosa, 0.05));
	float d = glass_thickness / max(cosa, 0.05);

	// for glass close to a surface, this could go under the beneath surface
	ray.Origin += ray.Direction * d;

	ray.Direction = refract(ray.Direction, surface.shading_normal, 1.0 / eta);

	return ray;
}

float3 path_trace(RayDesc ray, ShadeRayResult primaryShade, inout Rng rng)
{
#if EXTRACT_PRIMARY
	float3 total_radiance = primaryShade.radiance + primaryShade.mtrl.emissive;
	float3 throughput = 1.0;

	float bounce_boost = 1.0;

	ShadeRayResult shade = primaryShade;

	for (uint vertex = 1; vertex < g_constants.pathCount; vertex++)
	{
		const float3 V = -ray.Direction;

		rng.set_dim(vertex);

#if INTEGRATED_TRANSMISSION
		bool is_transmission = false;
		float reflect_pdf = 1.0;
		float3 transmission = 1.0;
		if (shade.mtrl.type == Material_Glass || shade.mtrl.type == Material_Headlight)
		{
			const float ior_air = 1.00029;
			const float ior_glass = 1.55;
			const float eta = ior_air / ior_glass;

			const float NoV = abs(dot(normalize(shade.surface.shading_normal), V));
			const float Rp = f_dialectric(eta, NoV); // reflectance prob
			const float Tp = 1.0 - Rp; // transmission prob

			reflect_pdf = Rp / (Rp + Tp);
			const bool is_reflection = rng.gen().x < reflect_pdf;
			is_transmission = !is_reflection;

			const float3 glass_tint = saturate(shade.mtrl.tint * 2.0 + 0.2);
			transmission = glass_tint;
		}
		else if (shade.mtrl.type == Material_Standard_Additive)
		{
			reflect_pdf = get_luminance(shade.mtrl.base_color);
			is_transmission = rng.gen().x > reflect_pdf;
		}

		if (is_transmission)
		{
			float3 N = -shade.surface.shading_normal;
			ray.Origin = get_ray_origin_offset(ray, shade.surface.pos, N);
			ray = get_refraction_ray(ray, shade.surface, shade.mtrl, V, throughput, rng);
			throughput *= rcp(1.0 - reflect_pdf);
			throughput *= transmission;
		}
		else
#endif
		{
			ray.Origin = get_ray_origin_offset(ray, shade.surface);
			ray.Direction = get_indirect_ray(shade.surface, shade.mtrl, shade.shade, V, throughput, rng);
			throughput *= rcp(reflect_pdf);
		}

		Visitor::rng = rng;
		RayHit hit = trace_ray_closest_any<Visitor>(g_rtScene, ray, get_ray_mask());

		if (hit.hitT < 0.0)
		{
			//TODO: sample a skybox
			total_radiance += throughput * get_sky(ray.Direction);
			break;
		}

		fetchMaterialAndSurface(ray, hit, shade.mtrl, shade.surface);
		shade = shade_ray(ray, hit, shade.mtrl, shade.surface, rng);

		//shade = shade_ray(ray, hit, rng);
		total_radiance += throughput * shade.mtrl.emissive;

		bounce_boost += g_constants.bounceBoost * vertex;
		total_radiance += throughput * shade.radiance * bounce_boost;

		// russian roulette. if expected lum is low, randomly kill
		// even though not all paths take this, it does help perf
		{
			const float kill = get_luminance(throughput);
			if (rng.gen().x > kill)
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
		RayHit hit = trace_ray_closest_any<Visitor>(g_rtScene, ray, InstanceMask_opaque_alphatest);
		if (hit.hitT < 0.0)
		{
			//TODO: sample a skybox
			total_radiance += throughput * get_sky(ray.Direction);
			break;
		}

		ShadeRayResult shade = shade_ray(ray, hit, rng);
		total_radiance += throughput * shade.mtrl.emissive;

		bounce_boost += g_constants.bounceBoost * vertex;
		total_radiance += throughput * shade.radiance * bounce_boost;

		if (vertex > 1)
		{
			const float kill = get_luminance(throughput);
			if (rng.gen().x > kill)
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

float3 trace_transparent(RayDesc ray, ShadeRayResult primaryShade, bool is_reflection, uint additional_bounces, inout Rng rng)
{
	float3 total_radiance = 0.0;
	float3 transmittance = 1.0;
	float bounce_boost = 1.0;

	const float ior_air = 1.00029;
	const float ior_glass = 1.55;
	const float eta = ior_air / ior_glass;
	const float glass_thickness = 0.003;
	const float3 glass_tint = saturate(primaryShade.mtrl.tint * 2.0 + 0.2);

	const bool primary_is_headlight = primaryShade.mtrl.type == Material_Headlight;

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
			is_reflection = rng.gen().x < F;

		float3 N = is_reflection ? shade.surface.shading_normal : -shade.surface.shading_normal;
		ray.Origin = get_ray_origin_offset(ray, shade.surface.pos, N);
		ray.Direction = reflect(-V, shade.surface.shading_normal);

		if (!is_reflection)
		{
			// glass is a single, double sided surface
			ray.Direction = refract(-V, shade.surface.shading_normal, eta);

			float cosa = abs(dot(ray.Direction, shade.surface.shading_normal));
			//float d = min(0.01, glass_thickness / max(cosa, 0.05));
			float d = glass_thickness / max(cosa, 0.05);

			// for glass close to a surface, this could go under the beneath surface
			ray.Origin += ray.Direction * d;

			ray.Direction = refract(ray.Direction, shade.surface.shading_normal, 1.0 / eta);

			transmittance *= glass_tint;
		}
		
		// decide which geo to look at
		// if we're on the last path vertex, just try get the ray to escape, only look at transparents
		// else include both transparents and opaque
		const uint instance_mask = vertex == total_pathcount - 1 && !is_reflection ? InstanceMask_transparent : InstanceMask_all;

		RayHit hit = trace_ray_closest_any<Visitor>(g_rtScene, ray, instance_mask);

		if (hit.hitT < 0.0)
		{
			total_radiance += transmittance * get_sky(ray.Direction);
			break;
		}

		fetchMaterialAndSurface(ray, hit, shade.mtrl, shade.surface);

		// 1st opaque hit inside a headlight is a reflector
		// TODO: only do this if the primary surface was headlight glass
		const bool is_reflector = primary_is_headlight && dot(shade.mtrl.tint, 1.0) < 0.01 && shade.mtrl.opacity > 0.5;
		if (shade.mtrl.opaque)
		{
			if (is_reflector)
			{
				//WIP... good? bad?
				shade.mtrl.metalness = 1.0;
			}
			ShadeRayResult shade_local = shade_ray(ray, hit, shade.mtrl, shade.surface, rng);

			// need better ambient estimate....
			const float3 ambient = 0.5 * shade.mtrl.base_color * ambient_brdf(shade.mtrl, shade.surface.shading_normal, V);
			float3 L = shade_local.radiance + shade_local.mtrl.emissive + ambient;

			if (is_reflector)
			{
				total_radiance += transmittance * shade_local.radiance;
			}
			else
			{
				total_radiance += transmittance * L;
				break;
			}			
		}
	}

	return total_radiance;
}

[numthreads(8, 8, 1)]
void ray_gen(uint3 tid : SV_DispatchThreadID)
{
	uint width, height;
	g_rtOutput.GetDimensions(width, height);

	float3 rayorigin = g_constants.viewPos.xyz;
	float3 raydir = 0.0;
	{
		//get far end of the ray

		float2 d = (((float2)tid.xy / float2(width, height)) * 2.f - 1.f);
		float4 ndc = float4(d.x, -d.y, 1.0, 1.0);
		float4 worldpos = mul(ndc, g_constants.viewMatrix);
		worldpos.xyz /= worldpos.w;

		raydir = normalize(worldpos.xyz - rayorigin);
	}

	RayDesc ray;
	ray.TMin = 1e-5;
	ray.TMax = g_constants.max_t;
	ray.Origin = rayorigin;
	ray.Direction = raydir;

	if (g_constants.debugView == DebugView_None)
	{
		Rng rng;
		rng.init(tid.xy, g_constants.frameIndex, RngType::Pcg);
		rng.set_sample_offset(g_constants.frameIndex * 32);

#ifdef COMPARE_RNG
		if (tid.x > (width / 2))
		{
			rng.init(tid.xy, g_constants.frameIndex, RngType::Halton);
			rng.set_dim(0);
			rng.set_sample_offset(g_constants.frameIndex * 32);
			rng.set_buffer(g_sample_buffer);
		}		
#endif

		float3 radiance = 0.0;

		// send a primary ray, ignoring transparent geo, but include alpha-tested geo
		Visitor::rng = rng;
		RayHit hit = trace_ray_closest_any<Visitor>(g_rtScene, ray, get_ray_mask());

		float3 mv = 0.0;
		if (hit.hitT >= 0.0)
		{
			ShadeRayResult shade = shade_ray(ray, hit, rng);

			for (int i = 0; i < g_constants.iterCount; i++)
			{
				radiance += path_trace(ray, shade, rng);
			}
			radiance /= float(g_constants.iterCount);

			radiance = max(0.0, radiance);

			const RtInstanceData data = g_instance_data_buffer[hit.instanceId];
			const float3 hitpos = get_ray_hitpoint(ray, hit);
			const float3 prev_hitpos = mul(data.toWorldPrevT, float4(hitpos, 1.0));
			mv = hitpos - prev_hitpos;
		}
		else
		{
			//sample sky?
			//radiance = g_spec_cube.SampleLevel(g_cube_sampler, ray.Direction, 0);
		}

		//trace transparent
#if INTEGRATED_TRANSMISSION == 0 && OPAQUE_TRANSPARENCY == 0
		if(g_constants.transparentEnable)
		{
			RayHit primaryHit = hit;
			if (primaryHit.hitT < 0.0)
			{
				primaryHit.hitT = 1000.0;
			}

			RayDesc trans_ray = ray;
			trans_ray.TMax = primaryHit.hitT;

			// trace transparent only, exclude alpha-test
			RayHit trans_hit = trace_ray_closest_transparent<Visitor>(g_rtScene, trans_ray, InstanceMask_transparent);
			if (trans_hit.hitT >= 0.0)
			{
				ShadeRayResult shade = (ShadeRayResult)0;
				fetchMaterialAndSurface(trans_ray, trans_hit, shade.mtrl, shade.surface);

				float3 transparent = 0.0;

				shade = shade_ray(trans_ray, trans_hit, shade.mtrl, shade.surface, rng);
				transparent += shade.radiance;
				transparent += trace_transparent(trans_ray, shade, true, 2, rng);
				transparent += trace_transparent(trans_ray, shade, false, 2, rng);

				// replace the radiance with our radiance
				radiance = transparent;

				// replace the hit with the 1st glass surface hit
				hit = trans_hit;
			}			
		}
#endif

#if 0
		// variance clamping
		float3 m1 = rgb_to_ycocg(radiance);
		float3 m2 = m1 * m1;
		for (int y = -1; y <= 1; y++)
		{
			for (int x = -1; x <= 1; x++)
			{
				int2 index = clamp(int2(tid.xy) + int2(x, y), 0.xx, int2(width-1, height-1));

				float3 c = rgb_to_ycocg(g_rtOutput[index].rgb);
				m1 += c;
				m2 += c * c;
			}
		}

		const float velocityConfidenceFactor = saturate(1.0 - length(mv) / 2.0);
		const float varianceGamma = lerp(0.75, 4.0, velocityConfidenceFactor * velocityConfidenceFactor);

		const float3 mean = m1 * (1.0/9.0);
		const float3 variance = sqrt(m2 * (1.0 / 9.0) - mean * mean) * varianceGamma;

		const float3 minC = float3(mean - variance);
		const float3 maxC = float3(mean + variance);

		float4 prevRadiance = g_rtOutput[tid.xy];
		prevRadiance.rgb = clamp(prevRadiance.rgb, ycocg_to_rgb(minC), ycocg_to_rgb(maxC));

		//TODO: load into groupshared, change to group shared barrier
		DeviceMemoryBarrierWithGroupSync();

		const float prevHitT = g_hitHistory[tid.xy];

		const bool hitInvalidate = abs(hit.hitT - prevHitT) > 0.1;
		const bool motionInvalidate = dot(mv, mv) > 0.1;
		const bool reset = g_constants.frameIndex == 0;// || motionInvalidate || hitInvalidate;

		float weight = reset ? 0.0 : velocityConfidenceFactor * prevRadiance.a;
		radiance = lerp(radiance, prevRadiance.rgb, weight);
		weight = saturate(1.0 / 2.0 - weight);
#else // !variance clamping
		const float prevHitT = g_hitHistory[tid.xy].x;
		const float prevMv = g_hitHistory[tid.xy].y;

#if INTEGRATED_TRANSMISSION || OPAQUE_TRANSPARENCY
		const float hitT = hit.minT;
#else
		const float hitT = hit.hitT;
#endif
		const float hit_diff = abs(hit.minT - hit.hitT);
		const float hit_thresh = hit_diff > 0.5 ? 100.0 : 2.1;
		// subtract .1 to account for loss of precision in the storage
		const float hit_weight = saturate((abs(hitT - prevHitT) - .1) / hit_thresh);

		// divide by maximum accepted mv length (8 cm)
		const float motion_len2 = dot(mv, mv);
		const float motion_weight = saturate(motion_len2 / 0.08);

		const float hit_lerp = (motion_len2 + prevMv ) > 0.0 ? 1.0 : 0.5;

		const float4 prevRadiance = g_rtOutput[tid.xy];
		const float time_weight = 1.0f / (1.0f + (1.0f / prevRadiance.a));

		float weight = 0.0;
		weight = time_weight;
		weight = lerp(weight, hit_lerp, hit_weight);
		weight = lerp(weight, 1.0, motion_weight);

		const bool reset = g_constants.frameIndex == 0;
		weight = reset ? 1.0 : saturate(weight);
		radiance = lerp(prevRadiance.rgb, radiance, weight);
#endif // variance clamping

		g_rtOutput[tid.xy] = float4(radiance, weight);
		g_hitHistory[tid.xy] = float2(hitT, motion_len2);
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
			RtInstanceAttachments att = g_attachments_buffer[instanceId];

			if (g_constants.debugView == DebugView_InstanceId)
			{
				value.rgb = instanceIdToColor(instanceId);
			}
			else if (g_constants.debugView == DebugView_Normals)
			{
				const uint3 indices = fetchIndices(att, primitiveIndex);
				const float3 geomNormal = fetchGeometryNormal(att, indices, transform);
				value.rgb = fetchShadingNormal(att, indices, baries, transform, geomNormal);
				value.rgb = value.rgb * 0.5 + 0.5;
			}
			else if (g_constants.debugView == DebugView_Uvs)
			{
				const uint3 indices = fetchIndices(att, primitiveIndex);
				float2 uvs = fetchUvs(att, indices, baries);
				value.rgb = float3(uvs, 0.0);
			}
			else if (g_constants.debugView == DebugView_Texture)
			{
				const uint3 indices = fetchIndices(att, primitiveIndex);
				float2 uvs = fetchUvs(att, indices, baries);
				float4 texcolor = fetchTexture(att, uvs);
				value = texcolor;
			}
			else if (g_constants.debugView == DebugView_Color)
			{
				const uint3 indices = fetchIndices(att, primitiveIndex);

				value = fetchVertexColor(att, indices, baries);
			}
			else if (g_constants.debugView == DebugView_Mtrl)
			{
				const uint3 indices = fetchIndices(att, primitiveIndex);

				const MaterialType mtrl = fetchVertexMtrl(att, indices, baries);

				if (mtrl == Material_Standard)
				{
					value.rgb = float3(1, 0, 0);
				}
				else
				{
					value.rgb = float3(0, 1, 0);
				}
			}
			else if (g_constants.debugView == DebugView_Motion)
			{
				const RtInstanceData data = g_instance_data_buffer[instanceId];

				const float3 hitpos = get_ray_hitpoint(ray, hit);
				const float3 prev_hitpos = mul(data.toWorldPrevT, float4(hitpos, 1.0));
				const float3 mv = hitpos - prev_hitpos;

				value.rgb = abs(mv) / 2.0;
			}
			else 
			{
				value.rgb = 0;
			}

			if (g_constants.debugChannel == 0)
				value.rgb = value.rrr;
			else if (g_constants.debugChannel == 1)
				value.rgb = value.ggg;
			else if (g_constants.debugChannel == 2)
				value.rgb = value.bbb;
			else if (g_constants.debugChannel == 3)
				value.rgb = value.aaa;
		}

		g_rtOutput[tid.xy] = float4(value.rgb, 1.0);
	}
	
}
