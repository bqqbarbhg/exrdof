#include "dof.hlsli"

[numthreads(8, 8, 1)]
void dof_nb_init(uint3 tid : SV_DispatchThreadID)
{
	float min_z = +1e38f;
	float max_z = -1e38f;

	int2 s = int2(tid.xy);

	[unroll] for (int dx = -1; dx <= +1; dx++)
	[unroll] for (int dy = -1; dy <= +1; dy++)
	{
		int2 p = s + int2(dx, dy);
		if (any(uint2(p) >= uint2(width, height))) continue;

		float p_z = src_z.Load(int3(p, 0));
		min_z = min(min_z, p_z);
		max_z = max(max_z, p_z);
	}

	float s_z = src_z.Load(int3(s, 0));

	pixel_nb nb;
	nb.min_z = min_z;
	nb.max_z = max_z;
	nb.z = s_z;
	nb.clean_z = s_z;
	nb.clean_level = MAX_CLEAN;

	float err = relative_error(depth_radius(min_z), depth_radius(max_z));
	if (err < clean_error) {
		nb.clean_level = 0;
	}

	if (all(uint2(s) < uint2(width, height))) {
		nbs[ctoi(s)] = nb;
	}
}
