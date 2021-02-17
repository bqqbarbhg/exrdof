#include "dof.hlsli"

[numthreads(8, 8, 1)]
void dof_nb_spread(uint3 tid : SV_DispatchThreadID)
{
	int2 s = int2(tid.xy);
	uint si = ctoi(s);

	if (all(uint2(s) < uint2(width, height)) && nbs[si].clean_level <= target_level) {

		float s_z = nbs[si].z;
		float s_rad = depth_radius(s_z);

		float best_err = spread_error;
		float best_z = 0.0f;

		[unroll] for (int dy = -2; dy <= +2; dy++)
		[unroll] for (int dx = -2; dx <= +2; dx++)
		{
			int2 p = s + int2(dx, dy);
			if (any(uint2(p) >= uint2(width, height))) continue;
			uint pi = ctoi(p);

			int p_level = nbs[pi].clean_level;
			if (p_level < target_level) {
				float p_z = nbs[pi].clean_z;
				float p_rad = depth_radius(p_z);
				float err = relative_error(s_rad, p_rad);
				if (err < best_err) {
					best_err = err;
					best_z = p_z;
				}
			}
		}

		if (best_err < spread_error) {
			nbs[si].clean_z = best_z;
			nbs[si].clean_level = target_level;
		}
	}
}
