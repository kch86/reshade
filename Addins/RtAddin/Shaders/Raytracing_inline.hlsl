//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#ifndef RAYTRACING_HLSL
#define RAYTRACING_HLSL

#define HLSL
#include "RaytracingHlslCompat.h"
//
//// Subobjects definitions at library scope. 
//GlobalRootSignature MyGlobalRootSignature =
//{
//    "DescriptorTable( UAV( u0 ) ),"                        // Output texture
//    "SRV( t0 ),"                                           // Acceleration structure
//    "CBV( b0 ),"                                           // Scene constants
//    "DescriptorTable( SRV( t1, numDescriptors = 2 ) )"     // Static index and vertex buffers.
//};
//
//LocalRootSignature MyLocalRootSignature = 
//{
//    "RootConstants( num32BitConstants = 4, b1 )"           // Cube constants        
//};
//
//TriangleHitGroup MyHitGroup =
//{
//    "",                     // AnyHit
//    "MyClosestHitShader",   // ClosestHit
//};
//
//SubobjectToExportsAssociation  MyLocalRootSignatureAssociation =
//{
//    "MyLocalRootSignature",  // subobject name
//    "MyHitGroup"             // export association 
//};
//
//RaytracingShaderConfig  MyShaderConfig =
//{
//    16, // max payload size
//    8   // max attribute size
//};
//
//RaytracingPipelineConfig MyPipelineConfig =
//{
//    1 // max trace recursion depth
//};


RaytracingAccelerationStructure Scene : register(t0, space0);
RWTexture2D<float4> RenderTarget : register(u0);


[numthreads(8, 8, 1)]
void ray_gen(uint3 tid : SV_GroupThreadID)
{
    //trace

    // Write the raytraced color to the output texture.
    RenderTarget[tid.xy] = float4(1.0, 0.2, 0.2, 1.0);
}


#endif // RAYTRACING_HLSL
