/*
 * Helpers for programs started by sweetrun (ios/sweetrun/sweetrun.c).
 */
#ifndef SWEETRUN_H
#define SWEETRUN_H

/*
 * Write property `prop` of the device tree node /sweet-run (an extra file
 * from the boot image, mkadt.py --run-lib) to `path`; returns path, or
 * NULL when the property is missing or cannot be written.
 */
const char *sweetrun_file(const char *prop, const char *path);

#endif
