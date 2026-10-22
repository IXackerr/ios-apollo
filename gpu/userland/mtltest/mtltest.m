/*
 * mtltest - the SweetMetal plug-in through Metal's own objects, before it
 * is installed: the loader bundle (what goes on the root) comes with the
 * boot image, is loaded from /private/var/tmp, and its device class is
 * instantiated the way Metal does it (-initWithAcceleratorPort: with the
 * IOAcceleratorES service); the loader unpacks the driver from
 * /sweet-metal as it does in backboardd.
 *
 *   the checks Metal makes before it registers a device; buffers; a
 *   compute kernel and a triangle from test.metal through
 *   MTLComputePipelineState / MTLRenderPipelineDescriptor /
 *   MTLRenderPassDescriptor, verified against the CPU; QuartzCore's
 *   default.metallib through -newLibraryWithURL:error:, as QuartzCore
 *   loads it; and a triangle drawn into an IOSurface, read back by the CPU
 *   from the surface's own memory, as CoreAnimation's output is.
 *
 * Files (mkadt.py --run-file): bundle (the loader), plist (its Info.plist),
 * metallib (test.metallib); the driver itself with --metal-file. Started
 * by ios/sweetrun.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <libkern/OSCacheControl.h>
#import <objc/runtime.h>
#include <sys/stat.h>
#include "sweetrun.h"

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);

#define B       "/private/var/tmp/sweetmetal-test/SweetMetal.bundle"

/* the plug-in entry point Metal calls (docs/METAL.md 2.1) */
@interface NSObject (SweetMetalPlugin)
- (instancetype)initWithAcceleratorPort:(mach_port_t)port;
@end

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); \
	fails++; } } while (0)

static void
test_compute(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib)
{
	const NSUInteger n = 1u << 16;
	NSError *err = nil;
	id<MTLFunction> f = [lib newFunctionWithName:@"square"];
	id<MTLComputePipelineState> p = f ? [dev newComputePipelineStateWithFunction:f error:&err] : nil;
	CHECK(p != nil, "compute pipeline: %s", err.localizedDescription.UTF8String);
	if (p == nil) {
		return;
	}
	id<MTLBuffer> b = [dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
	uint32_t *v = b.contents;
	for (uint32_t i = 0; i < n; i++) {
		v[i] = i;
	}
	id<MTLCommandBuffer> cb = [q commandBuffer];
	id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
	[e setComputePipelineState:p];
	[e setBuffer:b offset:0 atIndex:0];
	[e dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
	[e endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	uint32_t bad = 0;
	for (uint32_t i = 0; i < n; i++) {
		bad += v[i] != i * i + (i ^ 0x9e3779b9u);
	}
	printf("compute: %lu elements, %u wrong\n", (unsigned long)n, bad);
	CHECK(bad == 0, "compute results");
}

static void
test_render(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib)
{
	const NSUInteger w = 256, h = 256;
	NSError *err = nil;
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"tri_vertex"];
	pd.fragmentFunction = [lib newFunctionWithName:@"tri_fragment"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
	CHECK(p != nil, "render pipeline: %s", err.localizedDescription.UTF8String);
	if (p == nil) {
		return;
	}
	MTLTextureDescriptor *td = [MTLTextureDescriptor
	    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	id<MTLTexture> t = [dev newTextureWithDescriptor:td];
	id<MTLBuffer> out = [dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
	float color[4] = { 1.0f, 0.5f, 0.25f, 1.0f };
	id<MTLBuffer> cbuf = [dev newBufferWithBytes:color length:sizeof(color)
	    options:MTLResourceStorageModeShared];

	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:p];
	[re setFragmentBuffer:cbuf offset:0 atIndex:0];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[re endEncoding];
	id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
	[be copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
	    sourceSize:MTLSizeMake(w, h, 1) toBuffer:out destinationOffset:0
	    destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
	[be endEncoding];
	[cb commit];
	[cb waitUntilCompleted];

	const uint32_t *px = out.contents, clear = 0xff0000ffu, tri = 0xffff8040u;
	uint32_t ntri = 0, other = 0;
	for (NSUInteger i = 0; i < w * h; i++) {
		if (px[i] == tri) {
			ntri++;
		} else if (px[i] != clear) {
			other++;
		}
	}
	double cover = (double)ntri / (w * h);
	printf("render: triangle %.2f %%, center %#x corner %#x, %u other pixels\n", cover * 100,
	    px[(h / 2) * w + w / 2], px[0], other);
	CHECK(other == 0 && cover > 0.31 && cover < 0.33, "render pixels");
}

/* CoreAnimation's path: draw into an IOSurface, then the CPU (in CA's case
 * the display) reads the surface's memory. */
static void
test_iosurface(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib)
{
	const int w = 256, h = 256;
	NSDictionary *props = @{
		(id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h),
		(id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfacePixelFormat: @('BGRA'),
	};
	IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef)props);
	CHECK(surf != NULL, "IOSurfaceCreate");
	if (surf == NULL) {
		return;
	}
	MTLTextureDescriptor *td = [MTLTextureDescriptor
	    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	id<MTLTexture> t = [dev newTextureWithDescriptor:td iosurface:surf plane:0];
	CHECK(t != nil, "texture over IOSurface %u", IOSurfaceGetID(surf));
	NSError *err = nil;
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"tri_vertex"];
	pd.fragmentFunction = [lib newFunctionWithName:@"tri_fragment"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
	if (t == nil || p == nil) {
		CFRelease(surf);
		return;
	}
	float color[4] = { 0.25f, 1.0f, 0.5f, 1.0f };
	id<MTLBuffer> cbuf = [dev newBufferWithBytes:color length:sizeof(color)
	    options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:p];
	[re setFragmentBuffer:cbuf offset:0 atIndex:0];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[re endEncoding];
	[cb commit];
	[cb waitUntilCompleted];

	IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
	const uint8_t *base = IOSurfaceGetBaseAddress(surf);
	size_t bpr = IOSurfaceGetBytesPerRow(surf);
	/* the display reads memory, not this CPU's cache: drop the lines the
	 * CPU may hold over the surface before reading it as the display would */
	sys_dcache_flush((void *)base, IOSurfaceGetAllocSize(surf));
	/* BGRA8 in memory: clear red = 00 00 ff ff, triangle = 80 ff 40 ff */
	const uint32_t clear = 0xffff0000u, tri = 0xff40ff80u;
	uint32_t ntri = 0, other = 0, center = 0, corner = 0;
	for (int y = 0; y < h; y++) {
		const uint32_t *row = (const uint32_t *)(base + y * bpr);
		for (int x = 0; x < w; x++) {
			if (row[x] == tri) {
				ntri++;
			} else if (row[x] != clear) {
				if (other++ < 4) {
					printf("  pixel %d,%d: %#x\n", x, y, row[x]);
				}
			}
		}
	}
	center = ((const uint32_t *)(base + (h / 2) * bpr))[w / 2];
	corner = ((const uint32_t *)base)[0];
	IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
	double cover = (double)ntri / (w * h);
	printf("iosurface %u (bpr %zu): triangle %.2f %%, center %#x corner %#x, %u other pixels\n",
	    IOSurfaceGetID(surf), bpr, cover * 100, center, corner, other);
	CHECK(other == 0 && cover > 0.31 && cover < 0.33, "IOSurface pixels");
	CFRelease(surf);
}

int
main(void)
{
	@autoreleasepool {
		mkdir("/private/var/tmp/sweetmetal-test", 0755);
		mkdir(B, 0755);
		/* the loader's directory, as the root install makes it */
		mkdir("/private/var/sweetmetal", 01777);
		chmod("/private/var/sweetmetal", 01777);
		if (!sweetrun_file("bundle", B "/SweetMetal") || !sweetrun_file("plist", B "/Info.plist") ||
		    !sweetrun_file("metallib", "/private/var/tmp/sweetmetal-test/test.metallib")) {
			printf("FAIL: files missing from the boot image\nMTLTEST FAILED\n");
			return 1;
		}
		NSBundle *bundle = [NSBundle bundleWithPath:@B];
		NSError *err = nil;
		if (![bundle loadAndReturnError:&err]) {
			printf("FAIL: bundle: %s\nMTLTEST FAILED\n", err.description.UTF8String);
			return 1;
		}
		/* what Metal's plug-in loader does (docs/METAL.md 2.1) */
		Class cls = [bundle classNamed:@"SweetMetalDevice"] ?: bundle.principalClass;
		Class base = objc_getClass("_MTLDevice");
		printf("class %s, subclass of _MTLDevice: %d\n", cls ? class_getName(cls) : "(none)",
		    cls && base && [cls isSubclassOfClass:base]);
		CHECK(cls && base && [cls isSubclassOfClass:base], "plug-in class");
		io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL,
		    IOServiceMatching("IOAcceleratorES"));
		id<MTLDevice> dev = cls ? [[cls alloc] initWithAcceleratorPort:svc] : nil;
		Protocol *spi = objc_getProtocol("MTLDeviceSPI");
		printf("device %s, MTLDevice %d, MTLDeviceSPI %d, service %#x\n",
		    dev.name.UTF8String ?: "(nil)", [dev conformsToProtocol:@protocol(MTLDevice)],
		    spi && [dev conformsToProtocol:spi], svc);
		CHECK(dev != nil && spi && [dev conformsToProtocol:spi], "device");
		if (dev == nil) {
			printf("MTLTEST FAILED\n");
			return 1;
		}
		id<MTLCommandQueue> q = [dev newCommandQueue];
		NSURL *url = [NSURL fileURLWithPath:@"/private/var/tmp/sweetmetal-test/test.metallib"];
		id<MTLLibrary> lib = [dev newLibraryWithURL:url error:&err];
		CHECK(q && lib, "queue or test library: %s", err.localizedDescription.UTF8String);
		if (q && lib) {
			test_compute(dev, q, lib);
			test_render(dev, q, lib);
			test_iosurface(dev, q, lib);
		}
		NSURL *qc = [[NSBundle bundleWithPath:@"/System/Library/Frameworks/QuartzCore.framework"]
		    URLForResource:@"default" withExtension:@"metallib"];
		id<MTLLibrary> qcl = qc ? [dev newLibraryWithURL:qc error:&err] : nil;
		printf("QuartzCore library %s: %s, %lu functions\n", qc.path.UTF8String ?: "(not found)",
		    qcl ? "loaded" : err.localizedDescription.UTF8String,
		    (unsigned long)qcl.functionNames.count);
		CHECK(qcl != nil, "QuartzCore library");
		printf("%s\n", fails ? "MTLTEST FAILED" : "MTLTEST PASSED");
		return fails ? 1 : 0;
	}
}
