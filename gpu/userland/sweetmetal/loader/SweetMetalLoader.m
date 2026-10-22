/*
 * SweetMetal.bundle as installed on the root: a loader.
 *
 * Metal loads /System/Library/Extensions/SweetMetal.bundle and creates a
 * SweetMetalDevice with -initWithAcceleratorPort:. This class only passes
 * Metal's checks (a _MTLDevice subclass) and, in its initializer, hands
 * over to the driver that came with the boot image: the device tree node
 * /sweet-metal (tools/mkadt.py --metal-file) carries SweetMetalImpl.bundle
 * and its libraries, unpacked once per boot image into
 * /private/var/sweetmetal/<id>/. The device returned is the
 * implementation's. A new driver then needs a new boot image only.
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <os/log.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sweetrun.h"

#define NODE    "IODeviceTree:/sweet-metal"
#define ROOT    "/private/var/sweetmetal"

/* Metal's private device base class (Metal.framework, no SDK header) */
@interface _MTLDevice : NSObject
@end

@interface SweetMetalDevice : _MTLDevice
@end

@interface NSObject (SweetMetalPlugin)
- (instancetype)initWithAcceleratorPort:(mach_port_t)port;
@end

typedef mach_port_t io_object_t;
typedef io_object_t io_registry_entry_t;
extern io_registry_entry_t IORegistryEntryFromPath(mach_port_t mainPort, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key,
    CFAllocatorRef allocator, uint32_t options);
extern kern_return_t IOObjectRelease(io_object_t object);

static void
smlog(NSString *msg)
{
	os_log(OS_LOG_DEFAULT, "SweetMetal loader: %{public}@", msg);
	fprintf(stderr, "SweetMetal loader: %s\n", msg.UTF8String);
}

/* The "id" of the driver in the boot image, nil without one. */
static NSString *
image_id(void)
{
	io_registry_entry_t node = IORegistryEntryFromPath(MACH_PORT_NULL, NODE);
	if (node == MACH_PORT_NULL) {
		return nil;
	}
	CFTypeRef v = IORegistryEntryCreateCFProperty(node, CFSTR("id"), NULL, 0);
	IOObjectRelease(node);
	if (v == NULL || CFGetTypeID(v) != CFDataGetTypeID()) {
		if (v != NULL) {
			CFRelease(v);
		}
		return nil;
	}
	NSData *d = CFBridgingRelease(v);
	return [[NSString alloc] initWithBytes:d.bytes length:strnlen(d.bytes, d.length)
	    encoding:NSUTF8StringEncoding];
}

/*
 * The unpacked SweetMetalImpl.bundle, unpacking it first if this boot
 * image's driver is not there yet. Processes race for it (backboardd and
 * every app making a device), so one unpacks under a lock into a
 * temporary directory and renames it into place.
 */
static NSString *
impl_bundle(void)
{
	NSString *ident = image_id();
	if (ident == nil) {
		smlog(@"no driver in the boot image (" NODE ")");
		return nil;
	}
	NSString *dir = [@ROOT stringByAppendingPathComponent:ident];
	NSString *bundle = [dir stringByAppendingPathComponent:@"SweetMetalImpl.bundle"];
	NSString *done = [bundle stringByAppendingPathComponent:@".unpacked"];
	if (access(done.fileSystemRepresentation, F_OK) == 0) {
		return bundle;
	}
	/* read-only: whoever made the lock file (root or mobile), any client
	 * can still flock it */
	int lock = open(ROOT "/.lock", O_RDONLY | O_CREAT, 0644);
	if (lock < 0 || flock(lock, LOCK_EX) != 0) {
		smlog([NSString stringWithFormat:@"cannot lock " ROOT ": %s", strerror(errno)]);
		if (lock >= 0) {
			close(lock);
		}
		return nil;
	}
	if (access(done.fileSystemRepresentation, F_OK) != 0) {
		NSString *tmp = [NSString stringWithFormat:@"%@.%d", dir, getpid()];
		NSString *tb = [tmp stringByAppendingPathComponent:@"SweetMetalImpl.bundle"];
		mkdir(tmp.fileSystemRepresentation, 0755);
		mkdir(tb.fileSystemRepresentation, 0755);
		static const char files[] =
		    "impl\0SweetMetalImpl\0plist\0Info.plist\0core\0libsweetmetal_core.dylib\0"
		    "sweetvk\0libsweetvk.dylib\0vk\0libvulkan_freedreno.dylib\0";
		BOOL ok = YES;
		for (const char *p = files; *p && ok; ) {
			const char *name = p + strlen(p) + 1;
			NSString *path = [tb stringByAppendingPathComponent:@(name)];
			ok = sweet_dt_file(NODE, p, path.fileSystemRepresentation) != NULL;
			p = name + strlen(name) + 1;
		}
		if (ok) {
			close(open([tb stringByAppendingPathComponent:@".unpacked"].fileSystemRepresentation,
			    O_WRONLY | O_CREAT, 0644));
			ok = rename(tmp.fileSystemRepresentation, dir.fileSystemRepresentation) == 0;
		}
		if (!ok) {
			smlog([NSString stringWithFormat:@"cannot unpack the driver into %@", dir]);
			[[NSFileManager defaultManager] removeItemAtPath:tmp error:nil];
			bundle = nil;
		} else {
			smlog([NSString stringWithFormat:@"driver %@ unpacked by %s", ident, getprogname()]);
		}
	}
	flock(lock, LOCK_UN);
	close(lock);
	return bundle;
}

@implementation SweetMetalDevice

- (instancetype)initWithAcceleratorPort:(mach_port_t)port
{
	/*
	 * This instance is never initialized by _MTLDevice, whose -dealloc then
	 * releases state that was never made: keep it alive. It is one small
	 * object per device creation, and the device returned is another.
	 */
	(void)CFBridgingRetain(self);

	NSString *path = impl_bundle();
	NSBundle *bundle = path ? [NSBundle bundleWithPath:path] : nil;
	NSError *err = nil;
	if (bundle == nil || ![bundle loadAndReturnError:&err]) {
		smlog([NSString stringWithFormat:@"cannot load %@: %@", path, err]);
		return nil;
	}
	Class impl = bundle.principalClass;
	if (impl == Nil) {
		smlog([NSString stringWithFormat:@"%@ has no principal class", path]);
		return nil;
	}
	return [[impl alloc] initWithAcceleratorPort:port];
}

@end
