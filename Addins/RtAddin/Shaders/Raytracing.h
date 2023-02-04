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

RayHit trace_ray_closest(RaytracingAccelerationStructure tlas, RayDesc ray)
{
	RayQuery<RAY_FLAG_FORCE_OPAQUE> query;

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

RayHit trace_ray_occlusion(RaytracingAccelerationStructure tlas, RayDesc ray)
{
	RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;

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
