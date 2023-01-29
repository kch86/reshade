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

float d_ggx(Surface surface, Light Light)
{
	const float alpha = pow2(surface.roughness);
	const float alpha2 = pow2(alpha);
	const float NoH2 = pow2(Light.n_dot_h);

	const float lower = lerp(1, alpha2, NoH2);
	return alpha2 / max(1e-6, M_PI * pow2(lower));
}

// Schlick-Smith specular geometric visibility function
float g_schlick_smith(Surface surface, Light Light)
{
	const float alpha = pow2(surface.roughness);
	return 1.0 / max(1e-6, lerp(surface.n_dot_v, 1, alpha * 0.5) * lerp(Light.n_dot_l, 1, alpha * 0.5));
}

float3 diffuse_brdf_burley(Surface surface, Light light)
{
	const float fd90 = 0.5 + 2.0 * surface.roughness * light.l_dot_h * light.l_dot_h;
	return f_schlick(1.0, fd90, light.n_dot_l).x * f_schlick(1.0, fd90, surface.n_dot_v).x;
}

float3 specular_brdf_ggx(Surface surface, Light light)
{
	float d = d_ggx(surface, light);

	float g = g_schlick_smith(surface, light);

	float3 f = f_schlick(surface.reflectance, 1.0, light.l_dot_h);

	return d * g * f;
}

#endif //
