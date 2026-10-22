/*
 * sysapps - make iOS's removable system apps (Calendar, Clock, Notes, ...)
 * visible on the phone.
 *
 * iOS 16 ships them in /private/var/staged_system_apps, and a data
 * migration on first boot installs them. Our root skips that migration, so
 * LaunchServices never learns of them. This links each staged app into
 * /Applications (hard links: no data is copied, the staged copy stays as
 * it is), and sets aside LaunchServices' database and SpringBoard's icon
 * layout, so that the next boot registers /Applications again and lays out
 * the home screen from its defaults.
 *
 *   sysapps          what it would do
 *   sysapps apply    do it (run as root, then reboot)
 *   sysapps undo     remove the linked apps, put the set-aside files back
 *
 * Set-aside files keep their name with ".sweet-bak" appended.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STAGED  "/private/var/staged_system_apps"
#define APPS    "/Applications"
#define BAK     ".sweet-bak"

static const char *const aside[] = {
	"/private/var/mobile/Containers/Data/InternalDaemon/*/Library/Caches/*com.apple.LaunchServices*.csstore",
	"/private/var/mobile/Library/SpringBoard/IconState.plist",
	NULL,
};

static int apply, undo;
static unsigned files, dirs, failed;

/* Link the tree at from into to (which does not exist yet). */
static int
link_tree(const char *from, const char *to)
{
	struct stat st;
	if (lstat(from, &st) != 0) {
		return -1;
	}
	if (S_ISDIR(st.st_mode)) {
		if (mkdir(to, st.st_mode & 07777) != 0 && errno != EEXIST) {
			return -1;
		}
		(void)lchown(to, st.st_uid, st.st_gid);
		dirs++;
		DIR *d = opendir(from);
		if (d == NULL) {
			return -1;
		}
		struct dirent *e;
		int err = 0;
		while ((e = readdir(d)) != NULL) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
				continue;
			}
			char f[PATH_MAX], t[PATH_MAX];
			snprintf(f, sizeof(f), "%s/%s", from, e->d_name);
			snprintf(t, sizeof(t), "%s/%s", to, e->d_name);
			if (link_tree(f, t) != 0) {
				fprintf(stderr, "sysapps: %s: %s\n", f, strerror(errno));
				failed++;
				err = -1;
			}
		}
		closedir(d);
		return err;
	}
	if (S_ISLNK(st.st_mode)) {
		char target[PATH_MAX];
		ssize_t n = readlink(from, target, sizeof(target) - 1);
		if (n < 0) {
			return -1;
		}
		target[n] = 0;
		files++;
		return symlink(target, to);
	}
	files++;
	return link(from, to);
}

/* Remove a tree whose files are links of the staged copy (st_nlink > 1). */
static int
remove_tree(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0) {
		return -1;
	}
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		if (d == NULL) {
			return -1;
		}
		struct dirent *e;
		while ((e = readdir(d)) != NULL) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
				continue;
			}
			char p[PATH_MAX];
			snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
			remove_tree(p);
		}
		closedir(d);
		dirs++;
		return rmdir(path);
	}
	files++;
	return unlink(path);
}

/* Whether app in /Applications is our link of the staged one. */
static int
is_our_link(const char *app, const char *staged)
{
	char a[PATH_MAX], s[PATH_MAX];
	struct stat sa, ss;
	snprintf(a, sizeof(a), "%s/Info.plist", app);
	snprintf(s, sizeof(s), "%s/Info.plist", staged);
	return stat(a, &sa) == 0 && stat(s, &ss) == 0 && sa.st_ino == ss.st_ino &&
	    sa.st_dev == ss.st_dev;
}

static void
apps(void)
{
	DIR *d = opendir(STAGED);
	if (d == NULL) {
		fprintf(stderr, "sysapps: %s: %s\n", STAGED, strerror(errno));
		failed++;
		return;
	}
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		size_t n = strlen(e->d_name);
		if (n < 5 || strcmp(e->d_name + n - 4, ".app") != 0) {
			continue;
		}
		char staged[PATH_MAX], app[PATH_MAX];
		snprintf(staged, sizeof(staged), "%s/%s", STAGED, e->d_name);
		snprintf(app, sizeof(app), "%s/%s", APPS, e->d_name);
		struct stat st;
		int exists = lstat(app, &st) == 0;
		if (undo) {
			if (exists && is_our_link(app, staged)) {
				printf("sysapps: remove %s\n", app);
				if (apply && remove_tree(app) != 0) {
					fprintf(stderr, "sysapps: %s: %s\n", app, strerror(errno));
					failed++;
				}
			}
			continue;
		}
		if (exists) {
			printf("sysapps: %s already there\n", app);
			continue;
		}
		printf("sysapps: link %s\n", app);
		if (apply && link_tree(staged, app) != 0) {
			failed++;
		}
	}
	closedir(d);
}

static void
set_aside(void)
{
	for (const char *const *g = aside; *g; g++) {
		glob_t gl;
		char pattern[PATH_MAX];
		snprintf(pattern, sizeof(pattern), "%s%s", *g, undo ? BAK : "");
		if (glob(pattern, 0, NULL, &gl) != 0) {
			continue;
		}
		for (size_t i = 0; i < gl.gl_pathc; i++) {
			const char *p = gl.gl_pathv[i];
			char to[PATH_MAX];
			if (undo) {
				snprintf(to, sizeof(to), "%.*s", (int)(strlen(p) - strlen(BAK)), p);
			} else {
				snprintf(to, sizeof(to), "%s%s", p, BAK);
			}
			printf("sysapps: %s -> %s\n", p, to);
			if (apply && rename(p, to) != 0) {
				fprintf(stderr, "sysapps: rename %s: %s\n", p, strerror(errno));
				failed++;
			}
		}
		globfree(&gl);
	}
}

int
main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "apply")) {
			apply = 1;
		} else if (!strcmp(argv[i], "undo")) {
			undo = apply = 1;
		}
	}
	printf("sysapps: %s\n", undo ? "undo" : apply ? "apply" : "dry run");
	apps();
	set_aside();
	if (apply) {
		sync();
	}
	printf("sysapps: done, %u files and %u directories %s, %u failures\n", files, dirs,
	    undo ? "removed" : "linked", failed);
	return failed ? 1 : 0;
}
