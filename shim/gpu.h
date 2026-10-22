#pragma once
#include <stdint.h>
/* Adreno bring-up to `stage` (sweet_gpu.h enum sgpu_stage); firmware in the ramdisk. */
int test_gpu(uint64_t dtb, unsigned stage);
void gpu_exception(uint64_t esr, uint64_t elr, uint64_t far);
