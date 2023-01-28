#ifndef MATERIAL_HLSL
#define MATERIAL_HLSL
#define MATERIAL_HLSL

struct Surface
{
	float3 pos;
	float3 norm;
	float3 view;
	float n_dot_v;
	float roughness;
};

struct Material
{
	float3 albedo;
	float3 albedo_tint;
	float3 specular;
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

float pow5(float x)
{
	return x * x * x * x * x;
}

float f_schlick(float F0, float F90, float cosine)
{
	return lerp(F0, F90, pow5(1.0 - cosine));
}

float3 diffuse_burley(Surface surface, Light light)
{
	const float fd90 = 0.5 + 2.0 * surface.roughness * light.l_dot_h * light.l_dot_h;
	return f_schlick(1.0, fd90, light.n_dot_l).x * f_schlick(1.0, fd90, surface.n_dot_v).x;
}

#endif //
