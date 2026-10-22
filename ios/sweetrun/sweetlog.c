/*
 * Linked into the programs sweetrun starts (not into libraries: it takes
 * over the process's stdout).
 *
 * The stdout launchd hands over never reaches the console log, so stdout
 * and stderr are pointed at /dev/null instead, which the SWEET kernel
 * prints to the console for the processes named in boot-arg sweetnull
 * (sweetrun-program), and stdout is line-buffered so a crash loses nothing
 * that was printed.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

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
