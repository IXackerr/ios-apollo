/*
 * Linked into the programs sweetrun starts.
 *
 * Output: the stdout launchd hands over never reaches the console log, so
 * stdout and stderr are pointed at /dev/null instead, which the SWEET
 * kernel prints to the console for the processes named in boot-arg
 * sweetnull (sweetrun-program), and stdout is line-buffered so a crash
 * loses nothing that was printed.
 *
 * Files: sweetrun_file() (sweetrun.h) unpacks extra files that came with
 * the boot image.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#include "sweetrun.h"

typedef mach_port_t io_object_t;
typedef io_object_t io_registry_entry_t;
extern io_registry_entry_t IORegistryEntryFromPath(mach_port_t mainPort, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key,
    CFAllocatorRef allocator, uint32_t options);
extern kern_return_t IOObjectRelease(io_object_t object);

__attribute__((constructor)) static void
sweetlog_init(void)
{
	struct stat st;
	int was = fstat(STDOUT_FILENO, &st) == 0 ? 0 : errno;
	int fd = open("/dev/null", O_WRONLY);

	if (fd < 0) {
		return;
	}
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	if (fd > STDERR_FILENO) {
		close(fd);
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (was) {
		printf("sweetlog: launchd's stdout was not open (errno %d)\n", was);
	} else {
		printf("sweetlog: launchd's stdout was mode %#o rdev %#x\n",
		    (unsigned)st.st_mode, (unsigned)st.st_rdev);
	}
}

static CFDataRef
run_prop(io_registry_entry_t node, const char *name)
{
	CFStringRef key = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	CFTypeRef v = IORegistryEntryCreateCFProperty(node, key, NULL, 0);
	CFRelease(key);
	if (v != NULL && CFGetTypeID(v) != CFDataGetTypeID()) {
		CFRelease(v);
		v = NULL;
	}
	return v;
}

static int
write_all(int fd, CFDataRef data)
{
	const UInt8 *p = CFDataGetBytePtr(data);
	CFIndex left = CFDataGetLength(data);
	while (left > 0) {
		ssize_t n = write(fd, p, (size_t)left);
		if (n <= 0) {
			return -1;
		}
		p += n;
		left -= n;
	}
	return 0;
}

const char *
sweetrun_file(const char *prop, const char *path)
{
	io_registry_entry_t node = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/sweet-run");
	if (node == MACH_PORT_NULL) {
		return NULL;
	}
	unlink(path);
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0755);
	int chunks = 0, err = fd < 0;
	/* the file is split into properties PROP-0, PROP-1, ... (mkadt.py) */
	for (; fd >= 0 && !err; chunks++) {
		char name[32];
		snprintf(name, sizeof(name), "%s-%d", prop, chunks);
		CFDataRef d = run_prop(node, name);
		if (d == NULL) {
			break;
		}
		err = write_all(fd, d);
		CFRelease(d);
	}
	IOObjectRelease(node);
	if (fd >= 0) {
		close(fd);
	}
	if (err || chunks == 0) {
		printf("sweetrun_file: %s: %s\n", prop, err ? "cannot write" : "not in the boot image");
		unlink(path);
		return NULL;
	}
	return path;
}
