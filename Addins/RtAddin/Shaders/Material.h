#ifndef MATERIAL_HLSL
#define MATERIAL_HLSL

#include "Color.h"
#include "Math.h"
#include "Sampling.h"
#include "RtShared.h"

struct Surface
{
	float3 pos;
	float3 geom_normal;
	float3 shading_normal;
};

struct Material
{
	float3 tint;
	float3 base_color;
	float3 emissive;
	float metalness;
	float roughness;
	float opacity;
	MaterialType type;
	bool opaque;
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

// VoX == VoN || VoH
float f_schlick(float F0, float F90, float VoX)
{
	return lerp(F0, F90, pow5(1.0 - VoX));
}

float3 f_schlick(float3 F0, float3 F90, float VoX)
{
	return lerp(F0, F90, pow5(1.0 - VoX));
}

float3 f_schlick(float3 F0, float VoX)
{
	return  F0 + (1.0 - F0) * pow5(1.0 - saturate(VoX));
}

// http://www.pbr-book.org/3ed-2018/Reflection_Models/Specular_Reflection_and_Transmission.html
// eta - relative index of refraction "from" / "to"
float f_dialectric(float eta, float VoN)
{
#if 1
	float saSq = eta * eta * (1.0 - VoN * VoN);

	// Cosine of angle between negative normal and transmitted direction ( 0 for total internal reflection )
	float ca = sqrt_sat(1.0 - saSq);

	float Rs = (eta * VoN - ca) * rcp_safe(eta * VoN + ca);
	float Rp = (eta * ca - VoN) * rcp_safe(eta * ca + VoN);

	return 0.5 * (Rs * Rs + Rp * Rp);
#else
	// pbrt reference
	float cosTheta_i = VoN;

	if (cosTheta_i < 0)
	{
		eta = 1 / eta;
		cosTheta_i = -cosTheta_i;
	}

	float sin2Theta_i = 1 - sqr(cosTheta_i);
	float sin2Theta_t = sin2Theta_i / sqr(eta);

	if (sin2Theta_t >= 1)
		return 1.0;

	float cosTheta_t = sqrt_safe(1 - sin2Theta_t);

	float r_parl = (eta * cosTheta_i - cosTheta_t) / (eta * cosTheta_i + cosTheta_t);
	float r_perp = (cosTheta_i - eta * cosTheta_t) / (cosTheta_i + eta * cosTheta_t);
	return (sqr(r_parl) + sqr(r_perp)) / 2;
#endif
}

float d_ggx(Brdf brdf)
{
	const float f = (brdf.n_dot_h * brdf.alpha2 - brdf.n_dot_h) * brdf.n_dot_h + 1.0f;
	return (brdf.alpha2 / max(1e-6, pow2(f))) * M_INV_PI;
}

// Smith ggx specular geometric visibility function
// NoX == NoV || NoL
float g_smith_ggx(float alpha, float NoX)
{
	return 2.0f / (1.0f + sqrt(alpha * alpha * (1.0f - NoX * NoX) / (NoX * NoX) + 1.0f));
}

float g_smith(float roughness, float NoX)
{
	float m = roughness * roughness;
	float m2 = m * m;
	float a = NoX + sqrt_sat((NoX - m2 * NoX) * NoX + m2);

	return 2.0 * NoX * rcp_safe(a);
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

float3 ambient_brdf(Material mtrl, float3 N, float3 V, bool approximate = false)
{
	float3 albedo = diffuse_reflectance(mtrl.base_color, mtrl.metalness);
	float3 f0 = specular_f0(mtrl.base_color, mtrl.metalness);

	float3 Fenv = f0;
	if (!approximate)
	{
		float NoV = abs(dot(N, V));
		Fenv = enironment_term_ross(f0, NoV, mtrl.roughness);
	}

	float3 ambBRDF = albedo * (1.0 - Fenv) + Fenv;

	return ambBRDF;
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
	float3 refl_dir = reflect(-V, normal);

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
	float3x3 basis = create_basis_fast(normal);
	float3 Vlocal = RotateVector(basis, V);

	const float EPS = 1e-7;

	// TODO: instead of using 2 roughness values introduce "anisotropy" parameter
	// https://blog.selfshadow.com/publications/s2013-shading-course/rad/s2013_pbs_rad_notes.pdf (page 3)

	float2 m = mtrl.roughness * mtrl.roughness;

	// Section 3.2: transforming the view direction to the hemisphere configuration
	float3 Vh = normalize(float3(m * Vlocal.xy, Vlocal.z));

	// Section 4.1: orthonormal basis (with special case if cross product is zero)
	float lensq = dot(Vh.xy, Vh.xy);
	float3 T1 = lensq > EPS ? float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
	float3 T2 = cross(Vh, T1);

	// Section 4.2: parameterization of the projected area
	// trimFactor: 1 - full lobe, 0 - true mirror
	float r = sqrt_sat(rnd.x * trimFactor);
	float phi = rnd.y * M_2PI;
	float t1 = r * cos(phi);
	float t2 = r * sin(phi);
	float s = 0.5 * (1.0 + Vh.z);
	t2 = (1.0 - s) * sqrt_sat(1.0 - t1 * t1) + s * t2;

	// Section 4.3: reprojection onto hemisphere
	float3 Nh = t1 * T1 + t2 * T2 + sqrt_sat(1.0 - t1 * t1 - t2 * t2) * Vh;

	// Section 3.4: transforming the normal back to the ellipsoid configuration
	float3 Ne = normalize(float3(m * Nh.xy, max(Nh.z, EPS)));

	const float3 Hlocal = Ne;
	const float3 local_ray = reflect(-Vlocal, Hlocal);

	const float NoL = saturate(local_ray.z);
	const float VoH = abs(dot(Vlocal, Hlocal));

	const float3 rf0 = specular_f0(mtrl.base_color, mtrl.metalness);
	const float3 f = f_schlick(rf0, VoH);
	const float3 g = g_smith(mtrl.roughness, NoL);

	SpecularRay ray;
	ray.dir = RotateVectorInverse(basis, local_ray);
	ray.weight = f * g;

	return ray;
}

#endif //MATERIAL_HLSL
