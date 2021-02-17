
struct vs_out {
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD0;
};

vs_out display_vs(float2 uv : TEXCOORD0)
{
	vs_out o;
	o.pos.xy = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
	o.pos.zw = float2(0.5, 1.0);
	o.uv = uv;
	return o;
}