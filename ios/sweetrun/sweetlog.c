/*
 * Linked into the programs sweetrun starts.
 *
 * Output: the stdout launchd hands over never reaches the console log, so
 * stdout and stderr are pointed at /dev/null instead, which the SWEET
 * kernel prints to the console for the processes named in boot-arg
 * sweetnull (sweetrun-program), and stdout is line-buffered so a crash
 * loses nothing that was printed.
 *
 * Files: sweetrun_file() (sweetrun.h) unpacks the extra, zlib-compressed
 * files that came with the boot image.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <zlib.h>
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
write_all(int fd, const UInt8 *p, size_t left)
{
	while (left > 0) {
		ssize_t n = write(fd, p, left);
		if (n <= 0) {
			return -1;
		}
		p += n;
		left -= (size_t)n;
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
	char name[32];
	snprintf(name, sizeof(name), "%s-z", prop);
	CFDataRef zsize = run_prop(node, name);
	z_stream z = { 0 };
	static UInt8 out[256 * 1024];
	int err = zsize == NULL || inflateInit(&z) != Z_OK;
	unlink(path);
	int fd = err ? -1 : open(path, O_WRONLY | O_CREAT | O_EXCL, 0755);
	int chunks = 0, zr = Z_OK;
	err |= fd < 0;
	/* zlib stream split into properties PROP-0, PROP-1, ... (mkadt.py) */
	for (; !err && zr != Z_STREAM_END; chunks++) {
		snprintf(name, sizeof(name), "%s-%d", prop, chunks);
		CFDataRef d = run_prop(node, name);
		if (d == NULL) {
			break;
		}
		z.next_in = (Bytef *)CFDataGetBytePtr(d);
		z.avail_in = (uInt)CFDataGetLength(d);
		while (!err && z.avail_in > 0 && zr != Z_STREAM_END) {
			z.next_out = out;
			z.avail_out = sizeof(out);
			zr = inflate(&z, Z_NO_FLUSH);
			err = (zr != Z_OK && zr != Z_STREAM_END) ||
			    write_all(fd, out, sizeof(out) - z.avail_out);
		}
		CFRelease(d);
	}
	if (zsize != NULL) {
		uint32_t want = 0;
		memcpy(&want, CFDataGetBytePtr(zsize), sizeof(want));
		err |= zr != Z_STREAM_END || z.total_out != want;
		CFRelease(zsize);
		inflateEnd(&z);
	}
	IOObjectRelease(node);
	if (fd >= 0) {
		close(fd);
	}
	if (err) {
		printf("sweetrun_file: %s: %s\n", prop, zsize == NULL && chunks == 0 ?
		    "not in the boot image" : "cannot unpack");
		unlink(path);
		return NULL;
	}
	return path;
}
