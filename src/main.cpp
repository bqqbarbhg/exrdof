#define _CRT_SECURE_NO_WARNINGS
#define SOKOL_IMPL
#define SOKOL_D3D11
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_IMPLEMENTATION

#include <zlib.h>

#include "ext/imgui.h"
#include "ext/sokol_app.h"
#include "ext/sokol_gfx.h"
#include "ext/sokol_glue.h"
#include "ext/sokol_time.h"
#include "ext/sokol_imgui.h"
#include "ext/tinyexr.h"

#include <d3d11.h>
#include <stdio.h>
#include <assert.h>

#include <utility>
#include <vector>
#include <string>

#include <dof_nb_init.hlsl.h>
#include <dof_nb_spread.hlsl.h>
#include <dof_blur.hlsl.h>
#include <display_vs.hlsl.h>
#include <display_ps.hlsl.h>

extern "C" {
	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
}

template <typename T>
struct com_ptr
{
    T *ptr;

    com_ptr() : ptr(nullptr) { }
    explicit com_ptr(T *t) : ptr(t) { }
    com_ptr(const com_ptr &p) : ptr(p.ptr) { p.ptr->AddRef(); }
    com_ptr(com_ptr &&p) : ptr(p.ptr) { p.ptr = nullptr; }
    com_ptr &operator=(com_ptr p) { reset(p.ptr); p.reset(); return *this; }
    ~com_ptr() { if (ptr) ptr->Release(); }

    void reset(T *p=nullptr) {
        if (p) p->AddRef();
        if (ptr) ptr->Release();
        ptr = p;
    }

    T *operator->() const { return ptr; }
    T *operator*() const { return *ptr; }
};

struct alignas(16) cb_consts
{
	uint32_t width;
	uint32_t height;
	float clean_error;
	float spread_error;
	float max_blur;
	float focal_plane;
	int32_t kernel_sz;
	int32_t kernel_sz2;
	float kernel_reduce;
	float min_nonclean_penalty;
	float max_nonclean_penalty;
	int32_t target_level;
};

struct alignas(16) cb_display
{
	float exposure;
};

struct d3d11_imagestate
{
    com_ptr<ID3D11Texture2D> src_rgba;
    com_ptr<ID3D11Texture2D> src_z;
    com_ptr<ID3D11Texture2D> dst_rgba;
    com_ptr<ID3D11Buffer> nb_buffer;
    com_ptr<ID3D11Buffer> consts;

	com_ptr<ID3D11ShaderResourceView> src_rgba_srv;
	com_ptr<ID3D11ShaderResourceView> src_z_srv;
	com_ptr<ID3D11ShaderResourceView> dst_rgba_srv;

	com_ptr<ID3D11UnorderedAccessView> nb_buffer_uav;
	com_ptr<ID3D11UnorderedAccessView> dst_rgba_uav;

	sg_image src_rgba_image;
	sg_image dst_rgba_image;

	bool updated = true;

	uint32_t width;
	uint32_t height;
	uint32_t dispatch_x;
	uint32_t dispatch_y;
};

struct d3d11_state
{
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11ComputeShader> dof_nb_init;
    com_ptr<ID3D11ComputeShader> dof_nb_spread;
    com_ptr<ID3D11ComputeShader> dof_blur;

	sg_shader display_shader;
	sg_pipeline display_pipe;
	sg_buffer display_buffer;

    d3d11_imagestate image;
};

template <size_t N>
com_ptr<ID3D11ComputeShader> load_cs(d3d11_state &ds, const BYTE (&code)[N])
{
    com_ptr<ID3D11ComputeShader> shader;

    HRESULT hr = ds.device->CreateComputeShader(code, sizeof(code), NULL, &shader.ptr);
    assert(SUCCEEDED(hr));

    return shader;
}

d3d11_state g_d3d11_state;

void init_d3d11(d3d11_state &ds, ID3D11Device *device)
{
    ds.device.reset(device);
    ds.dof_nb_init = load_cs(ds, g_dof_nb_init);
    ds.dof_nb_spread = load_cs(ds, g_dof_nb_spread);
    ds.dof_blur = load_cs(ds, g_dof_blur);

	{
		sg_shader_desc d = { };
		d.attrs[0].name = "uv";
		d.attrs[0].sem_name = "TEXCOORD";
		d.attrs[0].sem_index = 0;
		d.vs.bytecode.ptr = g_display_vs;
		d.vs.bytecode.size = sizeof(g_display_vs);
		d.fs.bytecode.ptr = g_display_ps;
		d.fs.bytecode.size = sizeof(g_display_ps);
		d.fs.images[0].name = "dst_rgba";
		d.fs.images[0].image_type = SG_IMAGETYPE_2D;
		d.fs.images[0].sampler_type = SG_SAMPLERTYPE_FLOAT;
		d.fs.uniform_blocks[0].size = sizeof(cb_display);
		ds.display_shader = sg_make_shader(d);
	}

	{
		sg_pipeline_desc d = { };
		d.shader = ds.display_shader;
		d.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
		ds.display_pipe = sg_make_pipeline(d);
	}

	{
		float data[] = { 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f };

		sg_buffer_desc d = { };
		d.type = SG_BUFFERTYPE_VERTEXBUFFER;
		d.size = sizeof(data);
		d.usage = SG_USAGE_IMMUTABLE;
		d.data = SG_RANGE(data);
		ds.display_buffer = sg_make_buffer(d);
	}
}

com_ptr<ID3D11ShaderResourceView> make_srv(d3d11_state &ds, ID3D11Texture2D *texture)
{
	com_ptr<ID3D11ShaderResourceView> res;
	HRESULT hr;

	D3D11_TEXTURE2D_DESC td;
	texture->GetDesc(&td);

	D3D11_SHADER_RESOURCE_VIEW_DESC d = { };
	d.Format = td.Format;
	d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	d.Texture2D.MipLevels = 1;
	d.Texture2D.MostDetailedMip = 0;

	hr = ds.device->CreateShaderResourceView(texture, &d, &res.ptr);
	assert(SUCCEEDED(hr));

	return res;
}

void init_d3d11_image(d3d11_state &ds, size_t width, size_t height, const float **rgba, const float *z)
{
    HRESULT hr;

	sg_destroy_image(ds.image.src_rgba_image);
	sg_destroy_image(ds.image.dst_rgba_image);

    ds.image = { };

    {
        D3D11_TEXTURE2D_DESC d = { };
        d.Width = (UINT)width;
        d.Height = (UINT)height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.SampleDesc.Quality = 0;

        {
			d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			d.Usage = D3D11_USAGE_IMMUTABLE;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			std::vector<float> rgba_data;
			rgba_data.reserve(width * height * 4);
			for (size_t i = 0; i < width * height; i++) {
				rgba_data.push_back(rgba[0][i]);
				rgba_data.push_back(rgba[1][i]);
				rgba_data.push_back(rgba[2][i]);
				rgba_data.push_back(rgba[3][i]);
			}

			D3D11_SUBRESOURCE_DATA data;
			data.pSysMem = rgba_data.data();
			data.SysMemPitch = (UINT)width * sizeof(float) * 4;
			data.SysMemSlicePitch = (UINT)height * data.SysMemPitch;

			hr = ds.device->CreateTexture2D(&d, &data, &ds.image.src_rgba.ptr);
			assert(SUCCEEDED(hr));
        }

        {
			d.Format = DXGI_FORMAT_R32_FLOAT;
			d.Usage = D3D11_USAGE_IMMUTABLE;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			D3D11_SUBRESOURCE_DATA data;
			data.pSysMem = z;
			data.SysMemPitch = (UINT)width * sizeof(float);
			data.SysMemSlicePitch = (UINT)height * data.SysMemPitch;

			hr = ds.device->CreateTexture2D(&d, &data, &ds.image.src_z.ptr);
			assert(SUCCEEDED(hr));
        }

        {
			d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			d.Usage = D3D11_USAGE_DEFAULT;
			d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			D3D11_SUBRESOURCE_DATA data;
			data.pSysMem = z;
			data.SysMemPitch = (UINT)width * sizeof(float);
			data.SysMemSlicePitch = (UINT)height * data.SysMemPitch;

			hr = ds.device->CreateTexture2D(&d, NULL, &ds.image.dst_rgba.ptr);
			assert(SUCCEEDED(hr));
        }
    }

    {
        size_t nb_size = 5 * sizeof(float);
        D3D11_BUFFER_DESC d = { };
        d.ByteWidth = (UINT)(width * height * nb_size);
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        d.StructureByteStride = (UINT)nb_size;

		hr = ds.device->CreateBuffer(&d, NULL, &ds.image.nb_buffer.ptr);
		assert(SUCCEEDED(hr));
    }

    {
        size_t consts_size = sizeof(cb_consts);
        D3D11_BUFFER_DESC d = { };
        d.ByteWidth = (UINT)consts_size;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

		hr = ds.device->CreateBuffer(&d, NULL, &ds.image.consts.ptr);
		assert(SUCCEEDED(hr));
    }

	ds.image.src_rgba_srv = make_srv(ds, ds.image.src_rgba.ptr);
	ds.image.src_z_srv = make_srv(ds, ds.image.src_z.ptr);
	ds.image.dst_rgba_srv = make_srv(ds, ds.image.dst_rgba.ptr);

	{
		D3D11_UNORDERED_ACCESS_VIEW_DESC d = { };
		d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		d.Texture2D.MipSlice = 0;
		hr = ds.device->CreateUnorderedAccessView(ds.image.dst_rgba.ptr, &d, &ds.image.dst_rgba_uav.ptr);
		assert(SUCCEEDED(hr));
	}

	{
		D3D11_UNORDERED_ACCESS_VIEW_DESC d = { };
		d.Format = DXGI_FORMAT_UNKNOWN;
		d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		d.Buffer.FirstElement = 0;
		d.Buffer.NumElements = (UINT)(width * height);
		d.Buffer.Flags = 0;
		hr = ds.device->CreateUnorderedAccessView(ds.image.nb_buffer.ptr, &d, &ds.image.nb_buffer_uav.ptr);
		assert(SUCCEEDED(hr));
	}

	{
		sg_image_desc d = { };
		d.type = SG_IMAGETYPE_2D;
		d.width = (int)width;
		d.height = (int)height;
		d.d3d11_texture = ds.image.src_rgba.ptr;
		d.d3d11_shader_resource_view = ds.image.src_rgba_srv.ptr;
		d.mag_filter = SG_FILTER_LINEAR;
		d.min_filter = SG_FILTER_LINEAR;
		ds.image.src_rgba_image = sg_make_image(d);
	}

	{
		sg_image_desc d = { };
		d.type = SG_IMAGETYPE_2D;
		d.width = (int)width;
		d.height = (int)height;
		d.d3d11_texture = ds.image.dst_rgba.ptr;
		d.d3d11_shader_resource_view = ds.image.dst_rgba_srv.ptr;
		d.mag_filter = SG_FILTER_LINEAR;
		d.min_filter = SG_FILTER_LINEAR;
		ds.image.dst_rgba_image = sg_make_image(d);
	}

	ds.image.width = (uint32_t)width;
	ds.image.height = (uint32_t)height;
	ds.image.dispatch_x = (ds.image.width + 7) / 8;
	ds.image.dispatch_y = (ds.image.height + 7) / 8;
}

struct exr_image
{
	EXRVersion version;
	EXRHeader header;
	EXRImage image;
	bool has_version = false;
	bool has_header = false;
	bool has_image = false;

	exr_image() { }
	exr_image(const exr_image &rhs) = delete;
	exr_image& operator=(const exr_image &rhs) = delete;
	exr_image(exr_image &&rhs) : version(rhs.version), header(rhs.header), image(rhs.image)
		, has_version(rhs.has_version), has_header(rhs.has_header), has_image(rhs.has_image) {
		rhs.has_version = false;
		rhs.has_header = false;
		rhs.has_image = false;
	}
	exr_image& operator=(exr_image &&rhs) {
		if (has_header) FreeEXRHeader(&header);
		if (has_image) FreeEXRImage(&image);
		version = rhs.version;
		header = rhs.header;
		image = rhs.image;
		has_version = rhs.has_version;
		has_header = rhs.has_header;
		has_image = rhs.has_image;
		rhs.has_version = false;
		rhs.has_header = false;
		rhs.has_image = false;
	}
	~exr_image() {
		if (has_header) FreeEXRHeader(&header);
		if (has_image) FreeEXRImage(&image);
	}

	int get_channel(const char *name)
	{
		for (int i = 0; i < header.num_channels; i++) {
			if (!strcmp(header.channels[i].name, name)) {
				return i;
			}
		}
		return -1;
	}
};

struct exr_load_result
{
	exr_image image;
	std::string error;
};

std::string exr_error_to_string(const char *err)
{
	std::string error(err);
	FreeEXRErrorMessage(err);
	return error;
}

exr_load_result load_exr(const char *filename)
{
	exr_load_result res;
	auto &img = res.image;

	const char *err = NULL;
	int ret = 0;

	ret = ParseEXRVersionFromFile(&img.version, filename);

	if (ret == 0) {
		img.has_version = true;
		InitEXRHeader(&img.header);
		ret = ParseEXRHeaderFromFile(&img.header, &img.version, filename, &err);
		if (ret == 0) {
			img.has_header = true;

			for (int i = 0; i < img.header.num_channels; i++) {
				if (img.header.pixel_types[i] == TINYEXR_PIXELTYPE_HALF) {
					img.header.requested_pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT;
				}
			}

			InitEXRImage(&img.image);
			ret = LoadEXRImageFromFile(&img.image, &img.header, filename, &err);
			if (ret == 0) {
				img.has_image = true;
			} else {
				res.error = exr_error_to_string(err);
			}

		} else {
			res.error = exr_error_to_string(err);
		}
	} else {
		res.error = std::string("Failed to parse version: ") + std::to_string(ret);
	}

	return res;
}

void init(void)
{
	{
		sg_desc d = { };
		d.context = sapp_sgcontext();
		sg_setup(d);
	}

	{
		simgui_desc_t d = { };
		simgui_setup(d);
	}

	stm_setup();

    init_d3d11(g_d3d11_state, (ID3D11Device*)sapp_d3d11_get_device());

	exr_load_result res = load_exr("in.exr");
	assert(res.image.has_image);

	auto image = std::move(res.image);

	const float *rgba[4] = {
		(const float*)image.image.images[image.get_channel("R")],
		(const float*)image.image.images[image.get_channel("G")],
		(const float*)image.image.images[image.get_channel("B")],
		(const float*)image.image.images[image.get_channel("A")],
	};
	const float *z = (const float*)image.image.images[image.get_channel("Z")];

	init_d3d11_image(g_d3d11_state, image.image.width, image.image.height, rgba, z);

}

void on_event(const sapp_event *e)
{
	simgui_handle_event(e);
}

uint64_t g_last_frame;

void frame(void)
{
	static bool show_original = false;
	static float max_blur = 10.0f;
	static float focal_plane = 0.0f;

	double dt = stm_sec(stm_laptime(&g_last_frame));

	simgui_new_frame(sapp_width(), sapp_height(), dt);

	d3d11_state &ds = g_d3d11_state;

	ID3D11DeviceContext *dc = (ID3D11DeviceContext*)sapp_d3d11_get_device_context();

	bool changed = ds.image.updated;
	ds.image.updated = false;

	ImGui::Begin("DOF");

	ImGui::Checkbox("Show original", &show_original);
	changed |= ImGui::SliderFloat("Max Blur", &max_blur, 0.0f, 50.0f);
	changed |= ImGui::SliderFloat("Focal plane", &focal_plane, 0.0f, 1000.0f);

	ImGui::End();

	if (changed)
	{
		cb_consts ccs = { };

		ccs.width = ds.image.width;
		ccs.height = ds.image.height;
		ccs.clean_error = 0.1f;
		ccs.spread_error = 0.1f;
		ccs.max_blur = max_blur;
		ccs.focal_plane = focal_plane;
		ccs.kernel_sz = (uint32_t)ceilf(ccs.max_blur);
		ccs.kernel_sz2 = ccs.kernel_sz * ccs.kernel_sz;;
		ccs.min_nonclean_penalty = 20.0f;
		ccs.max_nonclean_penalty = 200.0f;
		ccs.target_level = 0;

		ID3D11ShaderResourceView *srvs[2] = {
			ds.image.src_rgba_srv.ptr,
			ds.image.src_z_srv.ptr,
		};
		dc->CSSetShaderResources(0, 2, srvs);

		ID3D11UnorderedAccessView *uavs[2] = {
			ds.image.nb_buffer_uav.ptr,
			ds.image.dst_rgba_uav.ptr,
		};
		dc->CSSetUnorderedAccessViews(0, 2, uavs, NULL);

		dc->CSSetShader(ds.dof_nb_init.ptr, NULL, 0);
		dc->UpdateSubresource(ds.image.consts.ptr, 0, NULL, &ccs, 0, 0);
		dc->CSSetConstantBuffers(0, 1, &ds.image.consts.ptr);
		dc->Dispatch(ds.image.dispatch_x, ds.image.dispatch_y, 1);

		ccs.target_level = 1;
		dc->CSSetShader(ds.dof_nb_spread.ptr, NULL, 0);
		dc->UpdateSubresource(ds.image.consts.ptr, 0, NULL, &ccs, 0, 0);
		dc->CSSetConstantBuffers(0, 1, &ds.image.consts.ptr);
		dc->Dispatch(ds.image.dispatch_x, ds.image.dispatch_y, 1);

		dc->CSSetShader(ds.dof_blur.ptr, NULL, 0);
		dc->Dispatch(ds.image.dispatch_x, ds.image.dispatch_y, 1);

		ID3D11ShaderResourceView *null_srv[2] = { };
		ID3D11UnorderedAccessView *null_uav[2] = { };
		dc->CSSetShaderResources(0, 2, null_srv);
		dc->CSSetUnorderedAccessViews(0, 2, null_uav, NULL);
	}

    sg_pass_action action = { };
    sg_begin_default_pass(&action, sapp_width(), sapp_height());

	sg_apply_pipeline(ds.display_pipe);

	sg_bindings binds = { };
	binds.fs_images[0] = show_original ? ds.image.src_rgba_image : ds.image.dst_rgba_image;
	binds.vertex_buffers[0] = ds.display_buffer;

	sg_apply_bindings(binds);

	cb_display disp;
	disp.exposure = 1.0f;
	sg_apply_uniforms(SG_SHADERSTAGE_FS, 0, SG_RANGE(disp));

	sg_draw(0, 3, 1);

	simgui_render();

    sg_end_pass();
    sg_commit();
}

void cleanup(void) {
    sg_shutdown();
}

sapp_desc sokol_main(int argc, char* argv[]) {
    sapp_desc d = { };
	d.init_cb = init;
	d.event_cb = on_event;
	d.frame_cb = frame;
	d.cleanup_cb = cleanup;
	d.width = 1280;
	d.height = 720;
	d.window_title = "EXR DOF";
    return d;
}

