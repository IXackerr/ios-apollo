/*
 * Helpers for programs started by sweetrun (ios/sweetrun/sweetrun.c).
 */
#ifndef SWEETRUN_H
#define SWEETRUN_H

/*
 * Write the extra file `prop` from the boot image (mkadt.py --run-lib:
 * properties PROP-0, PROP-1, ... of the device tree node /sweet-run) to
 * `path`; returns path, or NULL when it is missing or cannot be written.
 */
const char *sweetrun_file(const char *prop, const char *path);

#endif
