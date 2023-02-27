#ifndef SAMPLING_HLSL
#define SAMPLING_HLSL
#define SAMPLING_HLSL

#include "Math.h"

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

float4 pcg4d_rng(uint4 v)
{
	v = v * 1664525u + 1013904223u;

	v.x += v.y * v.w;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v.w += v.y * v.z;

	v = v ^ (v >> 16u);

	v.x += v.y * v.w;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v.w += v.y * v.z;

	// Convert to float. The constant here is 2^-32.
	return float4(v) * 2.32830643654e-10;

	//return v;
}

enum RngType
{
	Pcg,
	Halton,
};

struct Rng
{
	uint4 _seed;
	RngType _type;

	Buffer<float> _samples;
	int _dim;
	int _offset;

	void init(uint2 xy, uint frameid, RngType type)
	{
		_seed = uint4(xy, frameid, 0);
		_type = type;
	}

	void set_dim(int dim)
	{
		_dim = (dim % 8) * 2048;
	}

	void set_buffer(Buffer<float> samples)
	{
		_samples = samples;
	}

	void set_sample_offset(int offset)
	{
		if (_type == RngType::Pcg)
		{
			_seed.w = offset;
		}
		else if (_type == RngType::Halton)
		{
			_offset = offset % 2048;
		}		
	}
	float2 gen()
	{
		if (_type == RngType::Pcg)
		{
			const float2 rng = pcg4d_rng(_seed).xy;
			_seed.w += 1;
			return rng;
		}
		else if (_type == RngType::Halton)
		{
			const float2 rotate = pcg4d_rng(_seed).xy;
			const float2 rng = _samples[_dim + _offset];
			_offset = (_offset + 1) % 2048;
			_seed.w += 1;

			//return rng;
			//return rotate;
			return frac(rng + rotate);
		}
		return 0.0.xx;
	}
};

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
