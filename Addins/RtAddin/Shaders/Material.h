#ifndef MATERIAL_HLSL
#define MATERIAL_HLSL

#include "Common.h"
#include "Color.h"

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

float f_schlick(float F0, float F90, float cosine)
{
	return lerp(F0, F90, pow5(1.0 - cosine));
}

float3 f_schlick(float3 F0, float3 F90, float cosine)
{
	return lerp(F0, F90, pow5(1.0 - cosine));
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

#endif //MATERIAL_HLSL
