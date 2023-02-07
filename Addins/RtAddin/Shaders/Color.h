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

#endif //COLOR_HLSL
