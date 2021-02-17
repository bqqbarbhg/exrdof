
cbuffer cb_display
{
	float exposure;
};

struct vs_in {
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD0;
};

Texture2D<float4> dst_rgba : register(t0);
sampler smp : register(s0);

float linear_to_srgb(float v)
{
	if (v <= 0.0031308f)
		return 12.92f * v;
	else
		return 1.055f*pow(v, (1.0f / 2.4f)) - 0.055f;
}

float3 linear_to_srgb(float3 v)
{
	return float3(linear_to_srgb(v.x), linear_to_srgb(v.y), linear_to_srgb(v.z));
}

float4 display_ps(vs_in i) : SV_TARGET
{
	float4 v = dst_rgba.Sample(smp, i.uv) * exposure;

	v.xyz = linear_to_srgb(v.xyz);

	return v;
}
