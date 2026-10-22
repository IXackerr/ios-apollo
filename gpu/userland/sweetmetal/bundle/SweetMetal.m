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
#include <mach/mach_time.h>
#include <os/lock.h>
#include "sweetmetal.h"
#include "sweetgpu.h"

/* The iOS SDK ships IOKit.framework without headers. */
typedef mach_port_t io_object_t;
typedef io_object_t io_registry_entry_t;
extern io_registry_entry_t IORegistryEntryFromPath(mach_port_t mainPort, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key,
    CFAllocatorRef allocator, uint32_t options);
extern kern_return_t IOObjectRelease(io_object_t object);

/* Metal's private device base class: exported by Metal.framework, without
 * a header in the SDK. */
@interface _MTLDevice : NSObject
@end

/* os_log, and stderr too: the SWEET kernel prints a daemon's /dev/null
 * output (boot-arg sweetnull), which is where launchd sends stderr. */
/* MTLFunctionConstantValues has setters only; Metal's own internal class
 * answers a value per declared constant. */
@interface NSObject (SweetMetalConstants)
- (instancetype)initWithName:(NSString *)name type:(NSUInteger)type index:(NSUInteger)index
    required:(BOOL)required;
- (const void *)constantValueWithFunctionConstant:(id)constant;
@end

#define SMLOG(...) smlog([NSString stringWithFormat:__VA_ARGS__])

/* A message repeated back to back is printed once, then counted: a failing
 * call per frame would otherwise flood the console. */
static void
smlog(NSString *msg)
{
	static NSString *last;
	static unsigned repeats;
	@synchronized ([NSObject class]) {
		if ([msg isEqualToString:last]) {
			if (++repeats % 1000 == 0) {
				fprintf(stderr, "SweetMetal: (%u times) %s\n", repeats, msg.UTF8String);
			}
			return;
		}
		if (repeats > 0) {
			fprintf(stderr, "SweetMetal: (last message %u times more)\n", repeats);
		}
		last = [msg copy];
		repeats = 0;
	}
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
/* a call into the runtime that it refused: logged, as Metal's validation would stop it */
#define SMCHECK(call) do { \
	if ((call) != 0) { \
		SMError(@#call); \
	} \
} while (0)

/* ---------------------------------------------------------------- */

@interface SMBase : NSObject
@end

/* Live objects per class, to see what a client keeps (smstats). */
static NSMutableDictionary<NSString *, NSNumber *> *smLive;

static void
smcount(Class c, int delta)
{
	@synchronized ([SMBase class]) {
		if (smLive == nil) {
			smLive = [NSMutableDictionary new];
		}
		NSString *k = NSStringFromClass(c);
		smLive[k] = @(smLive[k].integerValue + delta);
	}
}

static unsigned smCompleted;
static sgpu_dev *smGpu;         /* the device's kernel client, for SURFACE_BUSY */
static uint64_t smLatencySum, smLatencyMax;    /* commit to completion, mach ticks */
static uint64_t smGpuSum, smGpuMax, smGpuCount; /* GPUEndTime - GPUStartTime, ns */

/*
 * Which command buffers may still run, for what outlives the textures they
 * used: each takes a sequence number when made and is done once it
 * completed (or was dropped uncommitted). A surface image retired at
 * sequence n is destroyed once every buffer up to n is done.
 */
static os_unfair_lock smSeqLock = OS_UNFAIR_LOCK_INIT;
static NSMutableIndexSet *smRunning;
static uint64_t smLastSeq;
static struct smGrave { smtl_surface_image *image; uint64_t seq; } *smGraves;
static unsigned smGraveCount, smGraveCap;

static uint64_t
smSeqTake(void)
{
	os_unfair_lock_lock(&smSeqLock);
	if (smRunning == nil) {
		smRunning = [NSMutableIndexSet new];
	}
	uint64_t seq = ++smLastSeq;
	[smRunning addIndex:(NSUInteger)seq];
	os_unfair_lock_unlock(&smSeqLock);
	return seq;
}

/* `seq` is done (0: none); destroys what no running buffer can use any more */
static void
smSeqDone(uint64_t seq)
{
	smtl_surface_image *dead[16];
	unsigned n = 0;
	do {
		n = 0;
		os_unfair_lock_lock(&smSeqLock);
		if (seq != 0) {
			[smRunning removeIndex:(NSUInteger)seq];
			seq = 0;
		}
		uint64_t first = smRunning.count ? smRunning.firstIndex : UINT64_MAX;
		for (unsigned i = 0; i < smGraveCount && n < 16; ) {
			if (smGraves[i].seq < first) {
				dead[n++] = smGraves[i].image;
				smGraves[i] = smGraves[--smGraveCount];
			} else {
				i++;
			}
		}
		os_unfair_lock_unlock(&smSeqLock);
		for (unsigned i = 0; i < n; i++) {
			smtl_surface_image_release(dead[i]);
		}
	} while (n == 16);
}

static void
smRetire(smtl_surface_image *image)
{
	os_unfair_lock_lock(&smSeqLock);
	if (smGraveCount == smGraveCap) {
		smGraveCap = smGraveCap ? smGraveCap * 2 : 16;
		smGraves = reallocf(smGraves, smGraveCap * sizeof(*smGraves));
	}
	smGraves[smGraveCount++] = (struct smGrave){ image, smLastSeq };
	os_unfair_lock_unlock(&smSeqLock);
	smSeqDone(0);
}

/* every 50 command buffers: how many completed, the rate, and how many of
 * each object are alive */
static void
smstats(void)
{
	static unsigned commits;
	static uint64_t last_ns;
	if (++commits % 50 != 0) {
		return;
	}
	uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	double per_s = last_ns ? 50e9 / (double)(now - last_ns) : 0;
	last_ns = now;
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	unsigned done = __atomic_load_n(&smCompleted, __ATOMIC_RELAXED);
	static unsigned done_before;
	uint64_t sum = __atomic_exchange_n(&smLatencySum, 0, __ATOMIC_RELAXED);
	uint64_t max = __atomic_exchange_n(&smLatencyMax, 0, __ATOMIC_RELAXED);
	double ms = (double)tb.numer / tb.denom / 1e6;
	uint64_t gsum = __atomic_exchange_n(&smGpuSum, 0, __ATOMIC_RELAXED);
	uint64_t gmax = __atomic_exchange_n(&smGpuMax, 0, __ATOMIC_RELAXED);
	uint64_t gn = __atomic_exchange_n(&smGpuCount, 0, __ATOMIC_RELAXED);
	NSMutableString *line = [NSMutableString stringWithFormat:
	    @"%u commits (%.1f/s), %u completed, latency %.1f ms (max %.1f), GPU %.1f ms "
	    "(max %.1f), live:", commits, per_s, done,
	    done > done_before ? sum * ms / (done - done_before) : 0, max * ms,
	    gn ? gsum / 1e6 / gn : 0, gmax / 1e6];
	done_before = done;
	@synchronized ([SMBase class]) {
		for (NSString *k in [smLive.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
			[line appendFormat:@" %@ %@", [k stringByReplacingOccurrencesOfString:@"SweetMetal"
			    withString:@""], smLive[k]];
		}
	}
	fprintf(stderr, "SweetMetal: %s\n", line.UTF8String);
}

@implementation SMBase
- (instancetype)init
{
	if ((self = [super init]) != nil) {
		smcount([self class], 1);
	}
	return self;
}
- (void)dealloc
{
	smcount([self class], -1);
}
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
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state { return MTLPurgeableStateNonVolatile; }
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
	id _image;                      /* SweetSurfaceImage the texture lies on */
	uint8_t *_cpu;                  /* with _image: the texels in this process */
	NSUInteger _cpuRow, _cpuTexel;  /* their bytes per row and per texel */
	struct smtl_texture_info _info; /* what the runtime made (smTextureInfo) */
	MTLStorageMode _storage;
	MTLTextureSwizzleChannels _swizzle;
}
@property (copy, atomic) NSString *label;
@end

@implementation SweetMetalTexture
- (id<MTLDevice>)device { return _device; }
- (MTLPixelFormat)pixelFormat { return _format; }
- (NSUInteger)width { return _width; }
- (NSUInteger)height { return _height; }
- (NSUInteger)depth { return _info.depth ? _info.depth : 1; }
- (NSUInteger)mipmapLevelCount { return _info.mipmap_level_count ? _info.mipmap_level_count : 1; }
- (NSUInteger)arrayLength { return _info.array_length ? _info.array_length : 1; }
- (NSUInteger)sampleCount { return _info.sample_count ? _info.sample_count : 1; }
- (MTLTextureType)textureType
{
	return _info.mipmap_level_count ? (MTLTextureType)_info.texture_type : MTLTextureType2D;
}
- (MTLTextureSwizzleChannels)swizzle
{
	if (_swizzle.red == 0 && _swizzle.green == 0 && _swizzle.blue == 0 && _swizzle.alpha == 0) {
		return MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
		    MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha);
	}
	return _swizzle;
}
- (BOOL)allowGPUOptimizedContents { return NO; }
- (MTLTextureUsage)usage { return _usage; }
- (IOSurfaceRef)iosurface { return _iosurface; }
- (NSUInteger)iosurfacePlane { return _plane; }
- (BOOL)isFramebufferOnly { return NO; }
- (MTLStorageMode)storageMode
{
	return _iosurface ? MTLStorageModeShared : _storage;
}
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state { return MTLPurgeableStateNonVolatile; }
- (uint64_t)protectionOptions { return 0; }
static uint64_t
regionBytes(MTLRegion r, NSUInteger bpr, NSUInteger bpi)
{
	return r.size.depth > 1 ? bpi * r.size.depth : bpr * r.size.height;
}
/*
 * The CPU calls of a texture over a surface image: the texels are the
 * surface's own bytes, which the GPU reads and writes in place, so the CPU
 * copies them there as Metal does for an IOSurface texture.
 */
- (BOOL)cpuCopy:(MTLRegion)r texels:(uint8_t *)texels bytesPerRow:(NSUInteger)bpr
    toTexture:(BOOL)to
{
	if (r.origin.x + r.size.width > _width || r.origin.y + r.size.height > _height ||
	    r.origin.z != 0 || r.size.depth > 1) {
		return NO;
	}
	size_t row = r.size.width * _cpuTexel;
	for (NSUInteger y = 0; y < r.size.height; y++) {
		uint8_t *surface = _cpu + (r.origin.y + y) * _cpuRow + r.origin.x * _cpuTexel;
		if (to) {
			memcpy(surface, texels + y * bpr, row);
		} else {
			memcpy(texels + y * bpr, surface, row);
		}
	}
	return YES;
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice
    withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi
{
	if (_image != nil) {
		if (level != 0 || slice != 0 || ![self cpuCopy:r texels:(uint8_t *)bytes
		    bytesPerRow:bpr toTexture:YES]) {
			SMError(@"replaceRegion: (surface)");
		}
		return;
	}
	const uint64_t reg[6] = { r.origin.x, r.origin.y, r.origin.z, r.size.width, r.size.height,
		r.size.depth };
	if (smtl_texture_replace_region(_t, reg, level, slice, bytes, regionBytes(r, bpr, bpi), bpr,
	    bpi) != 0) {
		SMError(@"replaceRegion:");
	}
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes
    bytesPerRow:(NSUInteger)bpr
{
	[self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:bpr
	    bytesPerImage:0];
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi
    fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice
{
	if (_image != nil) {
		if (level != 0 || slice != 0 || ![self cpuCopy:r texels:bytes bytesPerRow:bpr
		    toTexture:NO]) {
			SMError(@"getBytes: (surface)");
		}
		return;
	}
	const uint64_t reg[6] = { r.origin.x, r.origin.y, r.origin.z, r.size.width, r.size.height,
		r.size.depth };
	if (smtl_texture_get_bytes(_t, bytes, regionBytes(r, bpr, bpi), bpr, bpi, reg, level,
	    slice) != 0) {
		SMError(@"getBytes:");
	}
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)bpr fromRegion:(MTLRegion)r
    mipmapLevel:(NSUInteger)level
{
	[self getBytes:bytes bytesPerRow:bpr bytesPerImage:0 fromRegion:r mipmapLevel:level slice:0];
}
- (void)dealloc
{
	smtl_texture_release(_t);
	if (_iosurface) {
		CFRelease(_iosurface);
	}
}
@end

/* A new texture object over t, its properties as the runtime made them. */
static SweetMetalTexture *
smTexture(smtl_texture *t, id<MTLDevice> device, MTLTextureDescriptor *desc)
{
	SweetMetalTexture *tex = [SweetMetalTexture new];
	tex->_t = t;
	tex->_device = device;
	tex->_format = desc.pixelFormat;
	tex->_width = desc.width;
	tex->_height = desc.height;
	tex->_usage = desc.usage;
	tex->_storage = desc.storageMode;
	tex->_swizzle = desc.swizzle;
	smtl_texture_info(t, &tex->_info);
	return tex;
}

/* textures over a buffer: after SweetMetalTexture, whose ivars they set */
@implementation SweetMetalBuffer (Textures)
- (id<MTLTexture>)newTextureOver:(MTLTextureDescriptor *)desc offset:(NSUInteger)off
    bytesPerRow:(NSUInteger)bpr linear:(BOOL)linear
{
	smtl_texture *t = smtl_buffer_new_texture(_b, (uint32_t)desc.pixelFormat, desc.width,
	    desc.height, (uint32_t)desc.usage, off, bpr, linear);
	if (t == NULL) {
		SMError(@"newTextureWithDescriptor:offset:bytesPerRow:");
		return nil;
	}
	SweetMetalTexture *tex = [SweetMetalTexture new];
	tex->_t = t;
	tex->_device = _device;
	tex->_format = desc.pixelFormat;
	tex->_width = desc.width;
	tex->_height = desc.height;
	tex->_usage = desc.usage;
	tex->_backing = self;
	smtl_texture_info(tex->_t, &tex->_info);
	return tex;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)desc offset:(NSUInteger)off
    bytesPerRow:(NSUInteger)bpr
{
	return [self newTextureOver:desc offset:off bytesPerRow:bpr linear:NO];
}
- (id<MTLTexture>)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)desc
    offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi
{
	return [self newTextureOver:desc offset:off bytesPerRow:bpr linear:YES];
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
static int
lookupConstant(void *ctx, const char *name, uint32_t index, uint32_t type, void *out, uint32_t len)
{
	MTLFunctionConstantValues *values = (__bridge MTLFunctionConstantValues *)ctx;
	Class cls = objc_getClass("MTLFunctionConstantInternal");
	if (cls == Nil || ![values respondsToSelector:@selector(constantValueWithFunctionConstant:)]) {
		return 0;
	}
	id c = [[cls alloc] initWithName:@(name) type:type index:index required:NO];
	const void *v = [values constantValueWithFunctionConstant:c];
	if (v == NULL) {
		return 0;
	}
	memcpy(out, v, len);
	return 1;
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
    constantValues:(MTLFunctionConstantValues *)values error:(NSError **)error
{
	smtl_function *f = values == nil ? smtl_library_new_function(_l, name.UTF8String) :
	    smtl_library_new_function_with_constants(_l, name.UTF8String, lookupConstant,
	    (__bridge void *)values);
	if (f == NULL) {
		if (error) {
			*error = SMError([NSString stringWithFormat:@"function %@", name]);
		}
		return nil;
	}
	SweetMetalFunction *fn = [SweetMetalFunction new];
	fn->_f = f;
	fn->_name = name;
	fn->_device = _device;
	return fn;
}
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)desc error:(NSError **)error
{
	return [self newFunctionWithName:desc.name constantValues:desc.constantValues error:error];
}
- (void)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values
    completionHandler:(void (^)(id<MTLFunction>, NSError *))handler
{
	NSError *err = nil;
	id<MTLFunction> f = [self newFunctionWithName:name constantValues:values error:&err];
	handler(f, err);
}
- (void)newFunctionWithDescriptor:(MTLFunctionDescriptor *)desc
    completionHandler:(void (^)(id<MTLFunction>, NSError *))handler
{
	NSError *err = nil;
	id<MTLFunction> f = [self newFunctionWithDescriptor:desc error:&err];
	handler(f, err);
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

@interface SweetMetalSamplerState : SMBase <MTLSamplerState> {
@public
	smtl_sampler_state *_s;
	id<MTLDevice> _device;
	NSString *_label;
}
@end

@implementation SweetMetalSamplerState
- (id<MTLDevice>)device { return _device; }
- (NSString *)label { return _label; }
- (void)dealloc { smtl_sampler_state_release(_s); }
@end

@interface SweetMetalDepthStencilState : SMBase <MTLDepthStencilState> {
@public
	smtl_depth_stencil_state *_s;
	id<MTLDevice> _device;
	NSString *_label;
}
@end

@implementation SweetMetalDepthStencilState
- (id<MTLDevice>)device { return _device; }
- (NSString *)label { return _label; }
- (void)dealloc { smtl_depth_stencil_state_release(_s); }
@end

/* ---------------------------------------------------------------- */
/* Command submission                                               */
/* ---------------------------------------------------------------- */

@interface SweetMetalCommandBuffer : SMBase <MTLCommandBuffer> {
@public
	smtl_cmdbuf *_c;
	id<MTLCommandQueue> _queue;
	NSMutableArray<MTLCommandBufferHandler> *_scheduled;
	dispatch_semaphore_t _slot;     /* the queue's in-flight slot this buffer holds */
	uint64_t _seq;                  /* smSeqTake, until its completion takes it */
	uint32_t _surfaces[8];          /* IOSurfaces it renders into */
	unsigned _surfaceCount;
	uint32_t _reads[32];            /* IOSurfaces its commands read */
	unsigned _readCount;
}
@property (copy, atomic) NSString *label;
- (void)readsTexture:(id<MTLTexture>)t;
@end

@interface SweetMetalRenderEncoder : SMBase <MTLRenderCommandEncoder> {
@public
	smtl_render_encoder *_e;
	SweetMetalCommandBuffer *_cb;
	MTLCullMode _cull;
	MTLWinding _winding;
	MTLTriangleFillMode _fill;
}
@property (copy, atomic) NSString *label;
@end

@implementation SweetMetalRenderEncoder
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)s
{
	SMCHECK(smtl_render_set_pipeline(_e, ((SweetMetalRenderPipelineState *)s)->_p));
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i
{
	SMCHECK(smtl_render_set_vertex_buffer(_e, b ? ((SweetMetalBuffer *)b)->_b : NULL, o, (uint32_t)i));
}
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i
{
	SMCHECK(smtl_render_set_fragment_buffer(_e, b ? ((SweetMetalBuffer *)b)->_b : NULL, o, (uint32_t)i));
}
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i
{
	SMCHECK(smtl_render_set_buffer_offset(_e, 0, o, (uint32_t)i));
}
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i
{
	SMCHECK(smtl_render_set_buffer_offset(_e, 1, o, (uint32_t)i));
}
- (void)setVertexBytes:(const void *)b length:(NSUInteger)n atIndex:(NSUInteger)i
{
	SMCHECK(smtl_render_set_bytes(_e, 0, b, n, (uint32_t)i));
}
- (void)setFragmentBytes:(const void *)b length:(NSUInteger)n atIndex:(NSUInteger)i
{
	SMCHECK(smtl_render_set_bytes(_e, 1, b, n, (uint32_t)i));
}
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
	SMCHECK(smtl_render_set_vertex_texture(_e, t ? ((SweetMetalTexture *)t)->_t : NULL, (uint32_t)i));
	[_cb readsTexture:t];
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i
{
	SMCHECK(smtl_render_set_sampler(_e, 0, s ? ((SweetMetalSamplerState *)s)->_s : NULL, (uint32_t)i));
}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i
{
	SMCHECK(smtl_render_set_sampler(_e, 1, s ? ((SweetMetalSamplerState *)s)->_s : NULL, (uint32_t)i));
}
/* SPI QuartzCore uses: a texture and its sampler in one call */
- (void)setFragmentTexture:(id<MTLTexture>)t atTextureIndex:(NSUInteger)ti
    samplerState:(id<MTLSamplerState>)s atSamplerIndex:(NSUInteger)si
{
	[self setFragmentTexture:t atIndex:ti];
	[self setFragmentSamplerState:s atIndex:si];
}
- (void)setViewport:(MTLViewport)v
{
	SMCHECK(smtl_render_set_viewport(_e, v.originX, v.originY, v.width, v.height, v.znear, v.zfar));
}
- (void)setScissorRect:(MTLScissorRect)r
{
	SMCHECK(smtl_render_set_scissor(_e, r.x, r.y, r.width, r.height));
}
- (void)setCullMode:(MTLCullMode)m
{
	_cull = m;
	SMCHECK(smtl_render_set_raster(_e, (uint32_t)_cull, (uint32_t)_winding, (uint32_t)_fill));
}
- (void)setFrontFacingWinding:(MTLWinding)w
{
	_winding = w;
	SMCHECK(smtl_render_set_raster(_e, (uint32_t)_cull, (uint32_t)_winding, (uint32_t)_fill));
}
- (void)setTriangleFillMode:(MTLTriangleFillMode)f
{
	_fill = f;
	SMCHECK(smtl_render_set_raster(_e, (uint32_t)_cull, (uint32_t)_winding, (uint32_t)_fill));
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a
{
	SMCHECK(smtl_render_set_blend_color(_e, r, g, b, a));
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexCount:(NSUInteger)n
    indexType:(MTLIndexType)t indexBuffer:(id<MTLBuffer>)ib indexBufferOffset:(NSUInteger)o
    instanceCount:(NSUInteger)ic baseVertex:(NSInteger)bv baseInstance:(NSUInteger)bi
{
	if (smtl_render_draw_indexed(_e, (uint32_t)p, (uint32_t)n, (uint32_t)t,
	    ((SweetMetalBuffer *)ib)->_b, o, (uint32_t)ic, (int32_t)bv, (uint32_t)bi) != 0) {
		SMError(@"drawIndexedPrimitives:");
	}
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexCount:(NSUInteger)n
    indexType:(MTLIndexType)t indexBuffer:(id<MTLBuffer>)ib indexBufferOffset:(NSUInteger)o
    instanceCount:(NSUInteger)ic
{
	[self drawIndexedPrimitives:p indexCount:n indexType:t indexBuffer:ib indexBufferOffset:o
	    instanceCount:ic baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexCount:(NSUInteger)n
    indexType:(MTLIndexType)t indexBuffer:(id<MTLBuffer>)ib indexBufferOffset:(NSUInteger)o
{
	[self drawIndexedPrimitives:p indexCount:n indexType:t indexBuffer:ib indexBufferOffset:o
	    instanceCount:1 baseVertex:0 baseInstance:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n
    instanceCount:(NSUInteger)ic baseInstance:(NSUInteger)bi
{
	SMCHECK(smtl_render_draw_primitives(_e, (uint32_t)p, s, n, ic, bi));
}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)s
{
	SMCHECK(smtl_render_set_depth_stencil_state(_e, s ? ((SweetMetalDepthStencilState *)s)->_s : NULL));
}
- (void)setStencilReferenceValue:(uint32_t)v
{
	SMCHECK(smtl_render_set_stencil_reference(_e, v, v));
}
- (void)setStencilFrontReferenceValue:(uint32_t)front backReferenceValue:(uint32_t)back
{
	SMCHECK(smtl_render_set_stencil_reference(_e, front, back));
}
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
	SMCHECK(smtl_render_set_fragment_texture(_e, t ? ((SweetMetalTexture *)t)->_t : NULL, (uint32_t)i));
	[_cb readsTexture:t];
}
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n
{
	SMCHECK(smtl_render_draw_primitives(_e, (uint32_t)p, s, n, 1, 0));
}
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n
    instanceCount:(NSUInteger)ic
{
	SMCHECK(smtl_render_draw_primitives(_e, (uint32_t)p, s, n, ic, 0));
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
	SweetMetalCommandBuffer *_cb;
}
@property (copy, atomic) NSString *label;
@end

@implementation SweetMetalBlitEncoder
- (void)copyFromBuffer:(id<MTLBuffer>)src sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)dst
    destinationOffset:(NSUInteger)dof size:(NSUInteger)size
{
	SMCHECK(smtl_blit_copy_buffer(_e, ((SweetMetalBuffer *)src)->_b, so, ((SweetMetalBuffer *)dst)->_b,
	    dof, size));
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)slice
    sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size
    toBuffer:(id<MTLBuffer>)dst destinationOffset:(NSUInteger)off
    destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi
{
	SMCHECK(smtl_blit_copy_texture_to_buffer(_e, ((SweetMetalTexture *)src)->_t, slice, level, origin.x,
	    origin.y, size.width, size.height, ((SweetMetalBuffer *)dst)->_b, off, bpr, bpi));
	[_cb readsTexture:src];
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl
    sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dst
    destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dor
{
	const uint64_t o[3] = { so.x, so.y, so.z }, z[3] = { size.width, size.height, size.depth },
	    d[3] = { dor.x, dor.y, dor.z };
	if (smtl_blit_copy_texture(_e, ((SweetMetalTexture *)src)->_t, ss, sl, o, z,
	    ((SweetMetalTexture *)dst)->_t, ds, dl, d) != 0) {
		SMError(@"copyFromTexture:toTexture:");
	}
	[_cb readsTexture:src];
	[_cb readsTexture:dst];
}
- (void)generateMipmapsForTexture:(id<MTLTexture>)t
{
	if (smtl_blit_generate_mipmaps(_e, ((SweetMetalTexture *)t)->_t) != 0) {
		SMError(@"generateMipmapsForTexture:");
	}
}
- (void)endEncoding
{
	SMCHECK(smtl_blit_end(_e));
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
	SMCHECK(smtl_compute_set_pipeline(_e, ((SweetMetalComputePipelineState *)s)->_p));
}
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i
{
	SMCHECK(smtl_compute_set_buffer(_e, b ? ((SweetMetalBuffer *)b)->_b : NULL, o, (uint32_t)i));
}
- (void)dispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)tg
{
	SMCHECK(smtl_compute_dispatch_threads(_e, grid.width, grid.height, grid.depth, tg.width, tg.height,
	    tg.depth));
}
- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)tg
{
	SMCHECK(smtl_compute_dispatch_threads(_e, groups.width * tg.width, groups.height * tg.height,
	    groups.depth * tg.depth, tg.width, tg.height, tg.depth));
}
- (void)endEncoding
{
	if (smtl_compute_end(_e) != 0) {
		SMError(@"compute endEncoding");
	}
	_e = NULL;
}
@end

/* A block handed to the runtime's handler threads: retained until it ran. */
static void
runHandler(void *ctx)
{
	void (^h)(void) = (__bridge_transfer void (^)(void))ctx;
	h();
}

@implementation SweetMetalCommandBuffer
- (id<MTLCommandQueue>)commandQueue { return _queue; }
- (id<MTLDevice>)device { return _queue.device; }
- (MTLCommandBufferStatus)status { return (MTLCommandBufferStatus)smtl_cmdbuf_status(_c); }
/* an IOSurface its commands read: held in use until it completes */
- (void)readsTexture:(id<MTLTexture>)texture
{
	SweetMetalTexture *t = (SweetMetalTexture *)texture;
	if (t == nil || t->_iosurface == NULL) {
		return;
	}
	uint32_t sid = IOSurfaceGetID(t->_iosurface);
	for (unsigned k = 0; k < _readCount; k++) {
		if (_reads[k] == sid) {
			return;
		}
	}
	if (_readCount < 32) {
		_reads[_readCount++] = sid;
	}
}

static void
smAttachment(struct smtl_attachment *to, MTLRenderPassAttachmentDescriptor *a)
{
	to->texture = ((SweetMetalTexture *)a.texture)->_t;
	to->load_action = (uint32_t)a.loadAction;
	to->store_action = (uint32_t)a.storeAction;
	to->level = (uint32_t)a.level;
	to->slice = (uint32_t)a.slice;
	to->depth_plane = (uint32_t)a.depthPlane;
	if (a.resolveTexture != nil) {
		to->resolve_texture = ((SweetMetalTexture *)a.resolveTexture)->_t;
		to->resolve_level = (uint32_t)a.resolveLevel;
		to->resolve_slice = (uint32_t)a.resolveSlice;
		to->resolve_depth_plane = (uint32_t)a.resolveDepthPlane;
	}
}

- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d
{
	struct smtl_render_pass pass = {
		.render_target_array_length = (uint32_t)d.renderTargetArrayLength,
		.render_target_width = (uint32_t)d.renderTargetWidth,
		.render_target_height = (uint32_t)d.renderTargetHeight,
		.default_raster_sample_count = (uint32_t)d.defaultRasterSampleCount,
	};
	for (uint32_t i = 0; i < 8; i++) {
		MTLRenderPassColorAttachmentDescriptor *a = d.colorAttachments[i];
		if (a.texture == nil) {
			continue;
		}
		smAttachment(&pass.colors[i], a);
		pass.colors[i].clear[0] = a.clearColor.red;
		pass.colors[i].clear[1] = a.clearColor.green;
		pass.colors[i].clear[2] = a.clearColor.blue;
		pass.colors[i].clear[3] = a.clearColor.alpha;
		SweetMetalTexture *t = (SweetMetalTexture *)a.texture;
		if (t->_image != nil && t->_iosurface != NULL) {
			uint32_t sid = IOSurfaceGetID(t->_iosurface);
			unsigned k = 0;
			while (k < _surfaceCount && _surfaces[k] != sid) {
				k++;
			}
			if (k == _surfaceCount && k < 8) {
				_surfaces[_surfaceCount++] = sid;
			}
		}
	}
	if (d.depthAttachment.texture != nil) {
		smAttachment(&pass.depth, d.depthAttachment);
		pass.depth.clear[0] = d.depthAttachment.clearDepth;
	}
	if (d.stencilAttachment.texture != nil) {
		smAttachment(&pass.stencil, d.stencilAttachment);
		pass.stencil.clear[0] = d.stencilAttachment.clearStencil;
	}
	smtl_render_encoder *e = smtl_cmdbuf_render_pass_encoder(_c, &pass);
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
	enc->_cb = self;
	return enc;
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)h
{
	/* run by the runtime once the GPU is done: no thread waits per buffer.
	 * The block keeps the command buffer alive until then, as Metal does. */
	void (^call)(void) = ^{ h(self); };
	if (smtl_cmdbuf_add_handler(_c, 0, runHandler, (__bridge_retained void *)[call copy]) != 0) {
		SMError(@"addCompletedHandler:");
	}
}

/* What a buffer gives back once it completed: its queue slot and its
 * sequence number; and its latency, for smstats. */
struct smDone {
	void *slot;             /* retained dispatch_semaphore_t, or NULL */
	uint64_t seq;
	uint64_t committed;     /* mach_absolute_time */
	int given;              /* the slot and seq went back already */
	uint32_t surfaces[8];   /* IOSurfaces marked busy for the framebuffer */
	unsigned surfaceCount;
	uint32_t reads[32];     /* IOSurfaces held in use while it reads them */
	unsigned readCount;
	smtl_cmdbuf *cb;        /* for its GPU times */
};

static void
smGiveBack(struct smDone *d)
{
	if (__atomic_exchange_n(&d->given, 1, __ATOMIC_ACQ_REL)) {
		return;
	}
	if (d->slot != NULL) {
		dispatch_semaphore_signal((__bridge_transfer dispatch_semaphore_t)d->slot);
	}
	for (unsigned i = 0; i < d->surfaceCount; i++) {
		sgpu_surface_busy(smGpu, d->surfaces[i], -1);
	}
	for (unsigned i = 0; i < d->readCount; i++) {
		sgpu_surface_use(smGpu, d->reads[i], -1);
	}
	smSeqDone(d->seq);
}

static void
countCompleted(void *ctx)
{
	struct smDone *d = ctx;
	uint64_t latency = mach_absolute_time() - d->committed;
	__atomic_fetch_add(&smLatencySum, latency, __ATOMIC_RELAXED);
	uint64_t max = __atomic_load_n(&smLatencyMax, __ATOMIC_RELAXED);
	while (latency > max && !__atomic_compare_exchange_n(&smLatencyMax, &max, latency, false,
	    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
	double start, end;
	if (d->cb != NULL && smtl_cmdbuf_gpu_times(d->cb, &start, &end) == 0 && end >= start) {
		uint64_t gpu = (uint64_t)((end - start) * 1e9);
		__atomic_fetch_add(&smGpuSum, gpu, __ATOMIC_RELAXED);
		__atomic_fetch_add(&smGpuCount, 1, __ATOMIC_RELAXED);
		uint64_t gmax = __atomic_load_n(&smGpuMax, __ATOMIC_RELAXED);
		while (gpu > gmax && !__atomic_compare_exchange_n(&smGpuMax, &gmax, gpu, false,
		    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
		}
	}
	if (d->cb != NULL) {
		smtl_cmdbuf_release(d->cb);
	}
	__atomic_fetch_add(&smCompleted, 1, __ATOMIC_RELAXED);
	smGiveBack(d);
	free(d);
}

- (void)commit
{
	/* every buffer reports its completion: smstats, its queue slot, and
	 * what waits for it to be done (smSeqDone) */
	struct smDone *d = calloc(1, sizeof(*d));
	d->slot = _slot ? (__bridge_retained void *)_slot : NULL;
	d->seq = _seq;
	d->committed = mach_absolute_time();
	if (smtl_cmdbuf_add_handler(_c, 0, countCompleted, d) != 0) {
		/* nothing will call it: keep the slot and seq for -dealloc */
		if (d->slot != NULL) {
			CFRelease(d->slot);
		}
		free(d);
		d = NULL;
	} else {
		_slot = nil;
		_seq = 0;
		d->cb = smtl_cmdbuf_retain(_c);
		/* the framebuffer shows these surfaces only once this completes */
		for (unsigned i = 0; smGpu != NULL && i < _surfaceCount; i++) {
			if (sgpu_surface_busy(smGpu, _surfaces[i], 1) == 0) {
				d->surfaces[d->surfaceCount++] = _surfaces[i];
			}
		}
		/* and its clients do not draw into what it reads (IOSurfaceIsInUse) */
		for (unsigned i = 0; smGpu != NULL && i < _readCount; i++) {
			if (sgpu_surface_use(smGpu, _reads[i], 1) == 0) {
				d->reads[d->readCount++] = _reads[i];
			}
		}
	}
	if (smtl_cmdbuf_commit(_c) != 0) {
		/* a refused commit runs no handler (which keeps d, unused) */
		if (d != NULL) {
			smGiveBack(d);
		}
		SMError(@"commit");
		return;
	}
	for (MTLCommandBufferHandler h in _scheduled) {
		h(self);
	}
	_scheduled = nil;
	smstats();
}
- (void)enqueue { }
- (NSError *)error { return nil; }
- (void)commitAndWaitUntilSubmitted { [self commit]; }
- (void)waitUntilScheduled { }
- (void)addScheduledHandler:(MTLCommandBufferHandler)h
{
	/* scheduled at commit: call it then (and at once when already committed) */
	if (_scheduled == nil) {
		_scheduled = [NSMutableArray new];
	}
	[_scheduled addObject:[h copy]];
}
- (void)setProtectionOptions:(uint64_t)o { }
- (uint64_t)protectionOptions { return 0; }
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
- (void)dealloc
{
	/* never committed (or its handler not taken): give back here */
	if (_slot != nil) {
		dispatch_semaphore_signal(_slot);
	}
	smtl_cmdbuf_release(_c);
	if (_seq != 0) {
		smSeqDone(_seq);
	}
}
@end

@interface SweetMetalCommandQueue : SMBase <MTLCommandQueue> {
@public
	smtl_queue *_q;
	id<MTLDevice> _device;
	dispatch_semaphore_t _slots;
}
@property (copy, atomic) NSString *label;
@end

@implementation SweetMetalCommandQueue
- (id<MTLDevice>)device { return _device; }
/*
 * As Metal's queue (maxCommandBufferCount), a new command buffer waits
 * while too many of the queue's are not yet completed: the producer is
 * paced by the GPU, and what in-flight buffers hold (staging copies of
 * IOSurface textures among it) stays bounded.
 */
#define SM_IN_FLIGHT    4
#define SM_SLOT_WAIT_MS 1000

- (id<MTLCommandBuffer>)commandBuffer
{
	/* bounded, so that a client holding uncommitted buffers (or making
	 * one in a completed handler) is slowed down, not stopped */
	BOOL slot = dispatch_semaphore_wait(_slots,
	    dispatch_time(DISPATCH_TIME_NOW, SM_SLOT_WAIT_MS * NSEC_PER_MSEC)) == 0;
	if (!slot) {
		SMLOG(@"commandBuffer: no slot free after %d ms", SM_SLOT_WAIT_MS);
	}
	smtl_cmdbuf *c = smtl_queue_command_buffer(_q);
	if (c == NULL) {
		if (slot) {
			dispatch_semaphore_signal(_slots);
		}
		SMError(@"commandBuffer");
		return nil;
	}
	SweetMetalCommandBuffer *cb = [SweetMetalCommandBuffer new];
	cb->_c = c;
	cb->_queue = self;
	cb->_slot = slot ? _slots : nil;
	cb->_seq = smSeqTake();
	return cb;
}
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences { return [self commandBuffer]; }
/* scheduling hints of Apple's driver: one queue to one VkQueue here */
- (void)setSubmissionQueue:(dispatch_queue_t)q { }
- (void)setCompletionQueue:(dispatch_queue_t)q { }
- (void)setGPUPriority:(NSUInteger)p { }
- (void)setBackgroundGPUPriority:(NSUInteger)p { }
- (void)dealloc { smtl_queue_release(_q); }
@end

/* ---------------------------------------------------------------- */
/* The device                                                       */
/* ---------------------------------------------------------------- */

/*
 * A surface's pages as an image of the GPU (smtl_surface_image), shared by
 * the textures CoreAnimation makes over the surface. Retired, not
 * destroyed, when the last of them and the device's cache let go: command
 * buffers that used it may still run (smRetire).
 */
@interface SweetSurfaceImage : NSObject {
@public
	smtl_surface_image *_s;
}
@end

@implementation SweetSurfaceImage
- (void)dealloc
{
	smRetire(_s);
}
@end

@interface SweetMetalDevice : _MTLDevice <MTLDevice> {
@public
	smtl_device *_d;
	sgpu_dev *_gpu;
	mach_port_t _acceleratorPort;
	NSString *_name;
	/* IOSurface imports by surface: CoreAnimation makes textures over the
	 * same few surfaces again and again (docs in -newTextureWithDescriptor:
	 * iosurface:plane:) */
	NSMutableDictionary<NSString *, id> *_surfaces;  /* SweetMetalBuffer, SweetSurfaceImage */
	NSMutableArray<NSString *> *_surfaceOrder;
	BOOL _noSurfaceImages;          /* SWEETMETAL_COPY_SURFACES: the copying path only */
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
/*
 * mkadt.py --metal-processes: while the driver grows, only the processes
 * named get a device; the others see no Metal and render as before.
 */
/* mkadt.py --metal-env: K=V strings for metal2vulkan's diagnostics. */
static void
applyEnvironment(void)
{
	io_registry_entry_t node = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/sweet-metal");
	if (node == MACH_PORT_NULL) {
		return;
	}
	CFTypeRef v = IORegistryEntryCreateCFProperty(node, CFSTR("env"), NULL, 0);
	IOObjectRelease(node);
	if (v == NULL || CFGetTypeID(v) != CFDataGetTypeID()) {
		if (v != NULL) {
			CFRelease(v);
		}
		return;
	}
	NSData *d = CFBridgingRelease(v);
	const char *p = d.bytes, *end = p + d.length;
	for (; p < end; p += strlen(p) + 1) {
		const char *eq = strchr(p, '=');
		if (eq != NULL) {
			setenv([[NSString alloc] initWithBytes:p length:(NSUInteger)(eq - p)
			    encoding:NSUTF8StringEncoding].UTF8String, eq + 1, 1);
		}
	}
}

static BOOL
processAllowed(void)
{
	io_registry_entry_t node = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/sweet-metal");
	if (node == MACH_PORT_NULL) {
		return YES;
	}
	CFTypeRef v = IORegistryEntryCreateCFProperty(node, CFSTR("processes"), NULL, 0);
	IOObjectRelease(node);
	if (v == NULL || CFGetTypeID(v) != CFDataGetTypeID()) {
		if (v != NULL) {
			CFRelease(v);
		}
		return YES;
	}
	NSData *d = CFBridgingRelease(v);
	const char *p = d.bytes, *end = p + d.length, *me = getprogname();
	for (; p < end; p += strlen(p) + 1) {
		if (strcmp(p, me) == 0) {
			return YES;
		}
	}
	SMLOG(@"no device for %s (not in /sweet-metal processes)", me);
	return NO;
}

- (instancetype)initWithAcceleratorPort:(mach_port_t)port
{
	/* initialized either way: -[_MTLDevice dealloc] expects it */
	if ((self = [super init]) == nil || !processAllowed()) {
		return nil;
	}
	applyEnvironment();
	_acceleratorPort = port;
	NSString *bundle = [NSBundle bundleForClass:[self class]].bundlePath;
	NSString *vk = [bundle stringByAppendingPathComponent:@"libsweetvk.dylib"];
	/* per user: a cache root made can not be written by mobile processes */
	NSString *cache = [NSTemporaryDirectory() stringByAppendingPathComponent:
	    [NSString stringWithFormat:@"SweetMetalCache-%u", getuid()]];
	_d = smtl_device_create(vk.fileSystemRepresentation, cache.fileSystemRepresentation);
	if (_d == NULL) {
		SMError(@"device");
		return nil;
	}
	_name = [NSString stringWithUTF8String:smtl_device_name(_d)];
	sgpu_open(&_gpu);       /* for IOSurface import and SURFACE_BUSY */
	if (smGpu == NULL) {
		smGpu = _gpu;
	}
	_noSurfaceImages = getenv("SWEETMETAL_COPY_SURFACES") != NULL;
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
- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count { return count <= 1; }
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
	queue->_slots = dispatch_semaphore_create(SM_IN_FLIGHT);
	return queue;
}
- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)n
{
	return [self newCommandQueue];
}

static struct smtl_stencil_desc
stencilDesc(MTLStencilDescriptor *s)
{
	return (struct smtl_stencil_desc){
		(uint32_t)s.stencilCompareFunction, (uint32_t)s.stencilFailureOperation,
		(uint32_t)s.depthFailureOperation, (uint32_t)s.depthStencilPassOperation,
		s.readMask, s.writeMask,
	};
}

- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d
{
	struct smtl_sampler_desc sd = {
		(uint32_t)d.minFilter, (uint32_t)d.magFilter, (uint32_t)d.mipFilter,
		(uint32_t)d.sAddressMode, (uint32_t)d.tAddressMode, (uint32_t)d.rAddressMode,
		(uint32_t)d.borderColor, d.normalizedCoordinates, d.lodMinClamp, d.lodMaxClamp,
		(uint32_t)d.maxAnisotropy, (uint32_t)d.compareFunction,
	};
	smtl_sampler_state *st = smtl_device_new_sampler_state(_d, &sd);
	if (st == NULL) {
		SMError(@"newSamplerStateWithDescriptor:");
		return nil;
	}
	SweetMetalSamplerState *state = [SweetMetalSamplerState new];
	state->_s = st;
	state->_device = self;
	state->_label = d.label;
	return state;
}

- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d
{
	struct smtl_stencil_desc front = stencilDesc(d.frontFaceStencil);
	struct smtl_stencil_desc back = stencilDesc(d.backFaceStencil);
	smtl_depth_stencil_state *st = smtl_device_new_depth_stencil_state(_d,
	    (uint32_t)d.depthCompareFunction, d.depthWriteEnabled, &front, &back);
	if (st == NULL) {
		SMError(@"newDepthStencilStateWithDescriptor:");
		return nil;
	}
	SweetMetalDepthStencilState *state = [SweetMetalDepthStencilState new];
	state->_s = st;
	state->_device = self;
	state->_label = d.label;
	return state;
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
	smtl_buffer *nb = smtl_device_new_buffer_no_copy(_d, p, len, o);
	if (nb == NULL) {
		SMError([NSString stringWithFormat:@"newBufferWithBytesNoCopy: %p length %lu options %#lx",
		    p, (unsigned long)len, (unsigned long)o]);
		return nil;
	}
	SweetMetalBuffer *b = wrapBuffer(self, nb, o);
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
	MTLTextureSwizzleChannels sw = desc.swizzle;
	struct smtl_texture_desc d = {
		.texture_type = (uint32_t)desc.textureType,
		.pixel_format = (uint32_t)desc.pixelFormat,
		.width = desc.width,
		.height = desc.height,
		.depth = desc.depth,
		.mipmap_level_count = desc.mipmapLevelCount,
		.sample_count = desc.sampleCount,
		.array_length = desc.arrayLength,
		.usage = (uint32_t)desc.usage,
		.swizzle = { sw.red, sw.green, sw.blue, sw.alpha },
		.resource_options = desc.resourceOptions,
	};
	smtl_texture *t = smtl_device_new_texture(_d, &d);
	if (t == NULL) {
		SMError(@"newTextureWithDescriptor:");
		return nil;
	}
	return smTexture(t, self, desc);
}

/* The imports of the surfaces seen last, by key */
- (void)keepSurface:(id)import key:(NSString *)key
{
	@synchronized (self) {
		if (_surfaces == nil) {
			_surfaces = [NSMutableDictionary new];
			_surfaceOrder = [NSMutableArray new];
		}
		_surfaces[key] = import;
		[_surfaceOrder addObject:key];
		while (_surfaceOrder.count > 24) {
			[_surfaces removeObjectForKey:_surfaceOrder[0]];
			[_surfaceOrder removeObjectAtIndex:0];
		}
	}
}

/*
 * A texture over the surface's pages with no copy (smtl_surface_image): the
 * GPU renders into and samples the surface's bytes themselves. One image
 * per surface and format, which every texture over it shares, so that the
 * runtime orders their uses as those of one texture. nil where the format
 * or layout has none.
 */
- (id<MTLTexture>)newSurfaceTexture:(MTLTextureDescriptor *)desc surface:(IOSurfaceRef)surface
    base:(uint8_t *)base size:(size_t)size bytesPerRow:(size_t)bpr
{
	BOOL write = (desc.usage & MTLTextureUsageShaderWrite) != 0;
	NSString *key = [NSString stringWithFormat:@"%u:%p:%zu:%lu:%lux%lu:%d",
	    IOSurfaceGetID(surface), base, size, (unsigned long)desc.pixelFormat,
	    (unsigned long)desc.width, (unsigned long)desc.height, write];
	SweetSurfaceImage *image;
	@synchronized (self) {
		image = _surfaces[key];
	}
	if (image == nil) {
		smtl_surface_image *s = smtl_device_new_surface_image(_d, base, size, 0, bpr,
		    (uint32_t)desc.pixelFormat, desc.width, desc.height, (uint32_t)desc.usage);
		if (s == NULL) {
			SMLOG(@"iosurface %u (%lux%lu format %lu): copied, no image: %s",
			    IOSurfaceGetID(surface), (unsigned long)desc.width,
			    (unsigned long)desc.height, (unsigned long)desc.pixelFormat,
			    smtl_last_error());
			return nil;
		}
		image = [SweetSurfaceImage new];
		image->_s = s;
		[self keepSurface:image key:key];
	}
	smtl_texture *t = smtl_surface_image_texture(image->_s);
	if (t == NULL) {
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
	tex->_image = image;
	tex->_cpu = base;
	tex->_cpuRow = bpr;
	tex->_cpuTexel = IOSurfaceGetBytesPerElement(surface);
	smtl_texture_info(tex->_t, &tex->_info);
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
	/*
	 * Importing a surface wires and maps all its pages, which costs more
	 * than a frame for a framebuffer: keep the imports of the surfaces seen
	 * last (a texture keeps its own alive too) and reuse them while the
	 * surface is still mapped at the same place.
	 */
	if (planes == 0 && base != NULL && !_noSurfaceImages) {
		id<MTLTexture> tex = [self newSurfaceTexture:desc surface:surface base:base
		    size:size bytesPerRow:bpr];
		if (tex != nil) {
			return tex;
		}
	}
	NSString *key = [NSString stringWithFormat:@"%u:%p:%zu", IOSurfaceGetID(surface), base, size];
	SweetMetalBuffer *backing;
	@synchronized (self) {
		backing = _surfaces[key];
	}
	if (backing == nil) {
		smtl_buffer *b = base ? smtl_device_new_buffer_no_copy(_d, base, size, 0) : NULL;
		if (b == NULL) {
			SMError([NSString stringWithFormat:@"iosurface %u (%zux%zu, %zu bytes at %p)",
			    IOSurfaceGetID(surface), IOSurfaceGetWidth(surface),
			    IOSurfaceGetHeight(surface), size, base]);
			return nil;
		}
		backing = wrapBuffer(self, b, 0);
		[self keepSurface:backing key:key];
	}
	smtl_buffer *b = backing->_b;
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
	smtl_texture_info(tex->_t, &tex->_info);
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
