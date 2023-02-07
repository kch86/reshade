#ifndef RAYTRACING_HLSL
#define RAYTRACING_HLSL

struct RayHit
{
	float hitT;
	uint instanceId;
	uint primitiveId;
	float2 barycentrics;
	float3x4 transform;
};

RayHit trace_ray_closest_opaque(RaytracingAccelerationStructure tlas, RayDesc ray)
{
	RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;

	const uint ray_flags = 0;
	const uint ray_instance_mask = 0xffffffff;
	query.TraceRayInline(tlas, ray_flags, /*ray_instance_mask*/1, ray);
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
	RayQuery<RAY_FLAG_FORCE_OPAQUE /*| RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES*/> query;

	const uint ray_flags = 0;
	query.TraceRayInline(tlas, ray_flags, 2, ray);
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

#if 1
// handles non-opaque geometry
// must define ON_TRANPARENT_HIT(RayHit hit, out bool acceptHit);
// TODO: when moving to hlsl 2021, template this
#ifndef ON_TRANPARENT_HIT
#error "ON_TRANPARENT_HIT undefined"
#endif
RayHit trace_ray_closest_transparent(RaytracingAccelerationStructure tlas, RayDesc ray)
{
	RayQuery<RAY_FLAG_CULL_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;

	const uint ray_flags = 0;
	const uint ray_instance_mask = 0xffffffff;
	query.TraceRayInline(tlas, ray_flags, ray_instance_mask, ray);

	while (query.Proceed())
	{
		RayHit hit;
		hit.hitT = query.CandidateTriangleRayT();
		hit.instanceId = query.CandidateInstanceID();
		hit.primitiveId = query.CandidatePrimitiveIndex();
		hit.barycentrics = query.CandidateTriangleBarycentrics();
		hit.transform = query.CandidateObjectToWorld3x4();

		bool acceptHit = false;
		ON_TRANPARENT_HIT(hit, acceptHit);

		if (acceptHit)
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
RayHit trace_ray_closest_all(RaytracingAccelerationStructure tlas, RayDesc ray)
{
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;

	const uint ray_flags = 0;
	const uint ray_instance_mask = 0xffffffff;
	query.TraceRayInline(tlas, ray_flags, ray_instance_mask, ray);

	while (query.Proceed())
	{
		RayHit hit;
		hit.hitT = query.CandidateTriangleRayT();
		hit.instanceId = query.CandidateInstanceID();
		hit.primitiveId = query.CandidatePrimitiveIndex();
		hit.barycentrics = query.CandidateTriangleBarycentrics();
		hit.transform = query.CandidateObjectToWorld3x4();

		bool acceptHit = false;
		ON_TRANPARENT_HIT(hit, acceptHit);

		if (acceptHit)
		{
			query.CommitNonOpaqueTriangleHit();
		}
	}

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

RayHit trace_ray_closest_any(RaytracingAccelerationStructure tlas, RayDesc ray, uint ray_instance_mask)
{
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
	//RayQuery<RAY_FLAG_NONE> query;

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

		bool acceptHit = false;
		ON_TRANPARENT_HIT(hit, acceptHit);

		if (acceptHit)
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
#endif

template<typename T>
RayHit trace_ray_closest_any_t(RaytracingAccelerationStructure tlas, RayDesc ray, uint ray_instance_mask)
{
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
	//RayQuery<RAY_FLAG_NONE> query;

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

		if(T::visit(hit))
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

float3 get_ray_hitpoint(RayDesc ray, RayHit hit)
{
	return ray.Origin + ray.Direction * hit.hitT;
}

float3 get_ray_origin_offset(RayDesc ray, float3 pos, float3 normal)
{
	return pos + normal * length(pos - ray.Origin) * 0.00001;
}


#endif // RAYTRACING_HLSL
