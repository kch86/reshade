#ifndef MATERIAL_HLSL
#define MATERIAL_HLSL

#include "Common.h"
#include "Color.h"
#include "Sampling.h"

struct Surface
{
	float3 pos;
	float3 geom_normal;
	float3 shading_normal;
};

struct Material
{
	float3 base_color;
	float metalness;
	float3 emissive;
	float roughness;
};

struct Light
{
	float3 dir;
	float3 color;
};

struct Brdf
{
	// surface properties
	float3 specular_f0;
	float3 diffuse_reflectance;
	float3 N;

	float roughness;	// linear roughness
	float alpha;		// squared roughness
	float alpha2;		// squared alpha

	// view based terms
	float3 F; // fresnel

	float3 V; // view vector
	float3 H; // half vector between view and light

	float n_dot_v;
	float l_dot_h;
	float n_dot_h;
	float v_dot_h;

	// lighting terms
	float3 L;
	float n_dot_l;
};

// Converts a Blinn-Phong specular power to a Beckmann roughness parameter
float spec_power_to_roughness(float s)
{
	return sqrt(2.0 / (s + 2.0));
}

float pow2(float x)
{
	return x * x;
}

float pow5(float x)
{
	return x * x * x * x * x;
}

float3 diffuse_reflectance(float3 base_color, float metalness)
{
	return base_color * (1.0f - metalness);
}

float3 specular_f0(float3 base_color, float metalness)
{
	// Specifies minimal reflectance for dielectrics (when metalness is zero)
	// Nothing has lower reflectance than 2%, but we use 4% to have consistent results with UE4, Frostbite, et al.
	const float3 MinDialectricsF0 = 0.04;
	return lerp(MinDialectricsF0, base_color, metalness);
}

float3 specular_f90(float3 f0)
{
	// from ue4, anything less than 2% is not possible
	return saturate(get_luminance(f0) / 0.02).xxx;
}

float f_schlick(float F0, float F90, float VoNH)
{
	return lerp(F0, F90, pow5(1.0 - VoNH));
}

float3 f_schlick(float3 F0, float3 F90, float VoNH)
{
	return lerp(F0, F90, pow5(1.0 - VoNH));
}

float3 f_schlick(float3 F0, float VoNH)
{
	return  F0 + (1.0 - F0) * pow5(VoNH);
}

float d_ggx(Brdf brdf)
{
	const float f = (brdf.n_dot_h * brdf.alpha2 - brdf.n_dot_h) * brdf.n_dot_h + 1.0f;
	return (brdf.alpha2 / max(1e-6, pow2(f))) * M_INV_PI;
}

// Smith ggx specular geometric visibility function
float g_smith_ggx(float alpha, float NoX)
{
	return 2.0f / (1.0f + sqrt(alpha * alpha * (1.0f - NoX * NoX) / (NoX * NoX) + 1.0f));
}

float g_smith_ggx(Brdf brdf)
{
	return g_smith_ggx(brdf.alpha, brdf.n_dot_v) * g_smith_ggx(brdf.alpha, brdf.n_dot_l);
}

float diffuse_brdf_burley(Brdf brdf)
{
	const float fd90 = 0.5 + 2.0 * brdf.roughness * brdf.l_dot_h * brdf.l_dot_h;
	return f_schlick(1.0, fd90, brdf.n_dot_l) * f_schlick(1.0, fd90, brdf.n_dot_v);
}

float3 specular_brdf_ggx(Brdf brdf)
{
	float d = d_ggx(brdf);
	float g = g_smith_ggx(brdf);

	return d * g * brdf.F;
}

// Shlick's approximation for Ross BRDF - makes Fresnel converge to less than 1.0 when NoV is low
// https://hal.inria.fr/inria-00443630/file/article-1.pdf
float3 enironment_term_ross(float3 f0, float NoV, float linearRoughness)
{
	const float m = saturate(linearRoughness * linearRoughness);
	const float OneMinusNoV = saturate(1.0 - NoV);

	float f = pow(OneMinusNoV, 5.0 * exp(-2.69 * m)) / (1.0 + 22.7 * pow(m, 1.5));

	float scale = 1.0 - f;
	float bias = f;

	return saturate(f0 * scale + bias);
}

float estimate_specular_probability_ross(Material mtrl, float3 normal, float3 V)
{
	float3 albedo = diffuse_reflectance(mtrl.base_color, mtrl.metalness);
	float3 f0 = specular_f0(mtrl.base_color, mtrl.metalness);

	float n_dot_v = saturate(dot(normal, V));

	float3 f_env = enironment_term_ross(f0, n_dot_v, mtrl.roughness);

	float lumSpec = get_luminance(f_env);
	float lumDiff = get_luminance(albedo * (1.0 - f_env));

	float diffSpec = lumSpec / (lumDiff + lumSpec + 1e-6);

	return diffSpec < 0.005 ? 0.0 : diffSpec;
}

float estimate_specular_probability_simple(Material mtrl)
{
	return clamp(1.0 - mtrl.roughness, 0.1, 0.9);
}

float3 get_diffuse_ray(float2 u, float3 normal)
{
	return sample_hemisphere_cosine_fast(u, normal);
}

struct SpecularRay
{
	float3 dir; // reflected specular ray
	float3 weight;
};

SpecularRay get_specular_ray_simple(float3 V, float3 normal, float3 diffuse_ray, float roughness, float3 spec_refl)
{
	float3 refl_dir = reflect(V, normal);

	SpecularRay ray;
	ray.dir = normalize(lerp(refl_dir, diffuse_ray, pow2(roughness)));
	ray.weight = spec_refl;
	return ray;
}

float3 get_specular_ray_ndf_ggx(float2 rnd, float linearRoughness)
{
	float m = linearRoughness * linearRoughness;
	float m2 = m * m;
	float t = (m2 - 1.0) * rnd.y + 1.0;
	float cosThetaSq = (1.0 - rnd.y) * rcp(t);
	float sinTheta = sqrt(1.0 - cosThetaSq);
	float phi = rnd.x * (2.0 * M_PI);

	float3 ray;
	ray.x = sinTheta * cos(phi);
	ray.y = sinTheta * sin(phi);
	ray.z = sqrt(cosThetaSq);

	return ray;
}

SpecularRay get_specular_ray_vndf_ggx(
	float2 rnd, Material mtrl,
	float3 normal, float3 V,
	float trimFactor = 1.0)
{
	const float EPS = 1e-7;

	float4 qRotationToZ = getRotationToZAxis(normal);
	float3 Vlocal = rotatePoint(qRotationToZ, V);

	// TODO: instead of using 2 roughness values introduce "anisotropy" parameter
	// https://blog.selfshadow.com/publications/s2013-shading-course/rad/s2013_pbs_rad_notes.pdf (page 3)

	float m = mtrl.roughness * mtrl.roughness;

	// Section 3.2: transforming the view direction to the hemisphere configuration
	float3 Vh = normalize(float3(m * Vlocal.xy, Vlocal.z));

	// Section 4.1: orthonormal basis (with special case if cross product is zero)
	float lensq = dot(Vh.xy, Vh.xy);
	float3 T1 = lensq > EPS ? float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
	float3 T2 = cross(Vh, T1);

	// Section 4.2: parameterization of the projected area
	// trimFactor: 1 - full lobe, 0 - true mirror
	float r = sqrt(rnd.x * trimFactor);
	float phi = rnd.y * (2.0 * M_PI);
	float t1 = r * cos(phi);
	float t2 = r * sin(phi);
	float s = 0.5 * (1.0 + Vh.z);
	t2 = (1.0 - s) * sqrt(1.0 - t1 * t1) + s * t2;

	// Section 4.3: reprojection onto hemisphere
	float3 Nh = t1 * T1 + t2 * T2 + sqrt(1.0 - t1 * t1 - t2 * t2) * Vh;

	// Section 3.4: transforming the normal back to the ellipsoid configuration
	float3 Ne = normalize(float3(m * Nh.xy, max(Nh.z, EPS)));

	const float3 Hlocal = Ne;
	const float3 local_ray = reflect(-Vlocal, Hlocal);

	const float NoL = saturate(local_ray.z);
	const float VoH = abs(dot(Vlocal, Hlocal));

	const float3 rf0 = specular_f0(mtrl.base_color, mtrl.metalness);
	const float3 f = f_schlick(rf0, VoH);
	const float3 g = g_smith_ggx(m, NoL);

	SpecularRay ray;
	ray.dir = normalize(rotatePoint(invertRotation(qRotationToZ), local_ray));
	ray.weight = f * g;

	return ray;
}

#endif //MATERIAL_HLSL
