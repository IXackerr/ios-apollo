/*
 * smtl_test.c - drives libsweetmetal_core through its C ABI exactly as the
 * SweetMetal Objective-C classes will: device, AIR library, compute and
 * render pipelines, a compute dispatch, a render pass into a texture, a
 * render pass into an "IOSurface" texture (a texture over a buffer's
 * bytes, MTLTexture over IOSurface plane 0), blits, commit + wait.
 *
 * On Linux it runs on Mesa turnip (sweet backend) over the SWEET kernel
 * driver + SoC model (hosttest/libsgpu_host.so): every API call must
 * succeed and every command buffer must complete. The model has no shader
 * cores, so pixel/ALU results are not checked here; that is the on-device
 * test (gputest + this program on the phone).
 *
 *   smtl_test runtime_examples.bc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sweetmetal.h"

#define MTLPixelFormatRGBA8Unorm        70
#define MTLPixelFormatBGRA8Unorm        80
#define MTLTextureUsageShaderRead       0x1
#define MTLTextureUsageRenderTarget     0x4
#define MTLResourceStorageModeShared    (0u << 4)
#define MTLResourceStorageModePrivate   (2u << 4)
#define MTLLoadActionClear              2
#define MTLStoreActionStore             1
#define MTLPrimitiveTypeTriangle        3

static int fails;
#define OK(x) do { if (!(x)) { printf("FAIL %s: %s\n", #x, smtl_last_error()); fails++; goto out; } } while (0)
#define ZERO(x) do { if ((x) != 0) { printf("FAIL %s: %s\n", #x, smtl_last_error()); fails++; goto out; } } while (0)

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s runtime_examples.bc\n", argv[0]);
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 2;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *air = malloc((size_t)n);
	if (fread(air, 1, (size_t)n, f) != (size_t)n) {
		return 2;
	}
	fclose(f);

	smtl_device *dev = NULL;
	smtl_library *lib = NULL;
	smtl_queue *q = NULL;
	OK(dev = smtl_device_create(NULL, getenv("SMTL_CACHE")));
	printf("device: %s (registry id %#llx)\n", smtl_device_name(dev),
	    (unsigned long long)smtl_device_registry_id(dev));
	OK(lib = smtl_device_new_library_with_air(dev, air, (size_t)n));
	OK(q = smtl_device_new_command_queue(dev));

	/* ---- compute: scale_and_offset(input, output, scale, gid) ---- */
	{
		float in[1000];
		for (int i = 0; i < 1000; i++) in[i] = (float)i;
		float scale = 2.0f;
		smtl_function *fn;
		smtl_compute_pipeline *cp;
		smtl_buffer *bin, *bout, *bscale;
		OK(fn = smtl_library_new_function(lib, "scale_and_offset"));
		OK(cp = smtl_device_new_compute_pipeline(dev, fn));
		printf("compute pipeline: %llu threads/threadgroup, SIMD width %llu\n",
		    (unsigned long long)smtl_compute_pipeline_max_threads(cp),
		    (unsigned long long)smtl_compute_pipeline_execution_width(cp));
		OK(bin = smtl_device_new_buffer_with_bytes(dev, in, sizeof(in), MTLResourceStorageModeShared));
		OK(bout = smtl_device_new_buffer(dev, sizeof(in), MTLResourceStorageModeShared));
		OK(bscale = smtl_device_new_buffer_with_bytes(dev, &scale, 4, MTLResourceStorageModeShared));
		smtl_cmdbuf *cb;
		smtl_compute_encoder *ce;
		OK(cb = smtl_queue_command_buffer(q));
		OK(ce = smtl_cmdbuf_compute_encoder(cb));
		ZERO(smtl_compute_set_pipeline(ce, cp));
		ZERO(smtl_compute_set_buffer(ce, bin, 0, 0));
		ZERO(smtl_compute_set_buffer(ce, bout, 0, 1));
		ZERO(smtl_compute_set_buffer(ce, bscale, 0, 2));
		ZERO(smtl_compute_dispatch_threads(ce, 1000, 1, 1, 64, 1, 1));
		ZERO(smtl_compute_end(ce));
		ZERO(smtl_cmdbuf_commit(cb));
		ZERO(smtl_cmdbuf_wait(cb, 10000));
		float *out = smtl_buffer_contents(bout);
		printf("compute: completed (status %u), output[1] = %g (3 on hardware)\n",
		    smtl_cmdbuf_status(cb), out ? out[1] : -1.0);
		smtl_cmdbuf_release(cb);
		smtl_buffer_release(bin);
		smtl_buffer_release(bout);
		smtl_buffer_release(bscale);
		smtl_compute_pipeline_release(cp);
		smtl_function_release(fn);
	}

	/* ---- render: colored_vertex / colored_fragment ---- */
	{
		smtl_function *vf, *ff;
		smtl_render_pipeline *rp, *rp_bgra;
		OK(vf = smtl_library_new_function(lib, "colored_vertex"));
		OK(ff = smtl_library_new_function(lib, "colored_fragment"));
		uint32_t fmt = MTLPixelFormatRGBA8Unorm, fmt_bgra = MTLPixelFormatBGRA8Unorm;
		OK(rp = smtl_device_new_render_pipeline(dev, vf, ff, &fmt, 1));
		OK(rp_bgra = smtl_device_new_render_pipeline(dev, vf, ff, &fmt_bgra, 1));

		float pos[6][2] = { { -1, 1 }, { 0, 1 }, { -1, 0 }, { 0, 1 }, { 0, 0 }, { -1, 0 } };
		float col[6][4];
		for (int i = 0; i < 6; i++) { col[i][0] = 1; col[i][1] = 0.2f; col[i][2] = 0; col[i][3] = 1; }
		smtl_buffer *bpos, *bcol, *readback;
		OK(bpos = smtl_device_new_buffer_with_bytes(dev, pos, sizeof(pos), 0));
		OK(bcol = smtl_device_new_buffer_with_bytes(dev, col, sizeof(col), 0));
		OK(readback = smtl_device_new_buffer(dev, 64 * 64 * 4, 0));

		/* (a) a private render target, as CA's offscreen layers */
		smtl_texture *target;
		OK(target = smtl_device_new_texture_2d(dev, MTLPixelFormatRGBA8Unorm, 64, 64, 0,
		    MTLTextureUsageRenderTarget, MTLResourceStorageModePrivate));
		/* (b) an "IOSurface": 64x64 BGRA over a shared buffer standing for the surface's
		 * pages (on the phone: SweetAdreno IMPORT_SURFACE of a CA layer surface) */
		smtl_buffer *surface;
		smtl_texture *surf_tex;
		OK(surface = smtl_device_new_buffer(dev, 64 * 256, MTLResourceStorageModeShared));
		OK(surf_tex = smtl_buffer_new_iosurface_texture(surface, MTLPixelFormatBGRA8Unorm, 64, 64,
		    MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, 0, 256, 0));

		for (int pass = 0; pass < 2; pass++) {
			smtl_cmdbuf *cb;
			smtl_render_encoder *re;
			smtl_blit_encoder *be;
			struct smtl_color_attachment ca = {
				.texture = pass ? surf_tex : target,
				.load_action = MTLLoadActionClear,
				.store_action = MTLStoreActionStore,
				.clear = { 0, 0, 1, 1 },
			};
			OK(cb = smtl_queue_command_buffer(q));
			OK(re = smtl_cmdbuf_render_encoder(cb, &ca, 1));
			ZERO(smtl_render_set_pipeline(re, pass ? rp_bgra : rp));
			ZERO(smtl_render_set_vertex_buffer(re, bpos, 0, 0));
			ZERO(smtl_render_set_vertex_buffer(re, bcol, 0, 1));
			ZERO(smtl_render_draw_primitives(re, MTLPrimitiveTypeTriangle, 0, 6, 1, 0));
			ZERO(smtl_render_end(re));
			if (!pass) {
				OK(be = smtl_cmdbuf_blit_encoder(cb));
				ZERO(smtl_blit_copy_texture_to_buffer(be, target, 0, 0, 0, 0, 64, 64, readback, 0,
				    256, 256 * 64));
				ZERO(smtl_blit_end(be));
			}
			ZERO(smtl_cmdbuf_commit(cb));
			ZERO(smtl_cmdbuf_wait(cb, 10000));
			printf("render pass into %s: completed (status %u)\n",
			    pass ? "IOSurface-style texture over a buffer" : "private texture + blit",
			    smtl_cmdbuf_status(cb));
			smtl_cmdbuf_release(cb);
		}
		smtl_texture_release(surf_tex);
		smtl_buffer_release(surface);
		smtl_texture_release(target);
		smtl_buffer_release(readback);
		smtl_buffer_release(bpos);
		smtl_buffer_release(bcol);
		smtl_render_pipeline_release(rp);
		smtl_render_pipeline_release(rp_bgra);
		smtl_function_release(vf);
		smtl_function_release(ff);
	}

	/* ---- error path: an unknown function is a clean error, not a crash ---- */
	if (smtl_library_new_function(lib, "no_such_function") != NULL) {
		printf("FAIL: unknown function returned an object\n");
		fails++;
	} else {
		printf("unknown function refused: %.80s...\n", smtl_last_error());
	}
out:
	if (q) smtl_queue_release(q);
	if (lib) smtl_library_release(lib);
	if (dev) smtl_device_release(dev);
	printf("%s\n", fails ? "SMTL TEST FAILED" : "SMTL TEST PASSED (API path; pixels are verified on hardware)");
	return fails ? 1 : 0;
}
