#include "ReShade.fxh"

texture texColorBuffer : COLOR;

sampler samplerColor
{
	Texture = texColorBuffer;
};

texture rtTexture
{ 
	Width = BUFFER_WIDTH;
	Height = BUFFER_HEIGHT; 
	Format = RGBA8; 
};

sampler samplerRtTexture
{
	Texture = rtTexture;
};

storage2D rtStorage
{
	// The texture to be used as storage.
	Texture = rtTexture;

	// The mipmap level of the texture to fetch/store.
	MipLevel = 0;
};


void RtRayGen(uint3 tid : SV_GroupThreadID)
{
	tex2Dstore(rtStorage, tid.xy, float4(1.0, 0.2, 0.2, 1.0));
}

void RtDummyPs(float4 pos : SV_Position, float2 texcoord : TEXCOORD0, out float4 color : SV_Target)
{
	//color = float4(1.0, 0.2, 0.2, 1.0);
	color = 1.0;
}

void RtBlit(float4 pos : SV_Position, float2 texcoord : TEXCOORD0, out float4 color : SV_Target)
{
	/*float4 fogcolor = float4(FogRGBBalance.r, FogRGBBalance.g, FogRGBBalance.b,	Fog_Intensity);
	float depth = tex2D(samplerDepth, texcoord).r;
	depth = Fog_Intensity / (-99.0 * depth + 101.0);
	color = tex2D(samplerColor, texcoord);
	color = depth * fogcolor + (Fog_Additive - depth) * color;*/

	color = tex2D(samplerColor, texcoord);
	
	const float4 modifier = tex2D(samplerRtTexture, texcoord);
	color = modifier * color;
}

technique Raytracing < enabled = true; >
{		
	/*pass p0
	{
		DispatchSizeX = (BUFFER_WIDTH + 7) / 8;
		DispatchSizeY = (BUFFER_HEIGHT + 7) / 8;
		ComputeShader = RtRayGen<8, 8, 1>;
	}*/
	pass p0
	{
		RenderTarget = rtTexture;
		VertexShader = PostProcessVS;
		PixelShader = RtDummyPs;
	}
	pass p1
	{
		VertexShader = PostProcessVS;
		PixelShader = RtBlit;
	}	
}