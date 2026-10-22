#pragma once
#include <stdint.h>

/* Is there a SWEETXNU payload in the ABL ramdisk? */
int loader_has_payload(uintptr_t dtb);
/* Boot it, with `prefix` put in front of its boot-args (XNU takes the first
 * occurrence of an argument); returns only on error. */
const char *loader_boot(uintptr_t dtb, const char *prefix);
