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

float3x3 create_basis(in float3 N)
{
	float3 U, V, W;

#if 0
	W = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
	U = normalize(cross(W, N));
	V = cross(N, U);
	W = N;
#else
	W = N;
	U = cross(float3(0.0f, 0.0f, 1.0f), W);

	// N already pointing down z-axis
	if (abs(dot(U, U)) < 1e-3f)
	{
		U = cross(W, float3(0.0f, 1.0f, 0.0f));
	}

	U = normalize(U);
	V = normalize(cross(U, W));
	//W = normalize( cross( U, V ) );
#endif

	return float3x3(U, V, W);
}

float3 sample_sphere(float2 u)
{
	float z = 2.0 * u[1] - 1.0;
	float phi = 2.0 * M_PI * u[0];
	float r = sqrt(1.0 - z * z);
	float x = cos(phi) * r;
	float y = sin(phi) * r;
	return float3(x, y, z);
}

float3 sample_hemisphere_cosine_fast(float2 u, float3 n)
{
	return normalize(n + sample_sphere(u));
}

float3 sample_hemisphere_cosine(float2 u, float3x3 basis)
{
	float r = sqrt(u.x);
	float theta = 2.0 * M_PI * u.y;

	float3 dir = float3(r * sin(theta), r * cos(theta), sqrt(1.0 - u.x));

	return normalize(mul(dir, basis));
}

float3 sample_hemisphere_cosine(float2 u, float3 n)
{
	const float3x3 basis = create_basis(n);

	float r = sqrt(u.x);
	float theta = 2.0 * M_PI * u.y;

	float3 dir = float3(r * sin(theta), r * cos(theta), sqrt(1.0 - u.x));

	return normalize(mul(dir, basis));
}

#endif //
