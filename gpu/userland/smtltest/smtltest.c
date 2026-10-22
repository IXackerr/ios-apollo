/*
 * smtltest - libsweetmetal_core (Metal on Vulkan, metal2vulkan) on the
 * phone, without Metal.framework: the C ABI the SweetMetal bundle forwards
 * to, driven directly.
 *
 *   device and buffers; a compute kernel from test.metal checked element
 *   by element; a render pass that clears a texture and draws a triangle,
 *   read back and checked pixel by pixel; then every pipeline of
 *   QuartzCore's default.metallib that needs no vertex descriptor and no
 *   function constants, compiled for the GPU.
 *
 * The libraries come with the boot image (mkadt.py --run-file): core
 * (libsweetmetal_core), sweetvk, vk (turnip) and metallib (test.metal).
 * Started on the phone by ios/sweetrun.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "sweetmetal.h"
#include "sweetrun.h"

#define DIR             "/private/var/tmp/sweetmetal"
#define QC_METALLIB     "/System/Library/Frameworks/QuartzCore.framework/default.metallib"

#define MTLPixelFormatBGRA8Unorm        80
#define MTLTextureUsageShaderRead       1
#define MTLTextureUsageRenderTarget     4
#define MTLLoadActionClear              2
#define MTLStoreActionStore             1
#define MTLPrimitiveTypeTriangle        3

/* every entry point, resolved from the dylib */
#define FNS(X) \
	X(smtl_last_error) X(smtl_device_create) X(smtl_device_name) \
	X(smtl_device_new_buffer) X(smtl_buffer_contents) X(smtl_device_new_texture_2d) \
	X(smtl_device_new_library_with_data) X(smtl_library_new_function) \
	X(smtl_device_new_compute_pipeline) X(smtl_device_new_render_pipeline) \
	X(smtl_device_new_command_queue) X(smtl_queue_command_buffer) X(smtl_cmdbuf_commit) \
	X(smtl_cmdbuf_wait) X(smtl_cmdbuf_compute_encoder) X(smtl_compute_set_pipeline) \
	X(smtl_compute_set_buffer) X(smtl_compute_dispatch_threads) X(smtl_compute_end) \
	X(smtl_cmdbuf_render_encoder) X(smtl_render_set_pipeline) \
	X(smtl_render_set_fragment_buffer) X(smtl_render_draw_primitives) X(smtl_render_end) \
	X(smtl_cmdbuf_blit_encoder) X(smtl_blit_copy_texture_to_buffer) X(smtl_blit_end) \
	X(smtl_function_release) X(smtl_render_pipeline_release) \
	X(smtl_compute_pipeline_release)
#define DECL(f) static __typeof__(&f) p_##f;
FNS(DECL)

static int fails;
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf(" (%s)\n", p_smtl_last_error()); \
	fails++; } while (0)

static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

static void *
read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	void *p = malloc(*len);
	if (p != NULL && fread(p, 1, *len, f) != *len) {
		free(p);
		p = NULL;
	}
	fclose(f);
	return p;
}

static int
wait_cb(smtl_cmdbuf *c, const char *what)
{
	if (p_smtl_cmdbuf_commit(c) != 0 || p_smtl_cmdbuf_wait(c, 5000) != 0) {
		FAIL("%s did not complete", what);
		return -1;
	}
	return 0;
}

static void
test_compute(smtl_device *d, smtl_queue *q, smtl_library *lib)
{
	const uint32_t n = 1u << 16;
	smtl_function *f = p_smtl_library_new_function(lib, "square");
	double t0 = now_ms();
	smtl_compute_pipeline *p = f ? p_smtl_device_new_compute_pipeline(d, f) : NULL;
	if (p == NULL) {
		FAIL("compute pipeline");
		return;
	}
	double tc = now_ms() - t0;
	smtl_buffer *b = p_smtl_device_new_buffer(d, n * 4, 0);
	uint32_t *v = p_smtl_buffer_contents(b);
	for (uint32_t i = 0; i < n; i++) {
		v[i] = i;
	}
	smtl_cmdbuf *c = p_smtl_queue_command_buffer(q);
	smtl_compute_encoder *e = p_smtl_cmdbuf_compute_encoder(c);
	p_smtl_compute_set_pipeline(e, p);
	p_smtl_compute_set_buffer(e, b, 0, 0);
	p_smtl_compute_dispatch_threads(e, n, 1, 1, 64, 1, 1);
	p_smtl_compute_end(e);
	if (wait_cb(c, "compute") != 0) {
		return;
	}
	uint32_t bad = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (v[i] != i * i + (i ^ 0x9e3779b9u)) {
			bad++;
		}
	}
	printf("compute: pipeline %.1f ms, %u elements, %u wrong\n", tc, n, bad);
	if (bad) {
		fails++;
	}
}

static void
test_render(smtl_device *d, smtl_queue *q, smtl_library *lib)
{
	const uint32_t w = 256, h = 256;
	smtl_function *vs = p_smtl_library_new_function(lib, "tri_vertex");
	smtl_function *fs = p_smtl_library_new_function(lib, "tri_fragment");
	uint32_t fmt = MTLPixelFormatBGRA8Unorm;
	double t0 = now_ms();
	smtl_render_pipeline *p = vs && fs ? p_smtl_device_new_render_pipeline(d, vs, fs, &fmt, 1) : NULL;
	if (p == NULL) {
		FAIL("render pipeline");
		return;
	}
	double tc = now_ms() - t0;
	smtl_texture *t = p_smtl_device_new_texture_2d(d, fmt, w, h, 0,
	    MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, 0);
	smtl_buffer *out = p_smtl_device_new_buffer(d, w * h * 4, 0);
	smtl_buffer *color = p_smtl_device_new_buffer(d, 16, 0);
	float *rgba = p_smtl_buffer_contents(color);
	rgba[0] = 1.0f; rgba[1] = 0.5f; rgba[2] = 0.25f; rgba[3] = 1.0f;
	if (t == NULL || out == NULL) {
		FAIL("texture or buffer");
		return;
	}
	struct smtl_color_attachment ca = { t, MTLLoadActionClear, MTLStoreActionStore,
		{ 0.0, 0.0, 1.0, 1.0 } };
	smtl_cmdbuf *c = p_smtl_queue_command_buffer(q);
	smtl_render_encoder *e = p_smtl_cmdbuf_render_encoder(c, &ca, 1);
	p_smtl_render_set_pipeline(e, p);
	p_smtl_render_set_fragment_buffer(e, color, 0, 0);
	p_smtl_render_draw_primitives(e, MTLPrimitiveTypeTriangle, 0, 3, 1, 0);
	p_smtl_render_end(e);
	smtl_blit_encoder *be = p_smtl_cmdbuf_blit_encoder(c);
	p_smtl_blit_copy_texture_to_buffer(be, t, 0, 0, 0, 0, w, h, out, 0, w * 4, w * h * 4);
	p_smtl_blit_end(be);
	if (wait_cb(c, "render") != 0) {
		return;
	}
	/* BGRA8 in memory: B G R A */
	const uint32_t clear = 0xff0000ffu, tri = 0xffff8040u;
	uint32_t *px = p_smtl_buffer_contents(out);
	uint32_t ntri = 0, other = 0;
	for (uint32_t i = 0; i < w * h; i++) {
		if (px[i] == tri) {
			ntri++;
		} else if (px[i] != clear) {
			other++;
		}
	}
	double cover = (double)ntri / (w * h);
	printf("render: pipeline %.1f ms, triangle %.2f %%, center %#x corner %#x, %u other pixels\n",
	    tc, cover * 100, px[(h / 2) * w + w / 2], px[0], other);
	if (other || cover < 0.31 || cover > 0.33) {
		fails++;
	}
}

/* QuartzCore's own shaders: what compiles as is. */
static void
test_quartzcore(smtl_device *d)
{
	size_t len = 0;
	void *bytes = read_file(QC_METALLIB, &len);
	smtl_library *lib = bytes ? p_smtl_device_new_library_with_data(d, bytes, len) : NULL;
	if (lib == NULL) {
		FAIL("QuartzCore default.metallib (%zu bytes)", len);
		return;
	}
	printf("QuartzCore default.metallib: %zu bytes, loaded\n", len);
	static const char kernels[] = "compute_apl\0compute_apl_fast\0sum_apl_and_compute_dimming_factor\0";
	for (const char *k = kernels; *k; k += strlen(k) + 1) {
		smtl_function *f = p_smtl_library_new_function(lib, k);
		smtl_compute_pipeline *p = f ? p_smtl_device_new_compute_pipeline(d, f) : NULL;
		printf("  kernel %s: %s\n", k, p ? "OK" : p_smtl_last_error());
	}
	/* vertex/fragment pairs of the same effect */
	static const char pairs[] =
	    "upsample_vert_lpf\0upsample_frag_lpf\0"
	    "downsample_blur_vert_lpf\0downsample_blur_2_frag_lpf\0"
	    "asg_vert_lpf\0asg_frag_lpf\0"
	    "variable_blur_vert_lpf\0variable_blur_frag_lpf\0"
	    "std_vert0_lpf\0p3_clamp_lpf\0"
	    "read_surf_vert\0read_surf_frag\0";
	uint32_t fmt = MTLPixelFormatBGRA8Unorm;
	for (const char *v = pairs; *v; ) {
		const char *fr = v + strlen(v) + 1;
		smtl_function *vf = p_smtl_library_new_function(lib, v);
		smtl_function *ff = p_smtl_library_new_function(lib, fr);
		smtl_render_pipeline *p = vf && ff ? p_smtl_device_new_render_pipeline(d, vf, ff, &fmt, 1) : NULL;
		printf("  render %s + %s: %s\n", v, fr, p ? "OK" : p_smtl_last_error());
		v = fr + strlen(fr) + 1;
	}
}

int
main(void)
{
	mkdir(DIR, 0755);
	if (!sweetrun_file("vk", DIR "/libvulkan_freedreno.dylib") ||
	    !sweetrun_file("sweetvk", DIR "/libsweetvk.dylib") ||
	    !sweetrun_file("core", DIR "/libsweetmetal_core.dylib") ||
	    !sweetrun_file("metallib", DIR "/test.metallib")) {
		printf("FAIL: files missing from the boot image\nSMTLTEST FAILED\n");
		return 1;
	}
	void *core = dlopen(DIR "/libsweetmetal_core.dylib", RTLD_NOW | RTLD_LOCAL);
	if (core == NULL) {
		printf("FAIL: dlopen: %s\nSMTLTEST FAILED\n", dlerror());
		return 1;
	}
#define LOAD(f) if ((p_##f = (__typeof__(&f))dlsym(core, #f)) == NULL) { \
	printf("FAIL: no %s\nSMTLTEST FAILED\n", #f); return 1; }
	FNS(LOAD)

	double t0 = now_ms();
	smtl_device *d = p_smtl_device_create(DIR "/libsweetvk.dylib", DIR "/cache");
	if (d == NULL) {
		printf("FAIL: device: %s\nSMTLTEST FAILED\n", p_smtl_last_error());
		return 1;
	}
	printf("device: %s (%.0f ms)\n", p_smtl_device_name(d), now_ms() - t0);
	smtl_queue *q = p_smtl_device_new_command_queue(d);
	size_t len = 0;
	void *bytes = read_file(DIR "/test.metallib", &len);
	smtl_library *lib = bytes ? p_smtl_device_new_library_with_data(d, bytes, len) : NULL;
	if (q == NULL || lib == NULL) {
		FAIL("queue or test.metallib");
	} else {
		test_compute(d, q, lib);
		test_render(d, q, lib);
	}
	test_quartzcore(d);
	printf("%s\n", fails ? "SMTLTEST FAILED" : "SMTLTEST PASSED");
	return fails ? 1 : 0;
}
