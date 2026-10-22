/*
 * sweetrun - run a program handed over in the boot image.
 *
 * launchd starts it once at boot (job com.sweet.run, installed by
 * tools/sweet-root-tweaks.py --sweetrun). It reads the device tree node
 * /sweet-run that tools/mkadt.py --run adds:
 *
 *   program   the Mach-O to run
 *   args      its arguments, NUL-separated (optional)
 *   delay     seconds to wait first, 32-bit (optional)
 *
 * The program is written to /private/var/tmp and run with stdout and
 * stderr on the console, so its output ends up in the kernel log. A new
 * test therefore needs a new boot image only, not a change to the root.
 * Without the node it exits at once.
 */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>

/* The iOS SDK ships IOKit.framework without headers. */
typedef mach_port_t io_object_t;
typedef io_object_t io_registry_entry_t;
extern io_registry_entry_t IORegistryEntryFromPath(mach_port_t mainPort, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key,
    CFAllocatorRef allocator, uint32_t options);
extern kern_return_t IOObjectRelease(io_object_t object);

#define NODE		"IODeviceTree:/sweet-run"
#define PROGRAM		"/private/var/tmp/sweetrun-program"
#define MAX_ARGS	32

extern char **environ;

static CFDataRef
prop(io_registry_entry_t node, const char *name)
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
write_program(CFDataRef data)
{
	const UInt8 *p = CFDataGetBytePtr(data);
	CFIndex left = CFDataGetLength(data);
	int fd;

	unlink(PROGRAM);
	fd = open(PROGRAM, O_WRONLY | O_CREAT | O_EXCL, 0755);
	if (fd < 0) {
		return -errno;
	}
	while (left > 0) {
		ssize_t n = write(fd, p, (size_t)left);
		if (n < 0) {
			int err = errno;
			close(fd);
			return -err;
		}
		p += n;
		left -= n;
	}
	close(fd);
	return 0;
}

int
main(void)
{
	io_registry_entry_t node = IORegistryEntryFromPath(MACH_PORT_NULL, NODE);
	if (node == MACH_PORT_NULL) {
		return 0;
	}
	CFDataRef program = prop(node, "program");
	CFDataRef args = prop(node, "args");
	CFDataRef delay = prop(node, "delay");
	IOObjectRelease(node);
	if (program == NULL) {
		printf("sweetrun: %s has no program\n", NODE);
		return 1;
	}

	char *argv[MAX_ARGS + 2] = { PROGRAM };
	int argc = 1;
	char *argbuf = NULL;
	if (args != NULL && CFDataGetLength(args) > 0) {
		CFIndex len = CFDataGetLength(args);
		argbuf = calloc(1, (size_t)len + 1);
		memcpy(argbuf, CFDataGetBytePtr(args), (size_t)len);
		for (char *a = argbuf; a < argbuf + len && argc <= MAX_ARGS; a += strlen(a) + 1) {
			if (*a) {
				argv[argc++] = a;
			}
		}
	}
	argv[argc] = NULL;

	uint32_t wait_s = 0;
	if (delay != NULL && CFDataGetLength(delay) == 4) {
		memcpy(&wait_s, CFDataGetBytePtr(delay), 4);
	}
	if (wait_s) {
		printf("sweetrun: starting in %u s\n", wait_s);
		fflush(stdout);
		sleep(wait_s);
	}

	int err = write_program(program);
	if (err) {
		printf("sweetrun: cannot write %s: %s\n", PROGRAM, strerror(-err));
		return 1;
	}
	printf("sweetrun: running %ld-byte program:", (long)CFDataGetLength(program));
	for (int i = 1; i < argc; i++) {
		printf(" %s", argv[i]);
	}
	printf("\n");
	fflush(stdout);

	pid_t pid;
	err = posix_spawn(&pid, PROGRAM, NULL, NULL, argv, environ);
	if (err) {
		printf("sweetrun: posix_spawn: %s\n", strerror(err));
		return 1;
	}
	int status;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	if (WIFEXITED(status)) {
		printf("sweetrun: exit status %d\n", WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		printf("sweetrun: killed by signal %d\n", WTERMSIG(status));
	}
	return 0;
}
