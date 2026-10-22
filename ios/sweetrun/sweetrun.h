/*
 * Helpers for programs started by sweetrun (ios/sweetrun/sweetrun.c).
 */
#ifndef SWEETRUN_H
#define SWEETRUN_H

/*
 * Unpack the extra file `prop` from the boot image (mkadt.py --run-file:
 * a zlib stream in properties PROP-0, PROP-1, ... and its size in PROP-z,
 * of the device tree node /sweet-run) to `path`; returns path, or NULL
 * when it is missing or cannot be unpacked.
 */
const char *sweetrun_file(const char *prop, const char *path);

#endif
