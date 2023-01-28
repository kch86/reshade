#ifndef SAMPLING_HLSL
#define SAMPLING_HLSL
#define SAMPLING_HLSL


#define M_PI 3.141592653589793238462643

float2 pcg2d_rng(inout uint2 seed)
{
	// This is PCG2D: https://jcgt.org/published/0009/03/02/
	seed = 1664525u * seed + 1013904223u;
	seed.x += 1664525u * seed.y;
	seed.y += 1664525u * seed.x;
	seed ^= (seed >> 16u);
	seed.x += 1664525u * seed.y;
	seed.y += 1664525u * seed.x;
	seed ^= (seed >> 16u);
	// Convert to float. The constant here is 2^-32.
	return float2(seed) * 2.32830643654e-10;
}

float3 sample_sphere(float2 random_numbers)
{
	float z = 2.0 * random_numbers[1] - 1.0;
	float phi = 2.0 * M_PI * random_numbers[0];
	float x = cos(phi) * sqrt(1.0 - z * z);
	float y = sin(phi) * sqrt(1.0 - z * z);
	return float3(x, y, z);
}


// Like sample_sphere() but only samples the hemisphere where the dot product
// with the given normal (n) is >= 0
float3 sample_hemisphere(float2 random_numbers, float3 normal)
{
	float3 direction = sample_sphere(random_numbers);
	if (dot(normal, direction) < 0.0)
		direction -= 2.0 * dot(normal, direction) * normal;
	return direction;
}

#endif //
