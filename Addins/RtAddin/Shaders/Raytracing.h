#ifndef RAYTRACING_HLSL
#define RAYTRACING_HLSL

struct RayHit
{
	float hitT;
	float minT;
	uint instanceId;
	uint primitiveId;
	float2 barycentrics;
	float3x4 transform;
};

RayHit trace_ray_closest_opaque(RaytracingAccelerationStructure tlas, RayDesc ray)
{
	RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;

	const uint ray_flags = 0;
	const uint ray_instance_mask = 0xffffffff;
	query.TraceRayInline(tlas, ray_flags, ray_instance_mask, ray);
	query.Proceed();

	RayHit hit;
	hit.hitT = -1.0;

	if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		hit.hitT = query.CommittedRayT();
		hit.instanceId = query.CommittedInstanceID();
		hit.primitiveId = query.CommittedPrimitiveIndex();
		hit.barycentrics = query.CommittedTriangleBarycentrics();
		hit.transform = query.CommittedObjectToWorld3x4();
	}

	return hit;
}

RayHit trace_ray_closest_opaque_mask(RaytracingAccelerationStructure tlas, RayDesc ray, uint ray_instance_mask)
{
	RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;

	const uint ray_flags = 0;
	query.TraceRayInline(tlas, ray_flags, ray_instance_mask, ray);
	query.Proceed();

	RayHit hit;
	hit.hitT = -1.0;

	if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		hit.hitT = query.CommittedRayT();
		hit.instanceId = query.CommittedInstanceID();
		hit.primitiveId = query.CommittedPrimitiveIndex();
		hit.barycentrics = query.CommittedTriangleBarycentrics();
		hit.transform = query.CommittedObjectToWorld3x4();
	}

	return hit;
}

struct RayCandidate
{
	bool hit;
	float opacity;

	static RayCandidate init(bool hit, float opacity)
	{
		RayCandidate c;
		c.hit = hit;
		c.opacity = opacity;
		return c;
	}
};

template<typename T>
RayHit trace_ray_closest_transparent(RaytracingAccelerationStructure tlas, RayDesc ray, uint ray_instance_mask)
{
	RayQuery<RAY_FLAG_CULL_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;

	const uint ray_flags = 0;
	query.TraceRayInline(tlas, ray_flags, ray_instance_mask, ray);

	while (query.Proceed())
	{
		RayHit hit;
		hit.hitT = query.CandidateTriangleRayT();
		hit.instanceId = query.CandidateInstanceID();
		hit.primitiveId = query.CandidatePrimitiveIndex();
		hit.barycentrics = query.CandidateTriangleBarycentrics();
		hit.transform = query.CandidateObjectToWorld3x4();

		RayCandidate c = T::visit(ray, hit);
		if (c.hit)
		{
			query.CommitNonOpaqueTriangleHit();
		}
	}

	RayHit hit = (RayHit)0;
	hit.hitT = -1.0;

	if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		hit.hitT = query.CommittedRayT();
		hit.instanceId = query.CommittedInstanceID();
		hit.primitiveId = query.CommittedPrimitiveIndex();
		hit.barycentrics = query.CommittedTriangleBarycentrics();
		hit.transform = query.CommittedObjectToWorld3x4();
	}

	return hit;
}

template<typename T>
RayHit trace_ray_closest_any(RaytracingAccelerationStructure tlas, RayDesc ray, uint ray_instance_mask)
{
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;

	const uint ray_flags = 0;
	query.TraceRayInline(tlas, ray_flags, ray_instance_mask, ray);

	float minT = ray.TMax;

	while (query.Proceed())
	{
		RayHit hit;
		hit.hitT = query.CandidateTriangleRayT();
		hit.instanceId = query.CandidateInstanceID();
		hit.primitiveId = query.CandidatePrimitiveIndex();
		hit.barycentrics = query.CandidateTriangleBarycentrics();
		hit.transform = query.CandidateObjectToWorld3x4();

		RayCandidate c = T::visit(ray, hit);
		if (c.hit)
		{
			query.CommitNonOpaqueTriangleHit();
		}

		if(c.opacity > 0.0)
			minT = min(minT, hit.hitT);
	}

	RayHit hit = (RayHit)0;
	hit.hitT = -1.0;
	hit.minT = -1.0;

	if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		hit.hitT = query.CommittedRayT();
		hit.instanceId = query.CommittedInstanceID();
		hit.primitiveId = query.CommittedPrimitiveIndex();
		hit.barycentrics = query.CommittedTriangleBarycentrics();
		hit.transform = query.CommittedObjectToWorld3x4();
		hit.minT = min(minT, hit.hitT);
	}

	return hit;
}

RayHit trace_ray_occlusion(RaytracingAccelerationStructure tlas, RayDesc ray)
{
	RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;

	const uint ray_flags = 0;
	const uint ray_instance_mask = 0xffffffff;
	query.TraceRayInline(tlas, ray_flags, ray_instance_mask, ray);
	query.Proceed();

	RayHit hit;
	hit.hitT = -1.0;

	if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		hit.hitT = query.CommittedRayT();
		hit.instanceId = query.CommittedInstanceID();
		hit.primitiveId = query.CommittedPrimitiveIndex();
		hit.barycentrics = query.CommittedTriangleBarycentrics();
		hit.transform = query.CommittedObjectToWorld3x4();
	}

	return hit;
}

template<typename T>
RayHit trace_ray_occlusion_any(RaytracingAccelerationStructure tlas, RayDesc ray, uint ray_instance_mask)
{
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;

	const uint ray_flags = 0;
	query.TraceRayInline(tlas, ray_flags, ray_instance_mask, ray);

	while (query.Proceed())
	{
		RayHit hit;
		hit.hitT = query.CandidateTriangleRayT();
		hit.instanceId = query.CandidateInstanceID();
		hit.primitiveId = query.CandidatePrimitiveIndex();
		hit.barycentrics = query.CandidateTriangleBarycentrics();
		hit.transform = query.CandidateObjectToWorld3x4();

		RayCandidate c = T::visit(ray, hit);
		if (c.hit)
		{
			query.CommitNonOpaqueTriangleHit();
		}
	}

	RayHit hit = (RayHit)0;
	hit.hitT = -1.0;

	if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		hit.hitT = query.CommittedRayT();
		hit.instanceId = query.CommittedInstanceID();
		hit.primitiveId = query.CommittedPrimitiveIndex();
		hit.barycentrics = query.CommittedTriangleBarycentrics();
		hit.transform = query.CommittedObjectToWorld3x4();
	}

	return hit;
}

float3 get_ray_hitpoint(RayDesc ray, RayHit hit)
{
	return ray.Origin + ray.Direction * hit.hitT;
}

float3 get_ray_origin_offset(RayDesc ray, float3 pos, float3 normal)
{
#if 1
	float3 X = pos;
	int3 o = int3(normal * 256.0);
	float3 a = asfloat(asint(X) + select(X < 0.0, -o, o));
	float3 b = X + normal * (1.0 / 65536.0);

	X = select(abs(X) < (1.0 / 32.0), b, a);
	return X;
#else
	return pos + normal * length(pos - ray.Origin) * 0.00001;
#endif
}


#endif // RAYTRACING_HLSL
