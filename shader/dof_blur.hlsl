#include "dof.hlsli"

uint hash( uint x ) {
    x += ( x << 10u );
    x ^= ( x >>  6u );
    x += ( x <<  3u );
    x ^= ( x >> 11u );
    x += ( x << 15u );
    return x;
}

float4 blur_evaluate(int2 s, float s_z)
{
	float4 sum = 0.0f;
	float weight = 0.0f;

	float s_rad = depth_radius(s_z);

	for (int dy = -kernel_sz; dy <= +kernel_sz; dy++)
	for (int dx = -kernel_sz; dx <= +kernel_sz; dx++)
	{
		int d2 = dx*dx + dy*dy;
		if (d2 > kernel_sz2) continue;

#if 0
		if (kernel_reduce > 0.0 && d2 > 25) {
			float h = float(hash(uint(dx) << 16 | uint(dy)) >> 16) * (1.0 / 65535.0f);
			if (h - kernel_reduce < 0.0f) continue;
		}
#endif

		int2 p = s + int2(dx, dy);
		if (any(uint2(p) >= uint2(width, height))) continue;

		pixel_nb p_nb = nbs[ctoi(p)];
		
		float p_z = p_nb.z;
		float p_rad = depth_radius(p_z);
		if (p_z > s_z) p_rad = min(p_rad, s_rad);

		float w = 1.0f / (1.0f + p_rad);
		if (p_nb.clean_level == MAX_CLEAN) w /= max_nonclean_penalty;
		else if (p_nb.clean_level > 0) w /= min_nonclean_penalty;

		if (float(d2) > p_rad) w = 0.0f;

		if (w > 1e-10f) {
			sum += src_rgba.Load(int3(p, 0)) * w;
			weight += w;
		}
	}

	if (weight > 1e-10f) {
		sum /= weight;
	}

	return sum;
}

[numthreads(8, 8, 1)]
void dof_blur(uint3 tid : SV_DispatchThreadID)
{
	float4 result;

	int2 s = int2(tid.xy);
	uint si = ctoi(s);

	[branch] if (nbs[si].clean_level < MAX_CLEAN) {
		result = blur_evaluate(s, nbs[si].z);
	} else {
		float min_z = nbs[si].min_z;
		float max_z = nbs[si].max_z;
		float4 n = blur_evaluate(s, min_z);
		float4 f = blur_evaluate(s, max_z);
		float t = (nbs[si].z - min_z) / (max_z - min_z);
		result = n * (1.0f - t) + f * t;
	}

	if (all(uint2(s) < uint2(width, height))) {
		dst_rgba[s] = result;
	}
}
