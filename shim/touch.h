#pragma once
#include <stdint.h>
/* NT36675 probe; firmware in the ramdisk. Runs for about `seconds`. */
void test_touch(uint64_t dtb, unsigned seconds);
