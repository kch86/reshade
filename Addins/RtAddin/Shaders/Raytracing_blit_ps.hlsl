
#include "Color.h"

Texture2D<float4> texture : register(t0);

float4 blit_ps(in float4 position : SV_Position, in float2 texcoord : TEXCOORD0) : SV_TARGET0
{
	uint width, height;
	texture.GetDimensions(width, height);
	uint2 index = uint2(texcoord * float2(width, height) + 0.5);

	//TODO: exposure and tone mapping
	float3 color = texture[index].rgb;
	color = to_srgb_from_linear(color);

	return float4(color, 1.0);
}
