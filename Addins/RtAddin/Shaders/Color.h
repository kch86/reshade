#ifndef COLOR_HLSL
#define COLOR_HLSL

float3 to_srgb_from_linear(float3 x)
{
	//return x < 0.0031308 ? 12.92 * x : 1.055 * pow(x, 1.0 / 2.4) - 0.055;
	return select(x < 0.0031308, 12.92 * x, 1.055 * pow(x, 1.0 / 2.4) - 0.055);
}

float3 to_linear_from_srgb(float3 x)
{
	//return x < 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
	return select(x < 0.04045, x / 12.92, pow((x + 0.055) / 1.055, 2.4));
}

float get_luminance(float3 c)
{
	return dot(float3(0.2126, 0.7152, 0.0722), c);
}

float4 bgra_unorm_to_float(uint bgra)
{
	float a = ((bgra >> 24) & 0xff) / 255.0;
	float r = ((bgra >> 16) & 0xff) / 255.0;
	float g = ((bgra >> 8) & 0xff) / 255.0;
	float b = ((bgra >> 0) & 0xff) / 255.0;

	return float4(r, g, b, a);
}

float3 rgb_to_ycocg(float3 inRGB)
{
	//Y = R / 4 + G / 2 + B / 4
	//Co = R / 2 - B / 2
	//Cg = -R / 4 + G / 2 - B / 4
	const float y = dot(inRGB, float3(0.25f, 0.5f, 0.25f));
	const float co = dot(inRGB, float3(0.5f, 0.f, -0.5f));
	const float cg = dot(inRGB, float3(-0.25f, 0.5f, -0.25f));
	return float3(y, co, cg);
}

float3 ycocg_to_rgb(float3 inYCoCg)
{
	//R = Y + Co - Cg
	//G = Y + Cg
	//B = Y - Co - Cg
	const float r = dot(inYCoCg, float3(1.f, 1.f, -1.f));
	const float g = dot(inYCoCg, float3(1.f, 0.f, 1.f));
	const float b = dot(inYCoCg, float3(1.f, -1.f, -1.f));
	return float3(r, g, b);
}

#endif //COLOR_HLSL
