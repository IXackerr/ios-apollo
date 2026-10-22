/*
 * Copyright © 2020 Google, Inc. (tu_knl_kgsl.cc, from which this is derived)
 * Copyright © 2026 ios-sweet contributors
 * SPDX-License-Identifier: MIT
 *
 * tu_knl_sweet.cc - turnip kernel backend for the SWEET Adreno driver
 * (XNU IOKit service "SweetAdreno", see libsweetgpu/sweetgpu.h and
 * sweet_gpu_uapi.h).
 *
 * The kernel interface is modelled on Qualcomm KGSL, so this file follows
 * tu_knl_kgsl.cc closely:
 *   - the kernel picks GPU virtual addresses and every allocation stays
 *     mapped in the GPU address space for its lifetime (no BO lists);
 *   - one kernel context per VkQueue, each with a monotonically increasing
 *     32-bit timestamp; a submission returns its timestamp;
 *   - vk_sync is a pure user-space state machine {UNSIGNALED, SIGNALED,
 *     TS(queue, timestamp)}; timelines are emulated by the runtime.
 * Dropped relative to KGSL: dma-buf/ION export/import, sync_file fds,
 * sparse "virtual BOs", SVM/fixed-VA allocations, perfetto profiling
 * buffers. The physical device therefore advertises no external memory or
 * SYNC_FD handle types, no sparse binding and no capture/replay.
 *
 * Buffer CPU mappings are created by the kernel at allocation time (one
 * IOMemoryMap per buffer in the client task) and destroyed by MEM_FREE,
 * so BOs are never_unmap and placed maps are not supported.
 */

#include <errno.h>
#include <pthread.h>
#include <string.h>

#include "util/os_time.h"
#include "util/timespec.h"
#include "util/u_debug.h"
#include "util/u_dynarray.h"
#include "vk_util.h"

#include "tu_cmd_buffer.h"
#include "tu_cs.h"
#include "tu_device.h"
#include "tu_dynamic_rendering.h"
#include "tu_knl.h"
#include "tu_queue.h"
#include "tu_rmv.h"

#include "sweet/sweetgpu.h"

static inline sgpu_dev *
sweet_dev(struct tu_device *dev)
{
   return (sgpu_dev *) dev->physical_device->sweet_dev;
}

static VkResult
sweet_errno_to_vk(struct tu_device *dev, int err, const char *what)
{
   switch (err) {
   case 0:
      return VK_SUCCESS;
   case -ETIMEDOUT:
      return VK_TIMEOUT;
   case -ENOMEM:
   case -ENOSPC:
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY, "%s: %s", what, strerror(-err));
   default:
      return vk_device_set_lost(&dev->vk, "%s failed: %s", what, strerror(-err));
   }
}

/* ------------------------------------------------------------------ */
/* contexts                                                           */
/* ------------------------------------------------------------------ */

static int
sweet_submitqueue_new(struct tu_device *dev, struct tu_queue *queue)
{
   uint32_t ctx_id;
   int ret = sgpu_ctx_create(sweet_dev(dev), 0, queue->priority, &ctx_id);
   if (ret)
      return ret;
   queue->msm_queue_id = ctx_id;
   return 0;
}

static void
sweet_submitqueue_close(struct tu_device *dev, struct tu_queue *queue)
{
   sgpu_ctx_destroy(sweet_dev(dev), queue->msm_queue_id);
}

/* ------------------------------------------------------------------ */
/* buffers                                                            */
/* ------------------------------------------------------------------ */

static VkResult
sweet_bo_init(struct tu_device *dev,
              struct vk_object_base *base,
              struct tu_bo **out_bo,
              uint64_t size,
              uint64_t client_iova,
              VkMemoryPropertyFlags mem_property,
              enum tu_bo_alloc_flags flags,
              struct tu_sparse_vma *lazy_vma,
              const char *name)
{
   assert(!lazy_vma); /* has_lazy_bos == false */

   if (client_iova) {
      return vk_errorf(dev, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                       "sweet: fixed GPU addresses are not supported");
   }
   if (flags & TU_BO_ALLOC_SHAREABLE) {
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "sweet: exportable memory is not supported yet");
   }

   struct sgpu_mem_alloc req = {
      .size = size,
      /* shaders and command streams can live in any BO */
      .flags = SGPU_MEM_GPU_EXEC,
   };
   if (mem_property & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
      req.flags |= SGPU_MEM_CACHED;
   if (flags & TU_BO_ALLOC_GPU_READ_ONLY)
      req.flags |= SGPU_MEM_GPU_READONLY;

   int ret = sgpu_mem_alloc(sweet_dev(dev), &req);
   if (ret) {
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "sweet: MEM_ALLOC of %" PRIu64 " bytes failed (%s)",
                       size, strerror(-ret));
   }

   struct tu_bo *bo = tu_device_lookup_bo(dev, req.id);
   assert(bo && bo->gem_handle == 0);

   *bo = (struct tu_bo) {
      .gem_handle = req.id,
      .size = req.alloc_size,
      .iova = req.gpuaddr,
      .name = tu_debug_bos_add(dev, req.alloc_size, name),
      .refcnt = 1,
      .sweet_map = (void *) (uintptr_t) req.cpu_addr,
      .base = base,
   };
   /* the CPU mapping belongs to the kernel object: tu_bo_unmap must not
    * munmap() it (that would leak the IOMemoryMap) */
   bo->never_unmap = true;

   tu_dump_bo_init(dev, bo);

   *out_bo = bo;

   TU_RMV(bo_allocate, dev, bo);
   if (flags & TU_BO_ALLOC_INTERNAL_RESOURCE) {
      TU_RMV(internal_resource_create, dev, bo);
      TU_RMV(resource_name, dev, bo, name);
   }

   return VK_SUCCESS;
}

static VkResult
sweet_bo_init_dmabuf(struct tu_device *dev, struct tu_bo **out_bo,
                     uint64_t size, enum tu_bo_alloc_flags flags, int fd)
{
   return vk_errorf(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                    "sweet: memory import is not supported yet");
}

/* VK_EXT_external_memory_host: the caller's pages, wired and mapped into
 * the GPU by the kernel. The CPU side already has them at ptr. */
static VkResult
sweet_bo_init_userptr(struct tu_device *dev, struct tu_bo **out_bo,
                      void *ptr, uint64_t size)
{
   struct sgpu_import_user req = {
      .addr = (uintptr_t) ptr,
      .size = size,
      /* the pages are cached for the CPU: an IO-coherent GPU snoops them */
      .flags = dev->physical_device->has_cached_coherent_memory ? SGPU_MEM_CACHED : 0u,
   };
   int ret = sgpu_import_user(sweet_dev(dev), &req);
   if (ret) {
      return vk_errorf(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                       "sweet: IMPORT_USER of %" PRIu64 " bytes at %p failed (%s)",
                       size, ptr, strerror(-ret));
   }

   struct tu_bo *bo = tu_device_lookup_bo(dev, req.id);
   assert(bo && bo->gem_handle == 0);

   *bo = (struct tu_bo) {
      .gem_handle = req.id,
      .size = size,
      .iova = req.gpuaddr,
      .name = tu_debug_bos_add(dev, size, "host pointer"),
      .refcnt = 1,
      .sweet_map = ptr,
   };
   bo->never_unmap = true;      /* the caller's mapping, not ours */

   tu_dump_bo_init(dev, bo);
   *out_bo = bo;
   TU_RMV(bo_allocate, dev, bo);
   return VK_SUCCESS;
}

static int
sweet_bo_export_dmabuf(struct tu_device *dev, struct tu_bo *bo)
{
   return -1;
}

static VkResult
sweet_bo_map(struct tu_device *dev, struct tu_bo *bo, void *placed_addr)
{
   if (placed_addr != NULL || bo->sweet_map == NULL)
      return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);

   bo->map = bo->sweet_map;
   TU_RMV(bo_map, dev, bo);
   return VK_SUCCESS;
}

static void
sweet_bo_allow_dump(struct tu_device *dev, struct tu_bo *bo)
{
}

static void
sweet_bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   assert(bo->gem_handle);

   if (!p_atomic_dec_zero(&bo->refcnt))
      return;

   tu_debug_bos_del(dev, bo);
   tu_dump_bo_del(dev, bo);

   if (bo->map)
      TU_RMV(bo_unmap, dev, bo);
   TU_RMV(bo_destroy, dev, bo);

   uint32_t id = bo->gem_handle;

   /* Tell sparse array that entry is free before the id can be reused */
   memset(bo, 0, sizeof(*bo));

   /* the kernel unmaps from the GPU only once queued work is past it */
   sgpu_mem_free(sweet_dev(dev), id);
}

/* ------------------------------------------------------------------ */
/* synchronisation: user-space syncobj on per-context timestamps      */
/* ------------------------------------------------------------------ */

enum sweet_syncobj_state {
   SWEET_SYNCOBJ_STATE_UNSIGNALED,
   SWEET_SYNCOBJ_STATE_SIGNALED,
   SWEET_SYNCOBJ_STATE_TS,
};

struct sweet_syncobj
{
   enum sweet_syncobj_state state;
   struct tu_queue *queue;
   uint32_t timestamp;
};

static void
sweet_syncobj_init(struct sweet_syncobj *s, bool signaled)
{
   s->state = signaled ? SWEET_SYNCOBJ_STATE_SIGNALED : SWEET_SYNCOBJ_STATE_UNSIGNALED;
   s->queue = NULL;
   s->timestamp = UINT32_MAX;
}

static void
sweet_syncobj_reset(struct sweet_syncobj *s)
{
   sweet_syncobj_init(s, false);
}

/* true if timestamp a is at or after b (wrap-safe) */
static inline bool
timestamp_cmp(uint32_t a, uint32_t b)
{
   return (int32_t) (a - b) >= 0;
}

static uint32_t
max_ts(uint32_t a, uint32_t b)
{
   return timestamp_cmp(a, b) ? a : b;
}

static uint32_t
min_ts(uint32_t a, uint32_t b)
{
   return timestamp_cmp(a, b) ? b : a;
}

/*
 * Wait for (ctx, ts) with an absolute CLOCK_MONOTONIC deadline. The kernel
 * takes a relative timeout in ms (32 bits); long or infinite waits loop.
 */
static VkResult
sweet_wait_timestamp(struct tu_device *dev, uint32_t ctx_id, uint32_t ts,
                     uint64_t abs_timeout_ns)
{
   for (;;) {
      uint64_t now = os_time_get_nano();
      uint32_t ms;
      if (abs_timeout_ns == UINT64_MAX || abs_timeout_ns >= INT64_MAX)
         ms = 1000;
      else if (abs_timeout_ns <= now)
         ms = 0;
      else
         ms = (uint32_t) MIN2((abs_timeout_ns - now + 999999) / 1000000, 1000);

      int ret = sgpu_wait_ts(sweet_dev(dev), ctx_id, ts, ms);
      if (ret == 0)
         return VK_SUCCESS;
      if (ret != -ETIMEDOUT)
         return vk_device_set_lost(&dev->vk, "GPU fault or hang (wait: %s)",
                                   strerror(-ret));
      if (ms == 0 || (abs_timeout_ns != UINT64_MAX && abs_timeout_ns < INT64_MAX &&
                      os_time_get_nano() >= abs_timeout_ns))
         return VK_TIMEOUT;
   }
}

static VkResult
sweet_queue_wait_fence(struct tu_queue *queue, uint32_t fence, uint64_t timeout_ns)
{
   uint64_t abs = timeout_ns >= INT64_MAX ? UINT64_MAX : os_time_get_nano() + timeout_ns;
   return sweet_wait_timestamp(queue->device, queue->msm_queue_id, fence, abs);
}

/* timeline_cond is CLOCK_MONOTONIC on Linux; Darwin has no condattr clock,
 * so use the relative wait there (tu_device.cc skips setclock on Apple). */
static int
sweet_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, uint64_t abs_ns)
{
#ifdef __APPLE__
   uint64_t now = os_time_get_nano();
   struct timespec rel;
   timespec_from_nsec(&rel, abs_ns > now ? abs_ns - now : 0);
   return pthread_cond_timedwait_relative_np(cond, mutex, &rel);
#else
   struct timespec abstime;
   timespec_from_nsec(&abstime, abs_ns);
   return pthread_cond_timedwait(cond, mutex, &abstime);
#endif
}

static VkResult
sweet_syncobj_wait(struct tu_device *device, struct sweet_syncobj *s,
                   uint64_t abs_timeout_ns)
{
   if (s->state == SWEET_SYNCOBJ_STATE_UNSIGNALED) {
      /* wait-before-signal: wait for a submit to turn it into a TS */
      if (abs_timeout_ns == 0)
         return VK_TIMEOUT;

      pthread_mutex_lock(&device->submit_mutex);
      while (s->state == SWEET_SYNCOBJ_STATE_UNSIGNALED) {
         int ret;
         if (abs_timeout_ns == UINT64_MAX)
            ret = pthread_cond_wait(&device->timeline_cond, &device->submit_mutex);
         else
            ret = sweet_cond_timedwait(&device->timeline_cond, &device->submit_mutex,
                                       abs_timeout_ns);
         if (ret != 0) {
            pthread_mutex_unlock(&device->submit_mutex);
            return VK_TIMEOUT;
         }
      }
      pthread_mutex_unlock(&device->submit_mutex);
   }

   switch (s->state) {
   case SWEET_SYNCOBJ_STATE_SIGNALED:
      return VK_SUCCESS;
   case SWEET_SYNCOBJ_STATE_TS:
      return sweet_wait_timestamp(device, s->queue->msm_queue_id, s->timestamp,
                                  abs_timeout_ns);
   default:
      return VK_TIMEOUT;
   }
}

static VkResult
sweet_syncobj_wait_any(struct tu_device *device, struct sweet_syncobj **syncobjs,
                       uint32_t count, uint64_t abs_timeout_ns)
{
   if (count == 0)
      return VK_TIMEOUT;
   if (count == 1)
      return sweet_syncobj_wait(device, syncobjs[0], abs_timeout_ns);

   for (uint32_t i = 0; i < count; i++) {
      if (syncobjs[i]->state == SWEET_SYNCOBJ_STATE_SIGNALED)
         return VK_SUCCESS;
   }

   /*
    * TS objects of one queue retire in order: the earliest one is the
    * first to signal. Objects on different queues (only possible with
    * more than one VkQueue) are polled round-robin.
    */
   for (;;) {
      struct tu_queue *queue = NULL;
      bool one_queue = true, any_ts = false;
      uint32_t lowest = 0;
      for (uint32_t i = 0; i < count; i++) {
         struct sweet_syncobj *s = syncobjs[i];
         if (s->state != SWEET_SYNCOBJ_STATE_TS)
            continue;
         if (queue && s->queue != queue)
            one_queue = false;
         lowest = (!any_ts) ? s->timestamp : min_ts(lowest, s->timestamp);
         queue = s->queue;
         any_ts = true;
      }
      if (!any_ts)
         return VK_TIMEOUT;        /* only unsignaled ones */
      if (one_queue)
         return sweet_wait_timestamp(device, queue->msm_queue_id, lowest,
                                     abs_timeout_ns);

      for (uint32_t i = 0; i < count; i++) {
         struct sweet_syncobj *s = syncobjs[i];
         if (s->state == SWEET_SYNCOBJ_STATE_TS &&
             sweet_wait_timestamp(device, s->queue->msm_queue_id, s->timestamp, 0) ==
                VK_SUCCESS)
            return VK_SUCCESS;
      }
      if (abs_timeout_ns != UINT64_MAX && os_time_get_nano() >= abs_timeout_ns)
         return VK_TIMEOUT;
      os_time_sleep(100);
   }
}

/* Merge into one syncobj that signals after all of them. TS objects of
 * different queues cannot be merged without kernel help; the second one is
 * returned in *extra for the caller to wait on from the CPU. */
static struct sweet_syncobj
sweet_syncobj_merge(const struct sweet_syncobj **syncobjs, uint32_t count,
                    struct util_dynarray *extra)
{
   struct sweet_syncobj ret;
   sweet_syncobj_init(&ret, true);

   for (uint32_t i = 0; i < count; ++i) {
      const struct sweet_syncobj *sync = syncobjs[i];

      switch (sync->state) {
      case SWEET_SYNCOBJ_STATE_SIGNALED:
         break;
      case SWEET_SYNCOBJ_STATE_UNSIGNALED:
         sweet_syncobj_reset(&ret);
         return ret;
      case SWEET_SYNCOBJ_STATE_TS:
         if (ret.state == SWEET_SYNCOBJ_STATE_TS && ret.queue != sync->queue) {
            util_dynarray_append(extra, *sync);
         } else if (ret.state == SWEET_SYNCOBJ_STATE_TS) {
            ret.timestamp = max_ts(ret.timestamp, sync->timestamp);
         } else {
            ret = *sync;
         }
         break;
      }
   }
   return ret;
}

struct vk_sweet_syncobj
{
   struct vk_sync vk;
   struct sweet_syncobj syncobj;
};

static VkResult
vk_sweet_sync_init(struct vk_device *device, struct vk_sync *sync, uint64_t initial_value)
{
   struct vk_sweet_syncobj *s = container_of(sync, struct vk_sweet_syncobj, vk);
   sweet_syncobj_init(&s->syncobj, initial_value != 0);
   return VK_SUCCESS;
}

static void
vk_sweet_sync_finish(struct vk_device *device, struct vk_sync *sync)
{
}

static VkResult
vk_sweet_sync_reset(struct vk_device *device, struct vk_sync *sync)
{
   struct vk_sweet_syncobj *s = container_of(sync, struct vk_sweet_syncobj, vk);
   sweet_syncobj_reset(&s->syncobj);
   return VK_SUCCESS;
}

static VkResult
vk_sweet_sync_move(struct vk_device *device, struct vk_sync *dst, struct vk_sync *src)
{
   struct vk_sweet_syncobj *d = container_of(dst, struct vk_sweet_syncobj, vk);
   struct vk_sweet_syncobj *s = container_of(src, struct vk_sweet_syncobj, vk);
   d->syncobj = s->syncobj;
   sweet_syncobj_init(&s->syncobj, false);
   return VK_SUCCESS;
}

static VkResult
vk_sweet_sync_wait(struct vk_device *_device, struct vk_sync *sync, uint64_t wait_value,
                   enum vk_sync_wait_flags wait_flags, uint64_t abs_timeout_ns)
{
   struct tu_device *device = container_of(_device, struct tu_device, vk);
   struct vk_sweet_syncobj *s = container_of(sync, struct vk_sweet_syncobj, vk);

   if (wait_flags & VK_SYNC_WAIT_PENDING)
      return VK_SUCCESS;
   return sweet_syncobj_wait(device, &s->syncobj, abs_timeout_ns);
}

static VkResult
vk_sweet_sync_wait_many(struct vk_device *_device, uint32_t wait_count,
                        const struct vk_sync_wait *waits,
                        enum vk_sync_wait_flags wait_flags, uint64_t abs_timeout_ns)
{
   struct tu_device *device = container_of(_device, struct tu_device, vk);

   if (wait_flags & VK_SYNC_WAIT_PENDING)
      return VK_SUCCESS;

   if (wait_flags & VK_SYNC_WAIT_ANY) {
      STACK_ARRAY(struct sweet_syncobj *, syncobjs, wait_count);
      for (uint32_t i = 0; i < wait_count; i++)
         syncobjs[i] = &container_of(waits[i].sync, struct vk_sweet_syncobj, vk)->syncobj;
      VkResult r = sweet_syncobj_wait_any(device, syncobjs, wait_count, abs_timeout_ns);
      STACK_ARRAY_FINISH(syncobjs);
      return r;
   }
   for (uint32_t i = 0; i < wait_count; i++) {
      struct vk_sweet_syncobj *s = container_of(waits[i].sync, struct vk_sweet_syncobj, vk);
      VkResult result = sweet_syncobj_wait(device, &s->syncobj, abs_timeout_ns);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

/* No import/export_sync_file: the runtime then advertises no SYNC_FD
 * handle types for semaphores and fences, which is conformant. */
const struct vk_sync_type vk_sweet_sync_type = {
   .size = sizeof(struct vk_sweet_syncobj),
   .features = (enum vk_sync_features)
               (VK_SYNC_FEATURE_BINARY |
                VK_SYNC_FEATURE_GPU_WAIT |
                VK_SYNC_FEATURE_GPU_MULTI_WAIT |
                VK_SYNC_FEATURE_CPU_WAIT |
                VK_SYNC_FEATURE_CPU_RESET |
                VK_SYNC_FEATURE_WAIT_ANY |
                VK_SYNC_FEATURE_WAIT_PENDING),
   .init = vk_sweet_sync_init,
   .finish = vk_sweet_sync_finish,
   .reset = vk_sweet_sync_reset,
   .move = vk_sweet_sync_move,
   .wait = vk_sweet_sync_wait,
   .wait_many = vk_sweet_sync_wait_many,
};

/* ------------------------------------------------------------------ */
/* submission                                                         */
/* ------------------------------------------------------------------ */

struct tu_sweet_queue_submit {
   struct util_dynarray ibs;   /* struct sgpu_ib */
};

static void *
sweet_submit_create(struct tu_device *device)
{
   return vk_zalloc(&device->vk.alloc, sizeof(struct tu_sweet_queue_submit), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
}

static void
sweet_submit_finish(struct tu_device *device, void *_submit)
{
   struct tu_sweet_queue_submit *submit = (struct tu_sweet_queue_submit *) _submit;
   util_dynarray_fini(&submit->ibs);
   vk_free(&device->vk.alloc, submit);
}

static void
sweet_submit_add_entries(struct tu_device *device, void *_submit,
                         struct tu_cs_entry *entries, unsigned num_entries)
{
   struct tu_sweet_queue_submit *submit = (struct tu_sweet_queue_submit *) _submit;

   for (unsigned i = 0; i < num_entries; i++) {
      assert(entries[i].size % 4 == 0);
      struct sgpu_ib ib = {
         .gpuaddr = entries[i].bo->iova + entries[i].offset,
         .size_dwords = entries[i].size / 4,
         .flags = 0,
      };
      util_dynarray_append(&submit->ibs, ib);
   }
}

static void
sweet_submit_add_bind(struct tu_device *device, void *_submit,
                      struct tu_sparse_vma *vma, uint64_t vma_offset,
                      struct tu_bo *bo, uint64_t bo_offset, uint64_t size)
{
   UNREACHABLE("sweet: sparse binding is not supported");
}

static VkResult
sweet_queue_submit(struct tu_queue *queue, void *_submit,
                   struct vk_sync_wait *waits, uint32_t wait_count,
                   struct vk_sync_signal *signals, uint32_t signal_count,
                   struct tu_u_trace_submission_data *u_trace_submission_data)
{
   struct tu_sweet_queue_submit *submit = (struct tu_sweet_queue_submit *) _submit;
   struct tu_device *dev = queue->device;
   struct util_dynarray extra;
   util_dynarray_init(&extra, NULL);

   STACK_ARRAY(const struct sweet_syncobj *, wait_semaphores, wait_count + 1);
   for (uint32_t i = 0; i < wait_count; i++)
      wait_semaphores[i] = &container_of(waits[i].sync, struct vk_sweet_syncobj, vk)->syncobj;

   uint32_t n_ibs = util_dynarray_num_elements(&submit->ibs, struct sgpu_ib);
   VkResult result = VK_SUCCESS;

   if (n_ibs == 0) {
      /* Nothing for the GPU: the signals complete when the waits and all
       * previous work on this queue complete (same as KGSL). */
      struct sweet_syncobj last;
      if (queue->fence >= 0) {
         last = (struct sweet_syncobj) {
            .state = SWEET_SYNCOBJ_STATE_TS, .queue = queue,
            .timestamp = (uint32_t) queue->fence,
         };
      } else {
         sweet_syncobj_init(&last, true);
      }
      wait_semaphores[wait_count] = &last;
      struct sweet_syncobj merged =
         sweet_syncobj_merge(wait_semaphores, wait_count + 1, &extra);
      /* cross-queue leftovers: resolve on the CPU now */
      util_dynarray_foreach (&extra, struct sweet_syncobj, s) {
         result = sweet_syncobj_wait(dev, s, UINT64_MAX);
         if (result != VK_SUCCESS)
            goto out;
      }
      for (uint32_t i = 0; i < signal_count; i++) {
         struct sweet_syncobj *signal_sync =
            &container_of(signals[i].sync, struct vk_sweet_syncobj, vk)->syncobj;
         *signal_sync = merged;
      }
      goto out;
   }

   {
      struct sweet_syncobj wait_sync =
         sweet_syncobj_merge(wait_semaphores, wait_count, &extra);
      assert(wait_sync.state != SWEET_SYNCOBJ_STATE_UNSIGNALED);

      /*
       * The kernel has no in-submit wait yet. A wait on this queue's own
       * timeline is already satisfied by ring order; anything on another
       * queue is waited for on the CPU (only reachable with >1 VkQueue).
       */
      if (wait_sync.state == SWEET_SYNCOBJ_STATE_TS && wait_sync.queue != queue) {
         result = sweet_syncobj_wait(dev, &wait_sync, UINT64_MAX);
         if (result != VK_SUCCESS)
            goto out;
      }
      util_dynarray_foreach (&extra, struct sweet_syncobj, s) {
         if (s->queue != queue) {
            result = sweet_syncobj_wait(dev, s, UINT64_MAX);
            if (result != VK_SUCCESS)
               goto out;
         }
      }
   }

   {
      struct sgpu_ib *ibs = (struct sgpu_ib *) util_dynarray_begin(&submit->ibs);
      uint32_t timestamp = 0;
      for (uint32_t first = 0; first < n_ibs; first += SGPU_MAX_IBS) {
         uint32_t n = MIN2(n_ibs - first, SGPU_MAX_IBS);
         int ret = sgpu_submit(sweet_dev(dev), queue->msm_queue_id, ibs + first, n, 0,
                               &timestamp);
         if (ret) {
            result = vk_device_set_lost(&dev->vk, "submit failed: %s", strerror(-ret));
            goto out;
         }
      }

      p_atomic_set(&queue->fence, timestamp);

      for (uint32_t i = 0; i < signal_count; i++) {
         struct sweet_syncobj *signal_sync =
            &container_of(signals[i].sync, struct vk_sweet_syncobj, vk)->syncobj;
         signal_sync->state = SWEET_SYNCOBJ_STATE_TS;
         signal_sync->queue = queue;
         signal_sync->timestamp = timestamp;
      }
   }

out:
   STACK_ARRAY_FINISH(wait_semaphores);
   util_dynarray_fini(&extra);
   return result;
}

/* ------------------------------------------------------------------ */
/* device                                                             */
/* ------------------------------------------------------------------ */

static VkResult
sweet_device_init(struct tu_device *dev)
{
   dev->fd = -1;    /* no DRM/KGSL fd: everything goes through libsweetgpu */
   return VK_SUCCESS;
}

static void
sweet_device_finish(struct tu_device *dev)
{
}

static int
sweet_device_get_gpu_timestamp(struct tu_device *dev, uint64_t *ts)
{
   return sgpu_gpu_time(sweet_dev(dev), ts, NULL);
}

static int
sweet_device_get_suspend_count(struct tu_device *dev, uint64_t *suspend_count)
{
   *suspend_count = 0;
   return 0;
}

static VkResult
sweet_device_check_status(struct tu_device *device)
{
   struct sgpu_status st;
   if (sgpu_get_status(sweet_dev(device), &st))
      return vk_device_set_lost(&device->vk, "sweet: status query failed");

   /* any GPU recovery since the device opened lost this device's work */
   if (st.hangs != device->physical_device->sweet_hangs_at_open ||
       st.stage_reached < st.stage_requested)
      return vk_device_set_lost(&device->vk, "GPU hang/recovery (%u hangs): %s",
                                st.hangs, st.message);
   return VK_SUCCESS;
}

static const struct tu_knl sweet_knl_funcs = {
      .name = "sweet",

      .device_init = sweet_device_init,
      .device_finish = sweet_device_finish,
      .device_get_gpu_timestamp = sweet_device_get_gpu_timestamp,
      .device_get_suspend_count = sweet_device_get_suspend_count,
      .device_check_status = sweet_device_check_status,
      .submitqueue_new = sweet_submitqueue_new,
      .submitqueue_close = sweet_submitqueue_close,
      .bo_init = sweet_bo_init,
      .bo_init_dmabuf = sweet_bo_init_dmabuf,
      .bo_export_dmabuf = sweet_bo_export_dmabuf,
      .bo_init_userptr = sweet_bo_init_userptr,
      .bo_map = sweet_bo_map,
      .bo_allow_dump = sweet_bo_allow_dump,
      .bo_finish = sweet_bo_finish,
      .submit_create = sweet_submit_create,
      .submit_finish = sweet_submit_finish,
      .submit_add_entries = sweet_submit_add_entries,
      .submit_add_bind = sweet_submit_add_bind,
      .queue_submit = sweet_queue_submit,
      .queue_wait_fence = sweet_queue_wait_fence,
};

void
tu_knl_sweet_finish(struct tu_physical_device *device)
{
   if (device->sweet_dev) {
      sgpu_close((sgpu_dev *) device->sweet_dev);
      device->sweet_dev = NULL;
   }
}

VkResult
tu_knl_sweet_load(struct tu_instance *instance)
{
   sgpu_dev *sdev = NULL;
   struct sgpu_info info;
   struct sgpu_status st;

   if (sgpu_open(&sdev) != 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;     /* no SweetAdreno service */

   if (sgpu_get_info(sdev, &info) != 0 || sgpu_get_status(sdev, &st) != 0) {
      sgpu_close(sdev);
      return vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                       "sweet: GET_INFO failed");
   }
   if (info.uapi_version != SGPU_UAPI_VERSION) {
      sgpu_close(sdev);
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "sweet: kernel UAPI %u, expected %u", info.uapi_version,
                       SGPU_UAPI_VERSION);
   }

   struct tu_physical_device *device = (struct tu_physical_device *)
      vk_zalloc(&instance->vk.alloc, sizeof(*device), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!device) {
      sgpu_close(sdev);
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   device->instance = instance;
   device->sweet_dev = sdev;
   device->sweet_hangs_at_open = st.hangs;
   /* tu_physical_device_finish close()s these: keep them invalid */
   device->local_fd = -1;
   device->master_fd = -1;
   device->kgsl_dma_fd = -1;

   device->dev_id.gpu_id =
      ((info.chip_id >> 24) & 0xff) * 100 +
      ((info.chip_id >> 16) & 0xff) * 10 +
      ((info.chip_id >>  8) & 0xff);
   device->dev_id.chip_id = info.chip_id;
   device->gmem_size = debug_get_num_option("TU_GMEM", info.gmem_size);
   device->gmem_base = info.gmem_base;
   device->uche_trap_base = info.uche_trap_base;

   device->has_raytracing = false;
   device->submitqueue_priority_count = 1;

   device->timeline_type = vk_sync_timeline_get_type(&vk_sweet_sync_type);
   device->sync_types[0] = &vk_sweet_sync_type;
   device->sync_types[1] = &device->timeline_type.sync;
   device->sync_types[2] = NULL;

   device->va_start = info.va_start;
   device->va_size = info.va_size;

   device->heap.size = tu_get_system_heap_size(device);
   device->heap.used = 0u;
   device->heap.flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

   device->has_set_iova = false;
   device->has_cached_coherent_memory = (info.flags & SGPU_INFO_IO_COHERENT) != 0;
   device->has_sparse = false;
   device->has_sparse_prr = false;
   device->has_lazy_bos = false;
   device->no_external_handles = true;   /* no dma-buf / sync_file on XNU yet */
   device->no_map_placed = true;         /* kernel-owned IOKit mappings */

   /* the kernel uses one ring and no preemption, but true is the safe value
    * (re-emits VSC state per bin) */
   device->has_preemption = true;
   /* CP_PROTECT covers the perf counter selects, like KGSL */
   device->is_perf_cntr_selectable = false;

   device->ubwc_config.highest_bank_bit = info.highest_bank_bit;
   switch (info.ubwc_mode) {
   case 1:
      device->ubwc_config.bank_swizzle_levels = 0x7;
      device->ubwc_config.macrotile_mode = FDL_MACROTILE_4_CHANNEL;
      break;
   case 2:
   case 3:
      device->ubwc_config.bank_swizzle_levels = 0x6;
      device->ubwc_config.macrotile_mode = FDL_MACROTILE_4_CHANNEL;
      break;
   default:     /* 4.0 on SM8250 */
      device->ubwc_config.bank_swizzle_levels = 0x6;
      device->ubwc_config.macrotile_mode = FDL_MACROTILE_8_CHANNEL;
      break;
   }

   instance->knl = &sweet_knl_funcs;

   VkResult result = tu_physical_device_init(device, instance);
   if (result != VK_SUCCESS) {
      vk_free(&instance->vk.alloc, device);
      sgpu_close(sdev);
      return result;
   }

   list_addtail(&device->vk.link, &instance->vk.physical_devices.list);

   if (TU_DEBUG(STARTUP))
      mesa_logi("sweet: Adreno %u (chip %#x), GMEM %u KiB, VA %#" PRIx64 "+%#" PRIx64,
                device->dev_id.gpu_id, info.chip_id, info.gmem_size / 1024,
                info.va_start, info.va_size);
   return VK_SUCCESS;
}
