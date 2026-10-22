/*
 * libsweetgpu - user-space access to the SWEET Adreno driver.
 *
 * Thin C wrapper over the IOKit user client "SweetAdreno"
 * (kernel/sweet_gpu_uapi.h). Used by gputest and by Mesa turnip's "sweet"
 * kernel-mode-driver backend (mesa/tu_knl_sweet.cc).
 *
 * All functions return 0 or a negative errno value. Thread-safe: every
 * call is one Mach message to the kernel, which serialises internally.
 */
#ifndef SWEETGPU_H
#define SWEETGPU_H

#include <stdint.h>
#include <stddef.h>
#include "sweet_gpu_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sgpu_dev sgpu_dev;

int  sgpu_open(sgpu_dev **out);
void sgpu_close(sgpu_dev *dev);

int  sgpu_get_info(sgpu_dev *dev, struct sgpu_info *info);
int  sgpu_get_status(sgpu_dev *dev, struct sgpu_status *st);

int  sgpu_ctx_create(sgpu_dev *dev, uint32_t flags, uint32_t priority, uint32_t *ctx_id);
int  sgpu_ctx_destroy(sgpu_dev *dev, uint32_t ctx_id);

/* Fills id, gpuaddr, cpu_addr, alloc_size of *req. */
int  sgpu_mem_alloc(sgpu_dev *dev, struct sgpu_mem_alloc *req);
int  sgpu_mem_free(sgpu_dev *dev, uint32_t id);
int  sgpu_mem_sync(sgpu_dev *dev, uint32_t id, uint32_t op, uint64_t offset, uint64_t length);
int  sgpu_import_surface(sgpu_dev *dev, struct sgpu_import_surface *req);
/* Fills id and gpuaddr of *req: the caller's pages at addr, mapped for the GPU. */
int  sgpu_import_user(sgpu_dev *dev, struct sgpu_import_user *req);

int  sgpu_submit(sgpu_dev *dev, uint32_t ctx_id, const struct sgpu_ib *ibs,
    uint32_t num_ibs, uint32_t flags, uint32_t *timestamp);
/* timeout_ms 0 = poll; returns -ETIMEDOUT if not retired yet */
int  sgpu_wait_ts(sgpu_dev *dev, uint32_t ctx_id, uint32_t ts, uint32_t timeout_ms);
int  sgpu_read_ts(sgpu_dev *dev, uint32_t ctx_id, uint32_t *retired, uint32_t *queued);
int  sgpu_gpu_time(sgpu_dev *dev, uint64_t *gpu_ticks, uint64_t *cpu_ticks);

#ifdef __cplusplus
}
#endif
#endif
