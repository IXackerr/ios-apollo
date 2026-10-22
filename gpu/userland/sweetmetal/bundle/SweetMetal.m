/*
 * SweetMetal.bundle - the Metal driver plugin of the SWEET iOS port.
 *
 *   Metal.framework --(MetalPluginName/MetalPluginClassName on the
 *   SweetAdreno service, -initWithAcceleratorPort:)--> SweetMetalDevice
 *   --> libsweetmetal_core (C ABI over metal2vulkan-runtime)
 *   --> Vulkan (Mesa turnip, sweet backend) --> SweetAdreno (XNU)
 *
 * The Objective-C side is deliberately thin: each class holds one smtl_*
 * handle and forwards. Metal accepts the device only as a subclass of its
 * private _MTLDevice that also conforms to MTLDeviceSPI (docs/METAL.md
 * section 2.1); the resources, queues and encoders are plain NSObjects.
 *
 * Bring-up aid: every class answers unknown selectors with a log line
 * ("SweetMetal: -[SweetMetalDevice fooBar:] not implemented") and nil/0
 * instead of raising, so running backboardd with sweetmetal=1 produces the
 * exact list of methods still to implement.
 *
 * Licence: this file is ours; libsweetmetal_core is LGPL-3.0-or-later
 * (metal2vulkan) and is linked dynamically.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <objc/runtime.h>
#import <os/log.h>
#include "sweetmetal.h"
#include "sweetgpu.h"

/* Metal's private device base class: exported by Metal.framework, without
 * a header in the SDK. */
@interface _MTLDevice : NSObject
@end

/* os_log, and stderr too: the SWEET kernel prints a daemon's /dev/null
 * output (boot-arg sweetnull), which is where launchd sends stderr. */
#define SMLOG(...) smlog([NSString stringWithFormat:__VA_ARGS__])

static void
smlog(NSString *msg)
{
	os_log(OS_LOG_DEFAULT, "SweetMetal: %{public}@", msg);
	fprintf(stderr, "SweetMetal: %s\n", msg.UTF8String);
}

static NSError *
SMError(NSString *what)
{
	NSString *why = [NSString stringWithFormat:@"%@: %s", what, smtl_last_error()];
	SMLOG(@"%@", why);
	return [NSError errorWithDomain:@"SweetMetal" code:-1
	    userInfo:@{ NSLocalizedDescriptionKey: why }];
}

/* ---------------------------------------------------------------- */
/* Unknown-selector logging (shared by every class)                 */
/* ---------------------------------------------------------------- */

@interface SMBase : NSObject
@end

@implementation SMBase
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
	NSMethodSignature *sig = [super methodSignatureForSelector:sel];
	if (sig == nil) {
		/* answer as "returns id, takes nothing": the invocation is swallowed */
		sig = [NSMethodSignature signatureWithObjCTypes:"@@:"];
	}
	return sig;
}

- (void)forwardInvocation:(NSInvocation *)inv
{
	SMLOG(@"-[%s %s] not implemented", class_getName([self class]),
	    sel_getName(inv.selector));
	id nilResult = nil;
	if (inv.methodSignature.methodReturnLength == sizeof(id)) {
		[inv setReturnValue:&nilResult];
	}
}
@end

/* ---------------------------------------------------------------- */
/* Resources                                                        */
/* ---------------------------------------------------------------- */

@interface SweetMetalBuffer : SMBase <MTLBuffer> {
@public
	smtl_buffer *_b;
	void (^_deallocator)(void *, NSUInteger);
	void *_noCopyPointer;
	NSUInteger _noCopyLength;
	id<MTLDevice> _device;
	MTLResourceOptions _options;
}
@property (copy, atomic) NSString *label;
@end

@implementation SweetMetalBuffer
- (void *)contents { return smtl_buffer_contents(_b); }
- (NSUInteger)length { return (NSUInteger)smtl_buffer_length(_b); }
- (uint64_t)gpuAddress { return smtl_buffer_gpu_address(_b); }
- (id<MTLDevice>)device { return _device; }
- (MTLResourceOptions)resourceOptions { return _options; }
- (MTLStorageMode)storageMode { return (MTLStorageMode)((_options >> 4) & 0xf); }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)(_options & 0xf); }
- (void)didModifyRange:(NSRange)range { /* unified memory: nothing to do */ }
- (void)dealloc
{
	smtl_buffer_release(_b);
	if (_deallocator != nil) {
		_deallocator(_noCopyPointer, _noCopyLength);
	}
}
@end

@interface SweetMetalTexture : SMBase <MTLTexture> {
@public
	smtl_texture *_t;
	id<MTLDevice> _device;
	MTLPixelFormat _format;
	NSUInteger _width, _height;
	MTLTextureUsage _usage;
	IOSurfaceRef _iosurface;        /* retained, for -iosurface */
	NSUInteger _plane;
	SweetMetalBuffer *_backing;     /* buffer standing for the surface */
}
@property (copy, atomic) NSString *label;
@end

@implementation SweetMetalTexture
- (id<MTLDevice>)device { return _device; }
- (MTLPixelFormat)pixelFormat { return _format; }
- (NSUInteger)width { return _width; }
- (NSUInteger)height { return _height; }
- (NSUInteger)depth { return 1; }
- (NSUInteger)mipmapLevelCount { return 1; }
- (NSUInteger)arrayLength { return 1; }
- (NSUInteger)sampleCount { return 1; }
- (MTLTextureType)textureType { return MTLTextureType2D; }
- (MTLTextureUsage)usage { return _usage; }
- (IOSurfaceRef)iosurface { return _iosurface; }
- (NSUInteger)iosurfacePlane { return _plane; }
- (BOOL)isFramebufferOnly { return NO; }
- (void)dealloc
{
	smtl_texture_release(_t);
	if (_iosurface) {
		CFRelease(_iosurface);
	}
}
@end

/* ---------------------------------------------------------------- */
/* Shaders and pipelines                                            */
/* ---------------------------------------------------------------- */

@interface SweetMetalFunction : SMBase <MTLFunction> {
@public
	smtl_function *_f;
	id<MTLDevice> _device;
	NSString *_name;
}
@property (copy, atomic) NSString *label;
@end

@implementation SweetMetalFunction
- (NSString *)name { return _name; }
- (id<MTLDevice>)device { return _device; }
- (MTLFunctionType)functionType { return (MTLFunctionType)smtl_function_type(_f); }
- (void)dealloc { smtl_function_release(_f); }
@end

@interface SweetMetalLibrary : SMBase <MTLLibrary> {
@public
	smtl_library *_l;
	id<MTLDevice> _device;
}
@property (copy, atomic) NSString *label;
@end

@implementation SweetMetalLibrary
- (id<MTLDevice>)device { return _device; }
- (id<MTLFunction>)newFunctionWithName:(NSString *)name
{
	smtl_function *f = smtl_library_new_function(_l, name.UTF8String);
	if (f == NULL) {
		SMError([NSString stringWithFormat:@"newFunctionWithName:%@", name]);
		return nil;
	}
	SweetMetalFunction *fn = [SweetMetalFunction new];
	fn->_f = f;
	fn->_device = _device;
	fn->_name = [name copy];
	return fn;
}
- (NSArray<NSString *> *)functionNames
{
	size_t len = smtl_library_function_names(_l, NULL, 0);
	char *buf = malloc(len + 1);
	NSMutableArray *names = [NSMutableArray new];
	if (buf != NULL) {
		smtl_library_function_names(_l, buf, len);
		for (size_t i = 0; i < len; i += strlen(buf + i) + 1) {
			[names addObject:@(buf + i)];
		}
		free(buf);
	}
	return names;
}
- (void)dealloc { smtl_library_release(_l); }
@end

@interface SweetMetalComputePipelineState : SMBase <MTLComputePipelineState> {
@public
	smtl_compute_pipeline *_p;
	id<MTLDevice> _device;
}
@end

@implementation SweetMetalComputePipelineState
- (id<MTLDevice>)device { return _device; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return (NSUInteger)smtl_compute_pipeline_max_threads(_p); }
- (NSUInteger)threadExecutionWidth { return (NSUInteger)smtl_compute_pipeline_execution_width(_p); }
- (void)dealloc { smtl_compute_pipeline_release(_p); }
@end

@interface SweetMetalRenderPipelineState : SMBase <MTLRenderPipelineState> {
@public
	smtl_render_pipeline *_p;
	id<MTLDevice> _device;
}
@end

@implementation SweetMetalRenderPipelineState
- (id<MTLDevice>)device { return _device; }
- (void)dealloc { smtl_render_pipeline_release(_p); }
@end

/* ---------------------------------------------------------------- */
/* Command submission                                               */
/* ---------------------------------------------------------------- */

@interface SweetMetalCommandBuffer : SMBase <MTLCommandBuffer> {
@public
	smtl_cmdbuf *_c;
	id<MTLCommandQueue> _queue;
	NSMutableArray<MTLCommandBufferHandler> *_completed;
}
@property (copy, atomic) NSString *label;
@end

@interface SweetMetalRenderEncoder : SMBase <MTLRenderCommandEncoder> {
@public
	smtl_render_encoder *_e;
	SweetMetalCommandBuffer *_cb;
}
@property (copy, atomic) NSString *label;
@end

@implementation SweetMetalRenderEncoder
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)s
{
	smtl_render_set_pipeline(_e, ((SweetMetalRenderPipelineState *)s)->_p);
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i
{
	smtl_render_set_vertex_buffer(_e, b ? ((SweetMetalBuffer *)b)->_b : NULL, o, (uint32_t)i);
}
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i
{
	smtl_render_set_fragment_buffer(_e, b ? ((SweetMetalBuffer *)b)->_b : NULL, o, (uint32_t)i);
}
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
	smtl_render_set_fragment_texture(_e, t ? ((SweetMetalTexture *)t)->_t : NULL, (uint32_t)i);
}
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n
{
	smtl_render_draw_primitives(_e, (uint32_t)p, s, n, 1, 0);
}
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n
    instanceCount:(NSUInteger)ic
{
	smtl_render_draw_primitives(_e, (uint32_t)p, s, n, ic, 0);
}
- (void)endEncoding
{
	if (smtl_render_end(_e) != 0) {
		SMError(@"render endEncoding");
	}
	_e = NULL;
}
@end

@interface SweetMetalBlitEncoder : SMBase <MTLBlitCommandEncoder> {
@public
	smtl_blit_encoder *_e;
}
@property (copy, atomic) NSString *label;
@end

@implementation SweetMetalBlitEncoder
- (void)copyFromBuffer:(id<MTLBuffer>)src sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)dst
    destinationOffset:(NSUInteger)dof size:(NSUInteger)size
{
	smtl_blit_copy_buffer(_e, ((SweetMetalBuffer *)src)->_b, so, ((SweetMetalBuffer *)dst)->_b,
	    dof, size);
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)slice
    sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size
    toBuffer:(id<MTLBuffer>)dst destinationOffset:(NSUInteger)off
    destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi
{
	smtl_blit_copy_texture_to_buffer(_e, ((SweetMetalTexture *)src)->_t, slice, level, origin.x,
	    origin.y, size.width, size.height, ((SweetMetalBuffer *)dst)->_b, off, bpr, bpi);
}
- (void)endEncoding
{
	smtl_blit_end(_e);
	_e = NULL;
}
@end

@interface SweetMetalComputeEncoder : SMBase <MTLComputeCommandEncoder> {
@public
	smtl_compute_encoder *_e;
}
@property (copy, atomic) NSString *label;
@end

@implementation SweetMetalComputeEncoder
- (void)setComputePipelineState:(id<MTLComputePipelineState>)s
{
	smtl_compute_set_pipeline(_e, ((SweetMetalComputePipelineState *)s)->_p);
}
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i
{
	smtl_compute_set_buffer(_e, b ? ((SweetMetalBuffer *)b)->_b : NULL, o, (uint32_t)i);
}
- (void)dispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)tg
{
	smtl_compute_dispatch_threads(_e, grid.width, grid.height, grid.depth, tg.width, tg.height,
	    tg.depth);
}
- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)tg
{
	smtl_compute_dispatch_threads(_e, groups.width * tg.width, groups.height * tg.height,
	    groups.depth * tg.depth, tg.width, tg.height, tg.depth);
}
- (void)endEncoding
{
	if (smtl_compute_end(_e) != 0) {
		SMError(@"compute endEncoding");
	}
	_e = NULL;
}
@end

@implementation SweetMetalCommandBuffer
- (id<MTLCommandQueue>)commandQueue { return _queue; }
- (id<MTLDevice>)device { return _queue.device; }
- (MTLCommandBufferStatus)status { return (MTLCommandBufferStatus)smtl_cmdbuf_status(_c); }
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d
{
	struct smtl_color_attachment ca[8];
	uint32_t n = 0;
	for (uint32_t i = 0; i < 8; i++) {
		MTLRenderPassColorAttachmentDescriptor *a = d.colorAttachments[i];
		if (a.texture == nil) {
			continue;
		}
		n = i + 1;
		ca[i] = (struct smtl_color_attachment){
			.texture = ((SweetMetalTexture *)a.texture)->_t,
			.load_action = (uint32_t)a.loadAction,
			.store_action = (uint32_t)a.storeAction,
			.clear = { a.clearColor.red, a.clearColor.green, a.clearColor.blue, a.clearColor.alpha },
		};
	}
	smtl_render_encoder *e = smtl_cmdbuf_render_encoder(_c, ca, n);
	if (e == NULL) {
		SMError(@"renderCommandEncoderWithDescriptor:");
		return nil;
	}
	SweetMetalRenderEncoder *enc = [SweetMetalRenderEncoder new];
	enc->_e = e;
	enc->_cb = self;
	return enc;
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder
{
	smtl_compute_encoder *e = smtl_cmdbuf_compute_encoder(_c);
	if (e == NULL) {
		SMError(@"computeCommandEncoder");
		return nil;
	}
	SweetMetalComputeEncoder *enc = [SweetMetalComputeEncoder new];
	enc->_e = e;
	return enc;
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoder
{
	smtl_blit_encoder *e = smtl_cmdbuf_blit_encoder(_c);
	if (e == NULL) {
		SMError(@"blitCommandEncoder");
		return nil;
	}
	SweetMetalBlitEncoder *enc = [SweetMetalBlitEncoder new];
	enc->_e = e;
	return enc;
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)h
{
	if (_completed == nil) {
		_completed = [NSMutableArray new];
	}
	[_completed addObject:[h copy]];
}
- (void)commit
{
	if (smtl_cmdbuf_commit(_c) != 0) {
		SMError(@"commit");
		return;
	}
	if (_completed.count) {
		/* completion handlers run off the committing thread, as Metal's do */
		NSArray *handlers = [_completed copy];
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
			smtl_cmdbuf_wait(self->_c, 60000);
			for (MTLCommandBufferHandler h in handlers) {
				h(self);
			}
		});
	}
}
- (void)waitUntilCompleted
{
	smtl_cmdbuf_wait(_c, UINT32_MAX);
}
- (void)presentDrawable:(id<MTLDrawable>)drawable
{
	/* CAMetalLayer drawables are IOSurfaces; presenting is QuartzCore's job
	 * once the command buffer completes (docs/METAL.md, drawables) */
	[self addCompletedHandler:^(id<MTLCommandBuffer> cb) { [drawable present]; }];
}
- (void)dealloc { smtl_cmdbuf_release(_c); }
@end

@interface SweetMetalCommandQueue : SMBase <MTLCommandQueue> {
@public
	smtl_queue *_q;
	id<MTLDevice> _device;
}
@property (copy, atomic) NSString *label;
@end

@implementation SweetMetalCommandQueue
- (id<MTLDevice>)device { return _device; }
- (id<MTLCommandBuffer>)commandBuffer
{
	smtl_cmdbuf *c = smtl_queue_command_buffer(_q);
	if (c == NULL) {
		SMError(@"commandBuffer");
		return nil;
	}
	SweetMetalCommandBuffer *cb = [SweetMetalCommandBuffer new];
	cb->_c = c;
	cb->_queue = self;
	return cb;
}
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences { return [self commandBuffer]; }
- (void)dealloc { smtl_queue_release(_q); }
@end

/* ---------------------------------------------------------------- */
/* The device                                                       */
/* ---------------------------------------------------------------- */

@interface SweetMetalDevice : _MTLDevice <MTLDevice> {
@public
	smtl_device *_d;
	sgpu_dev *_gpu;
	mach_port_t _acceleratorPort;
	NSString *_name;
}
@end

@implementation SweetMetalDevice

/* MTLDeviceSPI has no declaration in the SDK; Metal checks the device
 * conforms to it before it registers the device. */
+ (void)load
{
	Protocol *spi = objc_getProtocol("MTLDeviceSPI");
	if (spi != NULL) {
		class_addProtocol(self, spi);
	}
}

/* Same unknown-selector logging as SMBase, for what _MTLDevice lacks. */
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
	NSMethodSignature *sig = [super methodSignatureForSelector:sel];
	return sig ?: [NSMethodSignature signatureWithObjCTypes:"@@:"];
}

- (void)forwardInvocation:(NSInvocation *)inv
{
	SMLOG(@"-[SweetMetalDevice %s] not implemented", sel_getName(inv.selector));
	id nilResult = nil;
	if (inv.methodSignature.methodReturnLength == sizeof(id)) {
		[inv setReturnValue:&nilResult];
	}
}

/*
 * Plug-in entry point: Metal.framework calls it on the MetalPluginClassName
 * class with the SweetAdreno service. -[_MTLDevice init] does no IOKit
 * work, unlike Apple's IOGPUMetalDevice, whose kernel ABI we do not have.
 */
- (instancetype)initWithAcceleratorPort:(mach_port_t)port
{
	if ((self = [super init]) == nil) {
		return nil;
	}
	_acceleratorPort = port;
	NSString *bundle = [NSBundle bundleForClass:[self class]].bundlePath;
	NSString *vk = [bundle stringByAppendingPathComponent:@"libsweetvk.dylib"];
	NSString *cache = [NSTemporaryDirectory() stringByAppendingPathComponent:@"SweetMetalCache"];
	_d = smtl_device_create(vk.fileSystemRepresentation, cache.fileSystemRepresentation);
	if (_d == NULL) {
		SMError(@"device");
		return nil;
	}
	_name = [NSString stringWithUTF8String:smtl_device_name(_d)];
	sgpu_open(&_gpu);       /* for IOSurface import (IMPORT_SURFACE) */
	SMLOG(@"device %@ ready in %s", _name, getprogname());
	return self;
}

- (instancetype)init { return [self initWithAcceleratorPort:MACH_PORT_NULL]; }

- (NSString *)name { return _name; }
- (uint64_t)registryID { return smtl_device_registry_id(_d); }
- (mach_port_t)acceleratorPort { return _acceleratorPort; }

/* MTLDeviceSPI: what Metal and QuartzCore ask beyond the public protocol */
- (uint32_t)featureProfile { return 5; }       /* the A11's */
- (NSString *)vendorName { return @"Qualcomm"; }
- (NSUInteger)minConstantBufferAlignmentBytes { return 64; }
- (NSUInteger)iosurfaceReadOnlyTextureAlignmentBytes { return 64; }
- (NSUInteger)deviceLinearReadOnlyTextureAlignmentBytes { return 64; }
- (NSUInteger)maxColorAttachments { return 8; }
- (NSUInteger)maxFragmentTextures { return 31; }
- (NSUInteger)maxFragmentSamplers { return 16; }
- (NSUInteger)maxTextureWidth2D { return 16384; }
- (NSUInteger)maxTextureHeight2D { return 16384; }
- (BOOL)supportsMemorylessRenderTargets { return NO; }
- (BOOL)supportsRasterOrderGroups { return NO; }
- (BOOL)supportsYCBCRFormats12 { return NO; }
- (BOOL)supportsExtendedXR10Formats { return NO; }
- (BOOL)supportsNativeHardwareFP16 { return YES; }
- (BOOL)supportsTileShaders { return NO; }
- (BOOL)supportsSIMDGroup { return NO; }
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error
{
	/* QuartzCore only logs this and compiles its pipelines at run time */
	if (error) {
		*error = [NSError errorWithDomain:@"SweetMetal" code:-1
		    userInfo:@{ NSLocalizedDescriptionKey: @"no precompiled pipeline libraries" }];
	}
	return nil;
}
- (BOOL)isLowPower { return NO; }
- (BOOL)isHeadless { return NO; }
- (BOOL)isRemovable { return NO; }
- (BOOL)hasUnifiedMemory { return YES; }
- (MTLSize)maxThreadsPerThreadgroup { return MTLSizeMake(1024, 1024, 1024); }
- (NSUInteger)maxBufferLength { return 1ull << 30; }
- (BOOL)supportsFamily:(MTLGPUFamily)family
{
	/* start honest and small; raise as features land (docs/METAL.md) */
	switch (family) {
	case MTLGPUFamilyApple1:
	case MTLGPUFamilyApple2:
	case MTLGPUFamilyApple3:
	case MTLGPUFamilyCommon1:
	case MTLGPUFamilyCommon2:
		return YES;
	default:
		return NO;
	}
}

- (id<MTLCommandQueue>)newCommandQueue
{
	smtl_queue *q = smtl_device_new_command_queue(_d);
	if (q == NULL) {
		SMError(@"newCommandQueue");
		return nil;
	}
	SweetMetalCommandQueue *queue = [SweetMetalCommandQueue new];
	queue->_q = q;
	queue->_device = self;
	return queue;
}
- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)n
{
	return [self newCommandQueue];
}

static SweetMetalBuffer *
wrapBuffer(SweetMetalDevice *d, smtl_buffer *b, MTLResourceOptions o)
{
	if (b == NULL) {
		SMError(@"newBuffer");
		return nil;
	}
	SweetMetalBuffer *buf = [SweetMetalBuffer new];
	buf->_b = b;
	buf->_device = d;
	buf->_options = o;
	return buf;
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)len options:(MTLResourceOptions)o
{
	return wrapBuffer(self, smtl_device_new_buffer(_d, len, o), o);
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)p length:(NSUInteger)len options:(MTLResourceOptions)o
{
	return wrapBuffer(self, smtl_device_new_buffer_with_bytes(_d, p, len, o), o);
}
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)p length:(NSUInteger)len
    options:(MTLResourceOptions)o deallocator:(void (^)(void *, NSUInteger))dealloc
{
	SweetMetalBuffer *b = wrapBuffer(self, smtl_device_new_buffer_no_copy(_d, p, len, o), o);
	/* the deallocator runs when our wrapper goes, after the GPU's last use of
	 * it (command buffers retain their resources) */
	if (b != nil && dealloc != nil) {
		b->_deallocator = [dealloc copy];
		b->_noCopyPointer = p;
		b->_noCopyLength = len;
	}
	return b;
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)desc
{
	smtl_texture *t = smtl_device_new_texture_2d(_d, (uint32_t)desc.pixelFormat, desc.width,
	    desc.height, desc.mipmapLevelCount > 1, (uint32_t)desc.usage, desc.resourceOptions);
	if (t == NULL) {
		SMError(@"newTextureWithDescriptor:");
		return nil;
	}
	SweetMetalTexture *tex = [SweetMetalTexture new];
	tex->_t = t;
	tex->_device = self;
	tex->_format = desc.pixelFormat;
	tex->_width = desc.width;
	tex->_height = desc.height;
	tex->_usage = desc.usage;
	return tex;
}

/*
 * CoreAnimation's path: render into the IOSurface of a layer. The GPU uses
 * the surface's own pages, mapped in this process: a no-copy buffer over
 * them (SweetAdreno IMPORT_USER through turnip's VK_EXT_external_memory_host)
 * stands for the surface, and the texture is plane `plane` of that buffer.
 */
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)desc
    iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane
{
	size_t planes = IOSurfaceGetPlaneCount(surface);
	size_t bpr = planes ? IOSurfaceGetBytesPerRowOfPlane(surface, plane) :
	    IOSurfaceGetBytesPerRow(surface);
	size_t page = (size_t)getpagesize();
	size_t size = (IOSurfaceGetAllocSize(surface) + page - 1) & ~(page - 1);
	IOSurfaceLock(surface, 0, NULL);
	uint8_t *base = IOSurfaceGetBaseAddress(surface);
	uint64_t offset = planes ? (uint64_t)((uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface, plane) -
	    base) : 0;
	IOSurfaceUnlock(surface, 0, NULL);
	smtl_buffer *b = base ? smtl_device_new_buffer_no_copy(_d, base, size, 0) : NULL;
	if (b == NULL) {
		SMError([NSString stringWithFormat:@"iosurface %u (%zux%zu, %zu bytes at %p)",
		    IOSurfaceGetID(surface), IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface),
		    size, base]);
		return nil;
	}
	SweetMetalBuffer *backing = wrapBuffer(self, b, 0);
	smtl_texture *t = smtl_buffer_new_iosurface_texture(b, (uint32_t)desc.pixelFormat,
	    desc.width, desc.height, (uint32_t)desc.usage, offset, bpr, planes > 0);
	if (t == NULL) {
		SMError(@"newTextureWithDescriptor:iosurface:plane:");
		return nil;
	}
	SweetMetalTexture *tex = [SweetMetalTexture new];
	tex->_t = t;
	tex->_device = self;
	tex->_format = desc.pixelFormat;
	tex->_width = desc.width;
	tex->_height = desc.height;
	tex->_usage = desc.usage;
	tex->_iosurface = (IOSurfaceRef)CFRetain(surface);
	tex->_plane = plane;
	tex->_backing = backing;
	return tex;
}

- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error
{
	const void *bytes = NULL;
	size_t len = 0;
	dispatch_data_t flat = dispatch_data_create_map(data, &bytes, &len);
	smtl_library *l = smtl_device_new_library_with_data(_d, bytes, len);
	(void)flat;
	if (l == NULL) {
		if (error) {
			*error = SMError(@"newLibraryWithData:");
		}
		return nil;
	}
	SweetMetalLibrary *lib = [SweetMetalLibrary new];
	lib->_l = l;
	lib->_device = self;
	return lib;
}

- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error
{
	NSData *d = [NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:error];
	if (d == nil) {
		return nil;
	}
	dispatch_data_t dd = dispatch_data_create(d.bytes, d.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	return [self newLibraryWithData:dd error:error];
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)fn
    error:(NSError **)error
{
	smtl_compute_pipeline *p = smtl_device_new_compute_pipeline(_d, ((SweetMetalFunction *)fn)->_f);
	if (p == NULL) {
		if (error) {
			*error = SMError(@"newComputePipelineStateWithFunction:");
		}
		return nil;
	}
	SweetMetalComputePipelineState *s = [SweetMetalComputePipelineState new];
	s->_p = p;
	s->_device = self;
	return s;
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
    error:(NSError **)error
{
	struct smtl_render_pipeline_desc rd = {
		.vertex = ((SweetMetalFunction *)d.vertexFunction)->_f,
		.fragment = d.fragmentFunction ? ((SweetMetalFunction *)d.fragmentFunction)->_f : NULL,
		.depth_format = (uint32_t)d.depthAttachmentPixelFormat,
		.stencil_format = (uint32_t)d.stencilAttachmentPixelFormat,
		.sample_count = (uint32_t)d.rasterSampleCount,
	};
	for (uint32_t i = 0; i < 8; i++) {
		MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
		rd.colors[i] = (struct smtl_color_attachment_desc){
			.pixel_format = (uint32_t)a.pixelFormat,
			.blending_enabled = a.blendingEnabled,
			.source_rgb = (uint32_t)a.sourceRGBBlendFactor,
			.destination_rgb = (uint32_t)a.destinationRGBBlendFactor,
			.rgb_operation = (uint32_t)a.rgbBlendOperation,
			.source_alpha = (uint32_t)a.sourceAlphaBlendFactor,
			.destination_alpha = (uint32_t)a.destinationAlphaBlendFactor,
			.alpha_operation = (uint32_t)a.alphaBlendOperation,
			.write_mask = (uint32_t)a.writeMask,
		};
	}
	MTLVertexDescriptor *vd = d.vertexDescriptor;
	if (vd != nil) {
		rd.has_vertex_descriptor = 1;
		for (uint32_t i = 0; i < 31; i++) {
			MTLVertexAttributeDescriptor *a = vd.attributes[i];
			rd.attributes[i] = (struct smtl_vertex_attribute_desc){
				(uint32_t)a.format, (uint32_t)a.offset, (uint32_t)a.bufferIndex };
			MTLVertexBufferLayoutDescriptor *l = vd.layouts[i];
			rd.layouts[i] = (struct smtl_vertex_layout_desc){
				l.stride == NSUIntegerMax ? UINT32_MAX : (uint32_t)l.stride,
				(uint32_t)l.stepFunction, (uint32_t)l.stepRate };
		}
	}
	smtl_render_pipeline *p = smtl_device_new_render_pipeline_desc(_d, &rd);
	if (p == NULL) {
		if (error) {
			*error = SMError(@"newRenderPipelineStateWithDescriptor:");
		}
		return nil;
	}
	SweetMetalRenderPipelineState *s = [SweetMetalRenderPipelineState new];
	s->_p = p;
	s->_device = self;
	return s;
}

- (void)dealloc
{
	smtl_device_release(_d);
	sgpu_close(_gpu);
}
@end
