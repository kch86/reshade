#include "ReShade.fxh"

uniform bool g_showRtResultFull <ui_tooltip = "Show Rt Result Fullscreen";> = false;
uniform bool g_showRtResultHalf <ui_tooltip = "Show Rt Result Halfscreen";> = false;

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
	color = 1.0;
}

void RtBlit(float4 pos : SV_Position, float2 texcoord : TEXCOORD0, out float4 color : SV_Target)
{
	color = tex2D(samplerColor, texcoord);
	
	const float4 modifier = tex2D(samplerRtTexture, texcoord);

	if (g_showRtResultFull)
	{
		color = modifier;
	}
	else if (pos.x >= (BUFFER_WIDTH/2) && g_showRtResultHalf)
	{
		color = modifier;
	}
}

technique Raytracing < enabled = true; >
{		
	// pass 0 is the dummy place holder pass we will render/replace
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