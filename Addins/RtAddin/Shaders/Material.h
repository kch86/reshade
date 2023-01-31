#ifndef MATERIAL_HLSL
#define MATERIAL_HLSL
#define MATERIAL_HLSL

#include "Common.h"

struct Surface
{
	float3 pos;
	float3 norm;
	float3 view;
	float3 reflectance;
	float n_dot_v;
	float roughness;
};

struct Material
{
	float3 albedo;
	float3 albedo_tint;
	float3 specular;
	float roughness;
};

struct Light
{
	float3 dir;
	float3 color;
	float n_dot_l;
	float l_dot_h;
	float n_dot_h;
};

// Converts a Blinn-Phong specular power to a Beckmann roughness parameter
float spec_power_to_roughness(float s)
{
	return sqrt(2.0f / (s + 2.0f));
}

float pow2(float x)
{
	return x * x;
}

float pow5(float x)
{
	return x * x * x * x * x;
}

float f_schlick(float F0, float F90, float cosine)
{
	return lerp(F0, F90, pow5(1.0 - cosine));
}

float3 f_schlick(float3 F0, float3 F90, float cosine)
{
	return lerp(F0, F90, pow5(1.0 - cosine));
}

float d_ggx(float roughness, float NoH)
{
	const float alpha = pow2(roughness);
	const float alpha2 = pow2(alpha);

	const float f = (NoH * alpha2 - NoH) * NoH + 1.0f;
	return (alpha2 / max(1e-6, pow2(f))) * M_INV_PI;
}

// Smith ggx specular geometric visibility function
float g_smith_ggx(float alpha, float NoX)
{
	return 2.0f / (1.0f + sqrt(alpha * alpha * (1.0f - NoX * NoX) / (NoX * NoX) + 1.0f));
}

float g_smith_ggx(Surface surface, Light light)
{
	const float alpha = pow2(surface.roughness);
	return g_smith_ggx(alpha, surface.n_dot_v) * g_smith_ggx(alpha, light.n_dot_l);
}

float diffuse_brdf_burley(Surface surface, Light light)
{
	const float fd90 = 0.5 + 2.0 * surface.roughness * light.l_dot_h * light.l_dot_h;
	return f_schlick(1.0, fd90, light.n_dot_l) * f_schlick(1.0, fd90, surface.n_dot_v);
}

float3 specular_brdf_ggx(Surface surface, Light light)
{
	float d = d_ggx(surface.roughness, light.n_dot_h);

	float g = g_smith_ggx(surface, light);

	float3 f = f_schlick(surface.reflectance, 1.0, light.l_dot_h);

	return d * g * f;
}

#endif //
