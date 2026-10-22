/*
 * sweetmetal.h - C ABI of libsweetmetal_core (Rust, over metal2vulkan-runtime).
 *
 * SweetMetal.bundle's Objective-C classes (SweetMetalDevice, ...Buffer,
 * ...Texture, ...CommandQueue, ...) each own one of these handles and
 * forward Metal selectors to the functions below. Values use Metal's own
 * raw numbering (MTLPixelFormat, MTLPrimitiveType, MTLLoadAction,
 * MTLStoreAction, MTLTextureUsage, MTLResourceOptions), so the shim passes
 * enum values through unchanged.
 *
 * Conventions:
 *  - every object is an opaque pointer; release it with its *_release;
 *  - a NULL result / non-zero status means failure; smtl_last_error()
 *    returns the reason on the calling thread (valid until the next call);
 *  - encoders end with *_end, which also frees them.
 *
 * Licence: the library is LGPL-3.0-or-later (it links metal2vulkan); link
 * it dynamically.
 */
#ifndef SWEETMETAL_H
#define SWEETMETAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct smtl_device smtl_device;
typedef struct smtl_queue smtl_queue;
typedef struct smtl_cmdbuf smtl_cmdbuf;
typedef struct smtl_buffer smtl_buffer;
typedef struct smtl_texture smtl_texture;
typedef struct smtl_library smtl_library;
typedef struct smtl_function smtl_function;
typedef struct smtl_compute_pipeline smtl_compute_pipeline;
typedef struct smtl_render_pipeline smtl_render_pipeline;
typedef struct smtl_compute_encoder smtl_compute_encoder;
typedef struct smtl_render_encoder smtl_render_encoder;
typedef struct smtl_blit_encoder smtl_blit_encoder;
typedef struct smtl_depth_stencil_state smtl_depth_stencil_state;
typedef struct smtl_sampler_state smtl_sampler_state;

const char *smtl_last_error(void);

/* ---- device ---- */
/*
 * vulkan_library: path of a library exporting vkGetInstanceProcAddr
 * (on iOS: libsweetvk.dylib, a 10-line shim over turnip's
 * vk_icdGetInstanceProcAddr, because iOS has no Vulkan loader);
 * NULL = the platform's Vulkan loader. shader_cache_dir: where translated
 * shaders are kept (an app-container path on iOS); NULL = default.
 */
smtl_device *smtl_device_create(const char *vulkan_library, const char *shader_cache_dir);
void         smtl_device_release(smtl_device *d);
const char  *smtl_device_name(const smtl_device *d);       /* owned by d */
uint64_t     smtl_device_registry_id(const smtl_device *d);

/* ---- buffers (MTLResourceOptions raw value; 0 = shared) ---- */
smtl_buffer *smtl_device_new_buffer(smtl_device *d, uint64_t length, uint64_t options);
smtl_buffer *smtl_device_new_buffer_with_bytes(smtl_device *d, const void *bytes, uint64_t length,
    uint64_t options);
/* newBufferWithBytesNoCopy: the GPU uses the caller's pages at bytes, which
 * stay mapped while the buffer lives (also an IOSurface's memory) */
smtl_buffer *smtl_device_new_buffer_no_copy(smtl_device *d, void *bytes, uint64_t length,
    uint64_t options);
void        *smtl_buffer_contents(const smtl_buffer *b);  /* NULL for private storage */
uint64_t     smtl_buffer_length(const smtl_buffer *b);
uint64_t     smtl_buffer_gpu_address(const smtl_buffer *b);
void         smtl_buffer_release(smtl_buffer *b);

/* ---- textures ---- */
smtl_texture *smtl_device_new_texture_2d(smtl_device *d, uint32_t pixel_format, uint64_t width,
    uint64_t height, int mipmapped, uint32_t usage, uint64_t resource_options);
/* newTextureWithDescriptor:iosurface:plane: - `surface` is a buffer standing for the
 * IOSurface's memory (SweetAdreno IMPORT_SURFACE), the plane at offset/bytes_per_row. */
smtl_texture *smtl_buffer_new_iosurface_texture(smtl_buffer *surface, uint32_t pixel_format,
    uint64_t width, uint64_t height, uint32_t usage, uint64_t offset, uint64_t bytes_per_row,
    int planar);
/* a 2D texture over a buffer's bytes; linear: newLinearTextureWithDescriptor: */
smtl_texture *smtl_buffer_new_texture(smtl_buffer *b, uint32_t pixel_format, uint64_t width,
    uint64_t height, uint32_t usage, uint64_t offset, uint64_t bytes_per_row, int linear);
void          smtl_texture_release(smtl_texture *t);

/* MTLSamplerDescriptor, raw Metal values */
struct smtl_sampler_desc {
	uint32_t min_filter, mag_filter, mip_filter;
	uint32_t s_address, t_address, r_address;
	uint32_t border_color, normalized_coordinates;
	float    lod_min_clamp, lod_max_clamp;
	uint32_t max_anisotropy, compare_function;
};
smtl_sampler_state *smtl_device_new_sampler_state(smtl_device *d, const struct smtl_sampler_desc *s);
void smtl_sampler_state_release(smtl_sampler_state *s);
/* region: x, y, z, width, height, depth; len: bytes readable/writable at bytes */
int smtl_texture_replace_region(smtl_texture *t, const uint64_t region[6], uint64_t level,
    uint64_t slice, const void *bytes, uint64_t len, uint64_t bytes_per_row,
    uint64_t bytes_per_image);
int smtl_texture_get_bytes(smtl_texture *t, void *bytes, uint64_t len, uint64_t bytes_per_row,
    uint64_t bytes_per_image, const uint64_t region[6], uint64_t level, uint64_t slice);

/* ---- shaders and pipelines ---- */
smtl_library  *smtl_device_new_library_with_data(smtl_device *d, const void *metallib, size_t len);
smtl_library  *smtl_device_new_library_with_air(smtl_device *d, const void *bitcode, size_t len);
void           smtl_library_release(smtl_library *l);
smtl_function *smtl_library_new_function(smtl_library *l, const char *name);
void           smtl_function_release(smtl_function *f);
uint32_t       smtl_function_type(const smtl_function *f);      /* MTLFunctionType, 0 unknown */
/*
 * newFunctionWithName:constantValues:error: - lookup is asked for each
 * constant the function declares (name, index, MTLDataType) and writes its
 * packed value (len bytes) to out and returns 1, or returns 0 for none.
 */
typedef int (*smtl_constant_lookup)(void *ctx, const char *name, uint32_t index,
    uint32_t data_type, void *out, uint32_t len);
smtl_function *smtl_library_new_function_with_constants(smtl_library *l, const char *name,
    smtl_constant_lookup lookup, void *ctx);
/* the function names, NUL-separated into out (cut short where they do not
 * fit); returns the bytes the whole list needs */
size_t         smtl_library_function_names(const smtl_library *l, char *out, size_t out_len);

smtl_compute_pipeline *smtl_device_new_compute_pipeline(smtl_device *d, smtl_function *f);
uint64_t smtl_compute_pipeline_max_threads(const smtl_compute_pipeline *p);
uint64_t smtl_compute_pipeline_execution_width(const smtl_compute_pipeline *p);
void     smtl_compute_pipeline_release(smtl_compute_pipeline *p);

/* color_formats[i] = MTLPixelFormat of color attachment i (0 = none) */
smtl_render_pipeline *smtl_device_new_render_pipeline(smtl_device *d, smtl_function *vertex,
    smtl_function *fragment, const uint32_t *color_formats, uint32_t color_count);
/*
 * The full MTLRenderPipelineDescriptor, raw Metal enum values throughout.
 * An attachment with pixel_format 0 is absent; a vertex attribute with
 * format 0 is unset; a layout counts when its stride is non-zero or an
 * attribute names its buffer (stride UINT32_MAX = MTLBufferLayoutStrideDynamic).
 */
struct smtl_color_attachment_desc {
	uint32_t pixel_format;
	uint32_t blending_enabled;
	uint32_t source_rgb, destination_rgb, rgb_operation;        /* MTLBlendFactor/Operation */
	uint32_t source_alpha, destination_alpha, alpha_operation;
	uint32_t write_mask;                                         /* MTLColorWriteMask */
};
struct smtl_vertex_attribute_desc {
	uint32_t format;                /* MTLVertexFormat */
	uint32_t offset, buffer_index;
};
struct smtl_vertex_layout_desc {
	uint32_t stride, step_function, step_rate;   /* MTLVertexStepFunction */
};
struct smtl_render_pipeline_desc {
	smtl_function *vertex, *fragment;
	struct smtl_color_attachment_desc colors[8];
	uint32_t depth_format, stencil_format;      /* MTLPixelFormat, 0 = none */
	uint32_t sample_count;                      /* 0 = 1 */
	uint32_t has_vertex_descriptor;
	struct smtl_vertex_attribute_desc attributes[31];
	struct smtl_vertex_layout_desc layouts[31];
};
smtl_render_pipeline *smtl_device_new_render_pipeline_desc(smtl_device *d,
    const struct smtl_render_pipeline_desc *desc);
void smtl_render_pipeline_release(smtl_render_pipeline *p);

/* MTLDepthStencilDescriptor, raw Metal values */
struct smtl_stencil_desc {
	uint32_t compare;               /* MTLCompareFunction */
	uint32_t stencil_failure, depth_failure, depth_stencil_pass;   /* MTLStencilOperation */
	uint32_t read_mask, write_mask;
};
smtl_depth_stencil_state *smtl_device_new_depth_stencil_state(smtl_device *d,
    uint32_t depth_compare, int depth_write, const struct smtl_stencil_desc *front,
    const struct smtl_stencil_desc *back);
void smtl_depth_stencil_state_release(smtl_depth_stencil_state *s);

/* ---- queues and command buffers ---- */
smtl_queue  *smtl_device_new_command_queue(smtl_device *d);
void         smtl_queue_release(smtl_queue *q);
smtl_cmdbuf *smtl_queue_command_buffer(smtl_queue *q);
int          smtl_cmdbuf_commit(smtl_cmdbuf *c);
/* addCompletedHandler: (scheduled 0) / addScheduledHandler: (1): f(ctx) runs
 * once on one of the runtime's handler threads; before commit only */
int          smtl_cmdbuf_add_handler(smtl_cmdbuf *c, int scheduled, void (*f)(void *ctx), void *ctx);
/* 0 = completed, 1 = timed out, -1 = error (see smtl_last_error) */
int          smtl_cmdbuf_wait(smtl_cmdbuf *c, uint32_t timeout_ms);
/* MTLCommandBufferStatus raw value */
uint32_t     smtl_cmdbuf_status(const smtl_cmdbuf *c);
void         smtl_cmdbuf_release(smtl_cmdbuf *c);

/* ---- compute ---- */
smtl_compute_encoder *smtl_cmdbuf_compute_encoder(smtl_cmdbuf *c);
int smtl_compute_set_pipeline(smtl_compute_encoder *e, smtl_compute_pipeline *p);
int smtl_compute_set_buffer(smtl_compute_encoder *e, smtl_buffer *b, uint64_t offset, uint32_t index);
int smtl_compute_dispatch_threads(smtl_compute_encoder *e, uint64_t gx, uint64_t gy, uint64_t gz,
    uint64_t tx, uint64_t ty, uint64_t tz);
int smtl_compute_end(smtl_compute_encoder *e);

/* ---- render ---- */
struct smtl_color_attachment {
	smtl_texture *texture;
	uint32_t      load_action;      /* MTLLoadAction */
	uint32_t      store_action;     /* MTLStoreAction */
	double        clear[4];         /* RGBA */
};
smtl_render_encoder *smtl_cmdbuf_render_encoder(smtl_cmdbuf *c,
    const struct smtl_color_attachment *colors, uint32_t color_count);
int smtl_render_set_pipeline(smtl_render_encoder *e, smtl_render_pipeline *p);
int smtl_render_set_vertex_buffer(smtl_render_encoder *e, smtl_buffer *b, uint64_t offset, uint32_t index);
int smtl_render_set_fragment_buffer(smtl_render_encoder *e, smtl_buffer *b, uint64_t offset, uint32_t index);
int smtl_render_set_fragment_texture(smtl_render_encoder *e, smtl_texture *t, uint32_t index);
int smtl_render_draw_primitives(smtl_render_encoder *e, uint32_t primitive_type, uint64_t start,
    uint64_t count, uint64_t instances, uint64_t base_instance);
/* stage: 0 vertex, 1 fragment */
int smtl_render_set_buffer_offset(smtl_render_encoder *e, uint32_t stage, uint64_t offset, uint32_t index);
int smtl_render_set_bytes(smtl_render_encoder *e, uint32_t stage, const void *bytes, uint64_t len,
    uint32_t index);
int smtl_render_set_vertex_texture(smtl_render_encoder *e, smtl_texture *t, uint32_t index);
int smtl_render_set_sampler(smtl_render_encoder *e, uint32_t stage, smtl_sampler_state *s, uint32_t index);
int smtl_render_set_viewport(smtl_render_encoder *e, double x, double y, double w, double h,
    double znear, double zfar);
int smtl_render_set_scissor(smtl_render_encoder *e, uint64_t x, uint64_t y, uint64_t w, uint64_t h);
/* MTLCullMode, MTLWinding, MTLTriangleFillMode */
int smtl_render_set_raster(smtl_render_encoder *e, uint32_t cull, uint32_t winding, uint32_t fill);
int smtl_render_set_blend_color(smtl_render_encoder *e, float r, float g, float b, float a);
int smtl_render_draw_indexed(smtl_render_encoder *e, uint32_t primitive_type, uint32_t index_count,
    uint32_t index_type, smtl_buffer *index_buffer, uint64_t index_offset, uint32_t instances,
    int32_t base_vertex, uint32_t base_instance);
int smtl_render_set_depth_stencil_state(smtl_render_encoder *e, smtl_depth_stencil_state *s);
int smtl_render_set_stencil_reference(smtl_render_encoder *e, uint32_t front, uint32_t back);
int smtl_render_end(smtl_render_encoder *e);

/* ---- blit ---- */
smtl_blit_encoder *smtl_cmdbuf_blit_encoder(smtl_cmdbuf *c);
int smtl_blit_copy_buffer(smtl_blit_encoder *e, smtl_buffer *src, uint64_t src_offset,
    smtl_buffer *dst, uint64_t dst_offset, uint64_t size);
int smtl_blit_copy_texture_to_buffer(smtl_blit_encoder *e, smtl_texture *src, uint64_t slice,
    uint64_t level, uint64_t x, uint64_t y, uint64_t w, uint64_t h, smtl_buffer *dst,
    uint64_t dst_offset, uint64_t bytes_per_row, uint64_t bytes_per_image);
/* origins and size: x, y, z / width, height, depth */
int smtl_blit_copy_texture(smtl_blit_encoder *e, smtl_texture *src, uint64_t src_slice,
    uint64_t src_level, const uint64_t src_origin[3], const uint64_t size[3], smtl_texture *dst,
    uint64_t dst_slice, uint64_t dst_level, const uint64_t dst_origin[3]);
int smtl_blit_generate_mipmaps(smtl_blit_encoder *e, smtl_texture *t);
int smtl_blit_end(smtl_blit_encoder *e);

#ifdef __cplusplus
}
#endif
#endif
