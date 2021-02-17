
cbuffer cb_consts
{
	uint width;
	uint height;
	float clean_error;
	float spread_error;
	float max_blur;
	float focal_plane;
	int kernel_sz;
	int kernel_sz2;
	float kernel_reduce;
	float min_nonclean_penalty;
	float max_nonclean_penalty;
	int target_level;
};

struct pixel_nb
{
	float min_z, max_z;
	float z;
	float clean_z;
	int clean_level;
};

float relative_error(float a, float b)
{
	return abs(a - b) / max(max(a, b), 1e-20f);
}

float depth_radius(float z)
{
	return clamp(z / 150.0f * max_blur - focal_plane, 0.0, max_blur);
}

uint ctoi(int2 c) {
	return uint(c.y)*width + uint(c.x);
}

#define MAX_CLEAN 1000000

Texture2D<float4> src_rgba : register(t0);
Texture2D<float> src_z : register(t1);

RWStructuredBuffer<pixel_nb> nbs : register(u0);
RWTexture2D<float4> dst_rgba : register(u1);
