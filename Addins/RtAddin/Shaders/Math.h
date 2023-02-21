#ifndef COMMON_HLSL
#define COMMON_HLSL

#define M_PI 3.141592653589793238462643
#define M_2PI (2 * 3.141592653589793238462643)
#define M_INV_PI 0.318309886183790671537768

#define sqrt_sat(x) sqrt(saturate(x))
#define sqrt_safe(x) sqrt(max(0.0, x))
#define rcp_safe(x) rcp(max(x, 1e-15))
#define sqr(x) (x * x)

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

float3x3 create_basis_fast(float3 N)
{
	float sz = sign(N.z);
	float a = 1.0 / (sz + N.z);
	float ya = N.y * a;
	float b = N.x * ya;
	float c = N.x * sz;

	float3 T = float3(c * N.x * a - 1.0, sz * b, c);
	float3 B = float3(b, N.y * ya - sz, N.y);

	// Note: due to the quaternion formulation, the generated frame is rotated by 180 degrees,
	// s.t. if N = (0, 0, 1), then T = (-1, 0, 0) and B = (0, -1, 0).
	return float3x3(T, B, N);
}


float3 RotateVector(float3x3 m, float3 v)
{
	return mul(m, v);
}

float3 RotateVectorInverse(float3x3 m, float3 v)
{
	return mul(transpose(m), v);
}

// Calculates rotation quaternion from input vector to the vector (0, 0, 1)
// Input vector must be normalized!
float4 getRotationToZAxis(float3 input)
{
	// Handle special case when input is exact or near opposite of (0, 0, 1)
	if (input.z < -0.99999f) return float4(1.0f, 0.0f, 0.0f, 0.0f);

	return normalize(float4(input.y, -input.x, 0.0f, 1.0f + input.z));
}

// Returns the quaternion with inverted rotation
float4 invertRotation(float4 q)
{
	return float4(-q.x, -q.y, -q.z, q.w);
}

// Optimized point rotation using quaternion
// Source: https://gamedev.stackexchange.com/questions/28395/rotating-vector3-by-a-quaternion
float3 rotatePoint(float4 q, float3 v)
{
	const float3 qAxis = float3(q.x, q.y, q.z);
	return 2.0f * dot(qAxis, v) * qAxis + (q.w * q.w - dot(qAxis, qAxis)) * v + 2.0f * q.w * cross(qAxis, v);
}

#endif //COMMON_HLSL
