# Turnip kernel-interface (knl) layer — spec for a Darwin/XNU backend

Source: Mesa 26.2.0 (`9f0a761 VERSION: bump for 26.2.0`), sparse checkout at
a Mesa 26.2.0 tree. All `file:line` references are relative to that tree;
`vk/` = `src/freedreno/vulkan/`.

Target: Adreno 650 (SM8250), Apple XNU, GPU driver exposed as an IOKit user
client (`IOServiceOpen` + `IOConnectCallMethod` + `IOConnectMapMemory64`)
instead of `/dev/kgsl-3d0` + `ioctl` + `mmap`.

Companion specs in this directory: `gmu-hfi-a650.md`, `clocks-scm-zap-a650.md`
(kernel-side bring-up; this document only covers what turnip needs).

---

## 0. TL;DR

* Turnip talks to the kernel only through one vtable, `struct tu_knl`
  (`vk/tu_knl.h:125-178`), selected at runtime by assigning
  `instance->knl` inside a backend `*_load()` function. Three backends ship:
  `msm` (DRM), `virtio` (DRM over virtgpu), `kgsl` (Android downstream). The
  build selects them with `-Dfreedreno-kmds=` → `-DTU_HAS_{MSM,VIRTIO,KGSL}`
  (`vk/meson.build:130-150`).
* KGSL is the right model for XNU: the kernel owns GPU VA allocation, there is
  no BO list in submit (everything allocated is resident in the per-process
  GPU pagetable), synchronization is a per-context 32-bit monotonically
  increasing timestamp, and the sync primitive is a pure-userspace
  `vk_sync` type wrapping `(queue, timestamp)` (`vk/tu_knl_kgsl.cc:658-1270`).
* `tu_knl_kgsl.cc` itself can't be compiled on Darwin (`<linux/dma-heap.h>`,
  ION headers, `msm_kgsl.h` includes `<linux/types.h>`/`<linux/ioctl.h>`,
  `vk/tu_knl_kgsl.cc:8-17`, `vk/msm_kgsl.h:10-11`). Write a new
  `tu_knl_darwin.cc` that copies its structure, with the dma-buf/ION/sync_file
  parts dropped.
* Minimal kernel surface for a working (non-sparse, no external sharing)
  device: GET_INFO, CTX_CREATE/DESTROY, MEM_ALLOC/FREE, MEM_MAP (IOKit
  memory-type = BO id), SUBMIT(ctx, IB list) → timestamp, WAIT_TIMESTAMP,
  GET_RESET_STATUS. Everything else is optional and gates specific features
  (table in §3.3).
* Generic turnip code has a handful of Linux/fd assumptions outside the knl
  files that **must** be patched for Darwin (see §4.2). The two that fail
  at compile time or crash are `pthread_condattr_setclock`
  (`vk/tu_device.cc:3173`) and `close(device->local_fd)` on a non-fd
  (`vk/tu_device.cc:1896`).

---

## 1. The knl abstraction

### 1.1 Data types (`vk/tu_knl.h`)

`enum tu_bo_alloc_flags` (`vk/tu_knl.h:23-32`):

| flag | meaning for the backend |
|---|---|
| `TU_BO_ALLOC_ALLOW_DUMP` | BO may be included in GPU crash dumps / `FD_RD_DUMP`. `tu_bo_init_new_explicit_iova` stores it into `bo->dump` (`vk/tu_knl.cc:80`). KGSL ignores it. |
| `TU_BO_ALLOC_GPU_READ_ONLY` | GPU mapping must be read-only (used for the pipeline/shader suballocator, `vk/tu_device.cc:3017-3021`). CPU mapping is still RW. KGSL: `KGSL_MEMFLAGS_GPUREADONLY`. |
| `TU_BO_ALLOC_REPLAYABLE` | capture/replay: allocation must honour `client_iova` or be placed in a VA range that a replay can reproduce. KGSL: `KGSL_MEMFLAGS_USE_CPU_MAP` (SVM, GPU VA == CPU VA). |
| `TU_BO_ALLOC_INTERNAL_RESOURCE` | driver-internal; only affects RMV tracing. |
| `TU_BO_ALLOC_DMABUF` | not used by KGSL. |
| `TU_BO_ALLOC_SHAREABLE` | memory will be exported (`VkExportMemoryAllocateInfo`, `vk/tu_device.cc:3737-3741`). KGSL allocates these from dma-heap/ION instead of KGSL. |
| `TU_BO_ALLOC_IMPLICIT_SYNC` | WSI implicit sync (msm only). |

`struct tu_bo` (`vk/tu_knl.h:39-80`) — backend fills it:

* `gem_handle` – **kernel object id; also the index into the per-device
  `util_sparse_array bo_map`** (`tu_device_lookup_bo`, `vk/tu_device.h:614`,
  initialised `vk/tu_device.cc:2999`). The `tu_bo` storage lives *in* that
  array, so ids must be small, dense, non-zero integers; slot is free iff
  `gem_handle == 0` (asserted in every alloc path, e.g.
  `vk/tu_knl_kgsl.cc:309`).
* `size` – actual (page-rounded) size; `iova` – GPU VA; `map` – CPU pointer
  or NULL; `name` – from `tu_debug_bos_add()`; `refcnt` – starts at 1
  (`tu_bo_get_ref`, `vk/tu_knl.h:247-252`, used e.g. for the visibility
  stream BO shared between command buffers `vk/tu_queue.cc:204-213`).
* `never_unmap` – generic `tu_bo_unmap()` becomes a no-op
  (`vk/tu_knl.cc:149`). KGSL sets it for SVM BOs (`vk/tu_knl_kgsl.cc:210`).
* `cached_non_coherent` – set by the generic layer; makes
  `vkFlush/InvalidateMappedMemoryRanges` run userspace `DC CVAC/CIVAC`
  (`vk/tu_knl.cc:72-74,170-184`).
* `unique_id` – stable id for `VK_EXT_device_memory_report`; defaults to
  `gem_handle` (`vk/tu_knl.cc:82-83`).
* `#ifdef TU_HAS_KGSL int shared_fd` (`vk/tu_knl.h:53-59`) — backend-private
  field; a Darwin backend should add its own `#ifdef TU_HAS_DARWIN` field
  (e.g. export handle / IOSurface id).

`struct tu_sparse_vma` (`vk/tu_knl.h:82-123`) — union; for KGSL it is a
"virtual BO" (`kgsl.virtual_bo`), for msm a userspace iova range.

### 1.2 `struct tu_knl` vtable (`vk/tu_knl.h:125-178`)

All calls go through thin wrappers in `vk/tu_knl.cc` that do
`dev->instance->knl->X(...)`; **only `bo_set_metadata` and `bo_get_metadata`
are NULL-checked** (`vk/tu_knl.cc:197-198, 229-230`). Every other pointer
that can be reached must be non-NULL.

| # | member | signature (abridged) | caller / when | semantics | req? |
|---|---|---|---|---|---|
| 1 | `name` | `const char *` | `is_kgsl()` does `strcmp(name,"kgsl")` (`vk/tu_device.cc:174-177`) | backend id; string matters, see §4.2 | yes |
| 2 | `device_init` | `VkResult (tu_device*)` | `tu_CreateDevice` → `tu_drm_device_init` (`vk/tu_device.cc:2870`), before queues/BOs | set `dev->fd` (KGSL: `= physical_device->local_fd`, `vk/tu_knl_kgsl.cc:1692-1697`); msm also creates VM_BIND queue | yes |
| 3 | `device_finish` | `void (tu_device*)` | `tu_DestroyDevice` / CreateDevice failure (`vk/tu_device.cc:3290,3395`) | undo `device_init` | yes |
| 4 | `device_get_gpu_timestamp` | `int (tu_device*, uint64_t *ts)` | `vk.get_timestamp` → `vkGetCalibratedTimestampsKHR(DEVICE)` (`vk/tu_device.cc:2760-2765,2879`); Perfetto clock sync (`vk/tu_perfetto.cc:528`) | return raw 19.2 MHz always-on ticks, 0 = success. KGSL: `UNREACHABLE` (`vk/tu_knl_kgsl.cc:1705-1710`) — safe only because calibrated timestamps are exposed only when `has_persistent_counter` (a7xx gen3+, `vk/tu_device.cc:210,307`; `common/freedreno_devices.py:878`) and Perfetto is off. | yes (pointer); implement properly |
| 5 | `device_get_suspend_count` | `int (tu_device*, uint64_t*)` | Perfetto only (`vk/tu_perfetto.cc:542`) | KGSL returns 0 | yes (stub OK) |
| 6 | `device_check_status` | `VkResult (tu_device*)` | installed as `vk.check_status` (`vk/tu_device.cc:2878`); runtime calls it after waits/submits | must call `vk_device_set_lost()` and return `VK_ERROR_DEVICE_LOST` on hang/fault. KGSL queries `KGSL_PROP_GPU_RESET_STAT` per non-emulated queue (`vk/tu_knl_kgsl.cc:1720-1749`) | yes |
| 7 | `submitqueue_new` | `int (tu_device*, tu_queue*)` | `tu_queue_init` (`vk/tu_queue.cc:648`) for every non-emulated queue | create kernel context, write id to `queue->msm_queue_id`; `queue->priority` (0 = highest, `< submitqueue_priority_count`) is an input. Returns 0 / errno. | yes |
| 8 | `submitqueue_close` | `void (tu_device*, tu_queue*)` | `tu_queue_finish` | destroy context | yes |
| 9 | `bo_init` | `VkResult (dev, base, **out_bo, size, client_iova, mem_property, flags, lazy_vma, name)` | `tu_bo_init_new_explicit_iova` (`vk/tu_knl.cc:31-86`), after size is page-aligned (`:44`) and uncached→cached-coherent override (`:57-64`) | allocate memory with the caching implied by `mem_property` (§2.3), place it at `client_iova` if non-zero (fail with `VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS`), fill the `tu_bo` slot, call `tu_dump_bo_init`; if `lazy_vma` != NULL bind the new BO into that sparse VMA. Do **not** CPU-map (mapping is lazy via `bo_map`) except where the VA scheme requires it (KGSL SVM). | yes |
| 10 | `bo_init_dmabuf` | `VkResult (dev, **out_bo, size, flags, int fd)` | `vkAllocateMemory` with `VkImportMemoryFdInfoKHR` (`vk/tu_device.cc:3694-3713`), AHB import (`:3715-3725`), KGSL's own SHAREABLE path | import external memory; generic code `close()`s the caller's fd on success (`vk/tu_device.cc:3711-3713`) | only if external memory is exposed |
| 11 | `bo_export_dmabuf` | `int (dev, bo)` | `vkGetMemoryFdKHR` (`vk/tu_device.cc:4387-4402`) | return new fd or <0 | only if external memory exposed |
| 12 | `bo_alloc_lazy` | `VkResult (dev, bo)` | **never called, no backend sets it** (dead member) | — | no |
| 13 | `bo_map` | `VkResult (dev, bo, void *placed_addr)` | `tu_bo_map` (`vk/tu_knl.cc:134-144`) when `bo->map == NULL` | create CPU mapping; if `placed_addr` != NULL map exactly there (VK_EXT_map_memory_placed, `MAP_FIXED` in KGSL `vk/tu_knl_kgsl.cc:418-424`); set `bo->map` | yes |
| — | *(unmap)* | — | `tu_bo_unmap` is **generic**, calls `munmap`/`mmap(PROT_NONE, MAP_FIXED)` directly (`vk/tu_knl.cc:146-167`) | no hook! See §4.2 | — |
| 14 | `bo_allow_dump` | `void (dev, bo)` | `tu_bo_allow_dump` (`vk/tu_knl.cc:186-191`) — unconditionally from `vkBindBufferMemory2` for descriptor-buffer usage (`vk/tu_buffer.cc:206-211`) | mark BO for crash dumps; KGSL no-op | yes (no-op OK) |
| 15 | `bo_finish` | `void (dev, bo)` | `tu_bo_finish` (`vk/tu_knl.cc:122-132`) | **refcounted**: return unless `p_atomic_dec_zero(&bo->refcnt)`; then `tu_debug_bos_del`, `tu_dump_bo_del`, unmap, `memset(bo,0)` *before* freeing the kernel id (so a racing allocation reusing the id finds a clean slot), free (`vk/tu_knl_kgsl.cc:441-470`) | yes |
| 16 | `bo_set_metadata` | `void (dev, bo, void*, u32)` | `vkGetMemoryFdKHR` for image memory (`vk/tu_device.cc:4408-4424`) | attach `fdl_metadata` (modifier) for cross-process sharing | optional (NULL) |
| 17 | `bo_get_metadata` | `int (dev, bo, void*, u32)` | import paths | | optional (NULL → -ENOSYS) |
| 18 | `submit_create` | `void *(dev)` | `queue_submit` / `queue_submit_sparse` (`vk/tu_queue.cc:347,462`) | allocate opaque submit builder | yes |
| 19 | `submit_finish` | `void (dev, void*)` | end of both submit paths | free builder | yes |
| 20 | `submit_add_entries` | `void (dev, void*, tu_cs_entry*, n)` | many times per submit (`vk/tu_queue.cc:75-85`): perf-pass IB, each cmdbuf's IBs, timestamp-copy CS, autotune CS, vis-stream patch CS | append IBs; each entry = `{bo, offset, size(bytes)}` → GPU address `bo->iova + offset` | yes |
| 21 | `submit_add_bind` | `void (dev, void*, vma, vma_off, bo|NULL, bo_off, size)` | `queue_submit_sparse` (`vk/tu_queue.cc:340-411`) | record sparse bind/unbind (4 KiB aligned, asserted `vk/tu_knl.cc:315-316`) | only if `has_sparse` |
| 22 | `queue_submit` | `VkResult (queue, void*, waits, nwait, signals, nsignal, u_trace_data)` | `tu_queue_submit` from both submit paths, **with `device->submit_mutex` held**, followed by `pthread_cond_broadcast(&device->timeline_cond)` (`vk/tu_queue.cc:557-592`) | kick the GPU; wait on `waits[]` (backend sync type), make `signals[]` signal on completion, update `queue->fence` with the new timestamp; must handle the zero-IB case (§2.5) | yes |
| 23 | `queue_wait_fence` | `VkResult (queue, uint32 fence, uint64 timeout_ns)` | u_trace timestamp readback (`vk/tu_device.cc:2318`), `FD_RD_DUMP=full` (`vk/tu_queue.cc:512`) | block until `queue`'s timestamp `fence` retires; relative timeout | yes |
| 24 | `sparse_vma_init` | `VkResult (dev, base, out_vma, *out_iova, flags, size, client_iova)` | sparse `VkBuffer`/`VkImage` create (`vk/tu_buffer.cc:60`, `vk/tu_image.cc:1006`); `LAZILY_ALLOCATED` memory (`vk/tu_device.cc:3529`) | reserve a GPU VA range (optionally backed by the PRR zero page) | only if `has_sparse` or `has_lazy_bos` |
| 25 | `sparse_vma_finish` | `void (dev, vma)` | destroy of the above | | same |
| 26 | `device_entrypoints` | `const vk_device_entrypoint_table *` | merged last into the device dispatch table (`vk/tu_device.cc:2851-2856`) | backend can override any `vk*` device entrypoint; **no backend uses it today** | optional |

Other declarations in `tu_knl.h` that are *not* vtable members:
`tu_device_wait_u_trace` (declared `vk/tu_knl.h:295-296`, unused),
`struct tu_zombie_vma` (msm userspace-VA recycling only).

### 1.3 Backend helper contract (things `bo_init`/`bo_finish` must call)

* `tu_device_lookup_bo(dev, id)` → slot to fill (`vk/tu_device.h:614`).
* `tu_debug_bos_add(dev, size, name)` / `tu_debug_bos_del` (`vk/tu_device.cc:4472,4500`).
* `tu_dump_bo_init` / `tu_dump_bo_del` (`vk/tu_device.h:703`) — maintains
  `dev->dump_bo_list` used by `FD_RD_DUMP`.
* `TU_RMV(bo_allocate|bo_map|bo_unmap|bo_destroy|…)` macros (no-ops unless RMV
  tracing).

### 1.4 Physical-device enumeration and runtime selection

1. `tu_CreateInstance` installs two hooks (`vk/tu_device.cc:1973-1975`):
   `physical_devices.enumerate = tu_enumerate_devices` and
   `physical_devices.try_create_for_drm = tu_physical_device_try_create`.
2. The common runtime (`src/vulkan/runtime/vk_instance.c:445-462`) calls
   `enumerate` first; **only if it returns `VK_ERROR_INCOMPATIBLE_DRIVER`**
   does it fall back to libdrm `drmGetDevices2` + `try_create_for_drm`. Without
   libdrm (`HAVE_LIBDRM` undefined), `util/libdrm.h` provides stubs that
   return no devices (`src/util/libdrm.h:33-36,114,159`), so the DRM path is
   harmless on Darwin.
3. `tu_enumerate_devices` (`vk/tu_knl.cc:336-368`) is the non-DRM path; today
   it is `#ifdef TU_HAS_KGSL` only: `open("/dev/kgsl-3d0", O_RDWR|O_CLOEXEC)`,
   ENOENT → INCOMPATIBLE, else `tu_knl_kgsl_load(instance, fd)`. **A Darwin
   backend adds an `#elif TU_HAS_DARWIN` branch here** (IOServiceGetMatching
   Service + IOServiceOpen).
4. `tu_physical_device_try_create` (`vk/tu_knl.cc:373-493`) is the DRM path:
   opens the render node, `drmGetVersion`, dispatches on `version->name`
   (`"msm"` → `tu_knl_drm_msm_load`, `"virtio_gpu"` → virtio), then fills
   `master_fd`, `kgsl_dma_fd = -1`, stat()-derived major/minor, and calls
   `tu_physical_device_init`.
5. A `*_load()` function must (KGSL template `vk/tu_knl_kgsl.cc:1788-1965`):
   `vk_zalloc` a `tu_physical_device`, probe the kernel, fill the fields in
   §1.5, set **`instance->knl = &funcs`**, call
   `tu_physical_device_init(device, instance)`, then
   `list_addtail(&device->vk.link, &instance->vk.physical_devices.list)`
   (the DRM path instead returns `*out` and the runtime adds it).

### 1.5 `tu_physical_device` fields a backend must set before `tu_physical_device_init`

(`vk/tu_device.h:87-187`; consumed in `tu_physical_device_init`,
`vk/tu_device.cc:1673-1887`)

| field | KGSL source | notes for Darwin |
|---|---|---|
| `instance`, `local_fd` | open fd | `local_fd` is **`close()`d unconditionally** in `tu_physical_device_finish` (`vk/tu_device.cc:1896`). Either keep a real fd or patch. |
| `master_fd = -1`, `kgsl_dma_fd = -1` | | `vk_zalloc` leaves them 0 → `close(0)` at finish (`vk/tu_device.cc:1897-1901`). KGSL sets `master_fd=-1` (`vk/tu_knl_kgsl.cc:1857`) and `kgsl_dma_fd` to the heap fd or -1. |
| `dev_id.gpu_id`, `dev_id.chip_id` | from `KGSL_PROP_DEVICE_INFO` (§2.6) | lookup key into the device table; unknown → `VK_ERROR_INCOMPATIBLE_DRIVER` (`vk/tu_device.cc:1678-1684`) |
| `gmem_size`, `gmem_base` | `kgsl_devinfo.gmem_sizebytes`, `KGSL_PROP_UCHE_GMEM_VADDR` | must equal what the kernel programs in `UCHE_GMEM_RANGE_*`; overridable by `TU_GMEM` env |
| `uche_trap_base` | `KGSL_PROP_UCHE_TRAP_BASE`, fallback `0x1fffffffff000` (`vk/tu_knl_kgsl.cc:1848-1852`) | used by ir3 bounds lowering (`ir3/ir3_nir.c:598-606`) and hashed into the cache UUID (`vk/tu_device.cc:103`) |
| `ubwc_config.{highest_bank_bit, bank_swizzle_levels, macrotile_mode}` | `KGSL_PROP_HIGHEST_BANK_BIT`, `KGSL_PROP_UBWC_MODE` mapped by a switch (`vk/tu_knl_kgsl.cc:1908-1947`) | or leave `highest_bank_bit=0`, `bank_swizzle_levels=~0`, `macrotile_mode=FDL_MACROTILE_INVALID` to get table defaults (`vk/tu_device.cc:1802-1812`) |
| `has_raytracing` | `KGSL_PROP_IS_RAYTRACING_ENABLED` | a7xx only; false on A650 |
| `submitqueue_priority_count` | 1 (KGSL ignores priority) | 1–4 (`vk/tu_device.cc:2016-2050`) |
| `sync_types[0..2]`, `timeline_type` | `{&vk_kgsl_sync_type, &timeline_type.sync, NULL}` with `vk_sync_timeline_get_type()` (`vk/tu_knl_kgsl.cc:1873-1877`) | emulated timelines ⇒ runtime uses `VK_QUEUE_SUBMIT_MODE_DEFERRED` (`src/vulkan/runtime/vk_device.c:49-70,224-234`) |
| `heap.{size,used,flags}` | `tu_get_system_heap_size()` (`vk/tu_device.cc:2101-2112`) = % of `os_get_total_physical_memory()` capped by `va_size` | works on Darwin (sysctl) |
| `has_set_iova` | probe `KGSL_MEMFLAGS_USE_CPU_MAP` | gates capture/replay features; also makes the generic code create a `util_vma_heap` from `va_start/va_size` (`vk/tu_device.cc:2905-2909`) |
| `has_cached_coherent_memory` | probe IOCOHERENT\|WRITEBACK alloc | adds a HOST_CACHED+COHERENT memory type |
| `has_sparse`, `has_sparse_prr`, `has_lazy_bos` | `kgsl_is_virtual_bo_supported` (`vk/tu_knl_kgsl.cc:598-656,1891-1893`) | false ⇒ no sparse queue family, no lazily-allocated type |
| `va_start`, `va_size` | fake `0x100000000`, `KGSL_PROP_GPU_VA64_SIZE` | `va_size` also caps heap and is `sparseAddressSpaceSize` (`vk/tu_device.cc:1172`) |
| `has_preemption` | hard-coded true (`vk/tu_knl_kgsl.cc:1902-1903`) | on A6XX makes binning re-emit VSC state per bin (`vk/tu_cmd_buffer.cc:1551-1559`); **true is the safe value** |
| `is_perf_cntr_selectable` | false (`vk/tu_knl_kgsl.cc:1905-1906`) | true only if the kernel lets the CP write `*_PERFCTR_*_SEL` regs (CP_PROTECT); gates `KHR_performance_query` (`vk/tu_device.cc:255`) |
| `has_vm_bind`, `msm_*_version`, `syncobj_type` | untouched (0) | msm-only |

`tu_physical_device_init` additionally needs a **cache UUID**: it hashes a
build id obtained from `disk_cache_get_function_identifier()` and fails the
whole device with "cannot generate UUID" if that is unavailable
(`vk/tu_device.cc:58-74,1749-1753`). Use `-Dtu-build-id=<hex>` on Darwin
(§4.3).

### 1.6 Build-time selection

* Option: `freedreno-kmds`, array, default `['msm']`, choices
  `msm, kgsl, virtio, wsl` (`meson.options:220-228`). (This tree uses
  `meson.options`; there is no `meson_options.txt`.)
* `src/freedreno/vulkan/meson.build:130-150`:
  `kgsl` → `-DTU_HAS_KGSL` + `tu_knl_kgsl.cc`;
  `msm` → `-DTU_HAS_MSM` + `tu_knl_drm_msm.cc tu_knl_drm.cc` + `dep_libdrm`;
  `virtio` → `-DTU_HAS_VIRTIO` + `tu_knl_drm_virtio.cc tu_knl_drm.cc` +
  libvdrm + `dep_libdrm`. `tu_knl.cc` is always built (`:50`).
* Root `meson.build:327-340`: if kmds ≠ `['msm']` and turnip is the only
  Vulkan driver and no gallium DRM driver, `system_has_kms_drm` is forced
  false so libdrm is not linked. On Darwin it is already false
  (`meson.build:162`), so `dep_libdrm = null_dep` and `HAVE_LIBDRM` is unset
  (`meson.build:1920-1940`).
* `src/freedreno/meson.build`: `drm/` (libfreedreno_drm, gallium only) is
  built only if `system_has_kms_drm`; `common/ ir3/ fdl/ perfcntrs/
  registers/ isa/` always.
* `TU_HAS_*` is otherwise referenced only in `tu_knl.h` (`#ifdef` struct
  fields), `tu_knl.cc` (enumeration), `tu_device.h:459` (virtio).
* **To add a backend:** add `'darwin'` to the choices in `meson.options`, a
  `freedreno_kmds.contains('darwin')` block in `vk/meson.build` adding
  `-DTU_HAS_DARWIN` + `tu_knl_darwin.cc` + `dependency('appleframeworks',
  modules: ['IOKit','CoreFoundation'])`, the enumeration branch in
  `tu_knl.cc`, and the `tu_knl_darwin_load()` prototype in `tu_knl.h`.

---

## 2. The KGSL backend in detail (`vk/tu_knl_kgsl.cc`)

All ioctls go through `safe_ioctl()` which retries on `EINTR`/`EAGAIN`
(`:38-48`). UAPI header: `vk/msm_kgsl.h` (KGSL 3.14, `:20-21`).

### 2.1 Every kernel interaction

| interaction | where | struct fields set → read | purpose |
|---|---|---|---|
| `open("/dev/kgsl-3d0", O_RDWR\|O_CLOEXEC)` | `tu_knl.cc:343-353` | | device fd (`local_fd`, `dev->fd`) |
| `open("/dev/dma_heap/system")` else `open("/dev/ion")` + `ION_IOC_FREE(handle 0)` probe | `:1804-1826` | | allocator for **exportable** memory only; failure just logs and disables `external_memory_fd` |
| `IOCTL_KGSL_DEVICE_GETPROPERTY` (`msm_kgsl.h:561-570`) | `get_kgsl_prop` `:557-569` | `{type, value*, sizebytes}` | see §2.6 |
| `IOCTL_KGSL_DRAWCTXT_CREATE` (`:664`) | `kgsl_submitqueue_new :50-66` | `flags = SAVE_GMEM \| NO_GMEM_ALLOC \| PREAMBLE` → `drawctxt_id` | one context per VkQueue |
| `IOCTL_KGSL_DRAWCTXT_DESTROY` (`:672`) | `:68-76` | `drawctxt_id` | |
| `IOCTL_KGSL_GPUMEM_ALLOC_ID` (`:918`) | `kgsl_bo_init :279-306`; probes `:571-596,619` | `size, flags` → `id, flags(actual), size, mmapsize, gpuaddr` | normal allocation; kernel picks GPU VA |
| `IOCTL_KGSL_GPUMEM_FREE_ID` (`:935`) | `kgsl_bo_finish :462-469` | `id` | |
| `mmap(fd, offset = id << 12)` | `kgsl_bo_map :412-434`, SVM `kgsl_bo_user_map :184-213` | | CPU mapping; id encoded in page offset |
| `mmap(dmabuf_fd, 0)` | `:421-425` | | CPU map of imported/exportable BOs |
| `IOCTL_KGSL_GPUOBJ_IMPORT` (`:1475`) type `KGSL_USER_MEM_TYPE_DMABUF` | `kgsl_bo_init_dmabuf :345-367` | `{priv=&{fd}, priv_len, flags=0, type}` → `id` | import dma-buf |
| `IOCTL_KGSL_GPUOBJ_INFO` (`:1440`) | `:369-377` | `id` → `gpuaddr, size` | size/VA of import |
| `fstat(fd).st_ino` | `:391-396` | | `unique_id = ino \| 1<<63` |
| `IOCTL_KGSL_GPUOBJ_ALLOC` (`:1374`) with `KGSL_MEMFLAGS_VBO[_NO_MAP_ZERO]`, `USE_CPU_MAP` | `kgsl_sparse_vma_init :472-541` | `size, flags, va_len=0` → `id, mmapsize` | "virtual BO" = sparse VA range (GPUMEM_ALLOC_ID has only 32-bit flags, VBO is bit 34, `:480-481`) |
| `IOCTL_KGSL_GPUMEM_GET_INFO` (`:964`) | `:524-535` | `id` → `gpuaddr` | VBO alloc doesn't return the VA |
| `IOCTL_KGSL_GPUOBJ_FREE` (`:1419`) | `:543-555` | `id` | |
| `IOCTL_KGSL_GPUMEM_BIND_RANGES` (`:1815`) | lazy BO → VBO bind `:215-246`, probe `:640` | `{ranges*, nents, size, id=vbo, flags}`, range `{child_offset, target_offset, length, child_id, op=BIND}` | synchronous bind |
| `IOCTL_KGSL_GPU_COMMAND` (`:1567`) | `kgsl_queue_submit :1549-1572` | see §2.4 → `timestamp` | submit IBs |
| `IOCTL_KGSL_GPU_AUX_COMMAND` (`:1907`) | `:1573-1613` | `KGSL_GPU_AUX_COMMAND_BIND [\| SYNC]`, one bind cmd per ioctl → `timestamp` | queued sparse binds |
| `IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID` (`:592`) | `wait_timestamp_safe :784-814` | `{context_id, timestamp, timeout(ms)}` | CPU wait; on EINTR recomputes remaining time; any other error is treated as `VK_TIMEOUT` (asserts `ETIMEDOUT`) |
| `IOCTL_KGSL_TIMESTAMP_EVENT` (`:886`) `KGSL_TIMESTAMP_EVENT_FENCE` | `timestamp_to_fd :716-733` | `{type, timestamp, context_id, priv=&fd, len}` → sync_file fd | TS → sync_file |
| `sync_wait`, `sync_merge`, `poll` (util/libsync.h) | `:875-883, 1051-1063, 976-991` | | sync_file waits/merges |
| `IOCTL_KGSL_PERFCOUNTER_READ` (`:1106`) group `KGSL_PERFCOUNTER_GROUP_ALWAYSON` | `:1623-1636`, `HAVE_PERFETTO` only | → `value` (low 32 bits valid on old KGSL) | CPU/GPU clock correlation |
| `KGSL_PROP_GPU_RESET_STAT` | `kgsl_device_check_status :1720-1749` | in: ctx id, out: `KGSL_CTX_STAT_*` (`msm_kgsl.h:257-260`) | device-lost detection |
| `DMA_HEAP_IOCTL_ALLOC` / `ION_IOC_NEW_ALLOC` / `ION_IOC_ALLOC+SHARE+FREE` | `:80-182` | heap mask `1<<25` (system heap) | exportable allocations, then imported via GPUOBJ_IMPORT |

Present in the header but **not used**: `IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID` (`msm_kgsl.h:698`),
`GPUMEM_SYNC_CACHE` (`:1001`), `PERFCOUNTER_GET` (`:1029`), `GPUOBJ_SYNC` (`:1506`),
`SETPROPERTY`. Turnip never asks the kernel for cache maintenance (it uses `DC` instructions,
`src/util/cache_ops_aarch64.c:99-112`) and never polls a timestamp except via a
0-timeout wait.

### 2.2 Context creation

* Flags (`vk/tu_knl_kgsl.cc:53-57`): `KGSL_CONTEXT_SAVE_GMEM` (kernel
  saves/restores GMEM across preemption/context switch),
  `KGSL_CONTEXT_NO_GMEM_ALLOC`, `KGSL_CONTEXT_PREAMBLE` (lets KGSL skip IBs
  flagged `KGSL_CMDLIST_CTXTSWITCH_PREAMBLE` when no switch occurred — turnip
  never flags any IB that way, it only uses `KGSL_CMDLIST_IB`, so nothing is
  ever skipped; `msm_kgsl.h:120-122`).
* **Not set:** priority (`KGSL_CONTEXT_PRIORITY_MASK`), preemption style
  (`KGSL_CONTEXT_PREEMPT_STYLE_*`), context type (`KGSL_CONTEXT_TYPE_VK`),
  `KGSL_CONTEXT_SECURE` (`msm_kgsl.h:52-75`). `submitqueue_priority_count = 1`
  so only `VK_QUEUE_GLOBAL_PRIORITY_MEDIUM` is advertised.
* `has_preemption = true` unconditionally — KGSL always preempts.
* Contexts per device: 1 GFX queue (`queueCount = 1`, `vk/tu_device.cc:1648-1654`),
  plus 1 SPARSE queue family if `has_sparse` (`:1842-1849`). The drirc
  `emulate_second_queue` alias shares the first queue's context.

### 2.3 BO allocation

Normal path (`kgsl_bo_init`, `:248-343`):

* **SHAREABLE** → allocate from dma-heap/ION, then import (§2.1); rejects a
  fixed `client_iova` (`:259-277`). Result has `shared_fd` (used for both
  CPU mmap and export).
* Otherwise `GPUMEM_ALLOC_ID` with flags derived from the Vulkan memory type:
  * `HOST_CACHED && HOST_COHERENT` → `KGSL_MEMFLAGS_IOCOHERENT |
    CACHEMODE_WRITEBACK` (CPU-cached, GPU snoops via SMMU io-coherency);
  * `HOST_CACHED` only → `CACHEMODE_WRITEBACK` (CPU-cached, non-coherent;
    turnip flushes/invalidates with `DC CVAC/CIVAC`);
  * else → `CACHEMODE_WRITECOMBINE` (default type: device-local,
    host-visible, host-coherent, `vk/tu_device.cc:1763-1767`);
  * `GPU_READ_ONLY` → `KGSL_MEMFLAGS_GPUREADONLY`;
  * `REPLAYABLE` → `KGSL_MEMFLAGS_USE_CPU_MAP` (`msm_kgsl.h:136-161` for bits).
* The kernel returns `gpuaddr` (kernel-assigned VA), `mmapsize` (may be >
  requested; stored as `bo->size`), and echoes the *actual* flags — turnip's
  capability probes depend on that echo (`:584-589`).
* Alignment: generic code page-aligns sizes (`os_page_size`,
  `vk/tu_knl.cc:44,96,210`); no explicit alignment flag is passed (KGSL
  `KGSL_MEMALIGN_*` hints unused). Images may need larger alignment —
  `tu_image.cc:1151` aligns to `os_page_size`; the kernel's VA allocator must
  give at least page alignment (4 KiB on GPU side; turnip doesn't require
  64 KiB VA alignment for non-sparse BOs).
* **VA handling:** KGSL never lets userspace pick a GPU VA. The only exception
  is SVM: with `USE_CPU_MAP` the GPU VA is assigned *by mmap*, i.e. GPU VA ==
  CPU VA (`msm_kgsl.h:910-912`; `kgsl_bo_user_map :184-213` mmaps with
  `client_iova` as a hint, checks the result, sets `iova = map`, and marks
  `never_unmap`). That is how capture/replay (`has_set_iova`) works on KGSL.
  `va_start` is a dummy `0x100000000` just so the generic `util_vma_heap`
  init doesn't assert (`:1896-1899`).
* **No residency/BO list:** KGSL keeps every allocation mapped in the
  process's GPU pagetable for its whole life; submits reference only IBs. A
  Darwin kernel must do the same.
* **Secure/protected memory:** never used (`protectedMemory = false`,
  `vk/tu_device.cc:496`).
* **Sparse / lazy:** via "virtual BOs" (VBO) and `GPUMEM_BIND_RANGES` /
  `GPU_AUX_COMMAND_BIND` — there is no VM_BIND. Lazy BOs on KGSL are
  implemented on top of the same VBO mechanism, so `has_lazy_bos =
  has_sparse` (`:1893`). Unbind of a range in a VBO without `NO_MAP_ZERO`
  maps the PRR zero page (`:489-493`, `vk/tu_knl.h:86-92`).

### 2.4 CPU mapping

* Non-shared BO: `mmap(placed_addr, size, RW, MAP_SHARED|[MAP_FIXED],
  kgsl_fd, id << 12)` (`:412-434`). Mapping is created lazily on the first
  `tu_bo_map` and kept until `bo_finish` or `vkUnmapMemory`.
* Imported/exportable BO: `mmap` on the dma-buf fd.
* Caching attributes come from the allocation flags (WC vs WB), not from the
  mmap call.
* `vkUnmapMemory2(RESERVE)` replaces the mapping with an anonymous
  `PROT_NONE` `MAP_FIXED` mapping (generic `tu_bo_unmap`,
  `vk/tu_knl.cc:154-159`).

### 2.5 Submission

Builder (`struct tu_kgsl_queue_submit`, `:1272-1278`):
`commands` = array of `kgsl_command_object`, `ranges` + `bind_cmds` for sparse.

* `submit_add_entries` (`:1300-1319`): one `kgsl_command_object` per IB:
  `{gpuaddr = bo->iova + offset, size = bytes, flags = KGSL_CMDLIST_IB, id =
  bo->gem_handle}`. `offset` field unused for IBs.
* `queue_submit` (`:1389-1690`):
  1. **Empty submit** (no IBs, no binds; e.g. `vkQueueSubmit` with only
     semaphores): KGSL will not advance the timeline for a zero-IB submit
     but would still return a new timestamp that never signals (`:1402-1408`).
     Turnip therefore merges `waits[] + last queue fence` in userspace into one
     `kgsl_syncobj` and copies it into every signal object; no ioctl
     (`:1402-1461`). Your kernel either does the same or must accept
     zero-IB submits and signal them.
  2. Optional profiling: allocate a `kgsl_cmdbatch_profiling_buffer`
     (`msm_kgsl.h:541-547`) from `kgsl_profiling_suballoc`
     (`vk/tu_device.cc:3024-3028`) and pass it in `objlist` with
     `KGSL_OBJLIST_MEMOBJ | KGSL_OBJLIST_PROFILE` and
     `KGSL_CMDBATCH_PROFILING` (`:1468-1500,1562-1567`). Only when u_trace is
     active.
  3. Merge all wait syncobjs (`kgsl_syncobj_merge`, `:1065-1129`) into at most
     **one** syncpoint: `KGSL_CMD_SYNCPOINT_TYPE_TIMESTAMP {context_id,
     timestamp}` or `..._FENCE {fd}` (`:1514-1543`).
  4. `IOCTL_KGSL_GPU_COMMAND {flags = KGSL_CMDBATCH_SUBMIT_IB_LIST, cmdlist,
     cmdsize, numcmds, synclist, syncsize, numsyncs, context_id}` → `timestamp`
     (`:1549-1572`). For sparse binds, one `GPU_AUX_COMMAND` per target VBO,
     the sync attached to the first (`:1573-1613`).
  5. Failure → `vk_device_set_lost` (`:1603-1607, 1656-1660`).
  6. `queue->fence = timestamp`; every signal syncobj becomes
     `{STATE_TS, queue, timestamp}` (`:1662-1673`).
* Locking: caller holds `device->submit_mutex`; after the call the caller
  broadcasts `device->timeline_cond` (`vk/tu_queue.cc:589-592`).

### 2.6 Device info / chip id discovery (A650)

`tu_knl_kgsl_load` (`:1788-1965`) issues, in order:

| property | struct | used for |
|---|---|---|
| `KGSL_PROP_DEVICE_INFO` (0x1) | `kgsl_devinfo {device_id, chip_id, mmu_enabled, gmem_gpubaseaddr, gpu_id, gmem_sizebytes}` (`msm_kgsl.h:266-282`) | chip id, gmem size (fatal on failure) |
| `KGSL_PROP_UCHE_GMEM_VADDR` (0x13) | u64 | `gmem_base` (fatal) |
| `KGSL_PROP_HIGHEST_BANK_BIT` (0x17) | u32 | UBWC (fatal) |
| `KGSL_PROP_UBWC_MODE` (0x1B) | u32 `KGSL_UBWC_x_0` | UBWC swizzle/macrotile (fatal; unknown → fail) |
| `KGSL_PROP_UCHE_TRAP_BASE` (0x2F) | u64 | optional, default `0x1fffffffff000` |
| `KGSL_PROP_IS_RAYTRACING_ENABLED` (0x2D) | u32 | optional |
| `KGSL_PROP_GPU_VA64_SIZE` (0x2C) | u64 | optional `va_size` |

then three allocation probes (`USE_CPU_MAP`, `IOCOHERENT|WRITEBACK`, VBO+bind).

`fd_dev_id` filling (`:1861-1865`):
```
gpu_id  = ((chip_id >> 24) & 0xff) * 100 + ((chip_id >> 16) & 0xff) * 10 + ((chip_id >> 8) & 0xff);
chip_id = info.chip_id;          // 32-bit, upper fuse bits 0
```
For SM8250: `chip_id = 0x06050002` → `gpu_id = 6*100 + 5*10 + 0 = 650`.
(`kgsl_devinfo.gpu_id` is ignored.) Lookup in `common/freedreno_dev_info.c:22-79`:
because `gpu_id != 0`, the match is by gpu_id against the table entry
`GPUId(650)` (`common/freedreno_devices.py:543-571`; `GPUId(650)` expands to
`chip_id 0x060500ff`, `common/freedreno_dev_info.py:65-83`). Passing only
`chip_id = 0x06050002` also matches via the `0xff` patch wildcard. Name
`"FD650"` → `"Turnip Adreno (TM) 650"` (`vk/tu_device.cc:1696-1706`).
A650 = `a6xx_base + a6xx_gen3`, 3 CCUs, `highest_bank_bit = 16`, default
`ubwc_swizzle 0x6`, `macrotile_mode 1` (8-channel).

Values the Darwin kernel should report for SM8250/A650 (cross-checked
against upstream Linux msm `drivers/gpu/drm/msm/adreno/`):
`chip_id 0x06050002`, `gmem_size = 1 MiB + 128 KiB` (`a6xx_catalog.c`
A650 entry `.gmem = SZ_1M + SZ_128K`), `gmem_base = 0` (msm reports 0 for
`family >= ADRENO_6XX_GEN3`, `adreno_gpu.c` `MSM_PARAM_GMEM_BASE`; whatever
you program in `UCHE_GMEM_RANGE_MIN` is what must be reported),
`uche_trap_base = 0x1fffffffff000` (`a6xx_gpu.c`), UBWC 4.0 / HBB 16
(`soc/qcom/ubwc_config.c`: `qcom,sm8250` → `ubwc_4_0_hbb16`), which turnip's
KGSL mapping turns into `bank_swizzle_levels 0x6`, `FDL_MACROTILE_8_CHANNEL`
— identical to the table defaults.

### 2.7 Synchronization without DRM syncobj

`struct kgsl_syncobj` (`:658-674`) is a userspace state machine:

| state | meaning |
|---|---|
| `UNSIGNALED` | not yet submitted (wait-before-signal) |
| `SIGNALED` | trivially signalled |
| `TS` | `{queue, timestamp}` — signals when that context's timestamp retires |
| `FD` | holds a sync_file fd (imported or produced by merge) |

`vk_kgsl_sync_type` (`:1252-1270`): features `BINARY | GPU_WAIT |
GPU_MULTI_WAIT | CPU_WAIT | CPU_RESET | WAIT_ANY | WAIT_PENDING`; callbacks
`init/finish/reset/move/wait/wait_many/import_sync_file/export_sync_file`.
Timelines are the runtime's `vk_sync_timeline` emulation on top.

* **CPU wait** (`kgsl_syncobj_wait`, `:826-888`): `UNSIGNALED` → sleep on
  `device->timeline_cond` under `submit_mutex` until a submit turns it into
  TS/FD (that's why every submit broadcasts); `TS` →
  `WAITTIMESTAMP_CTXTID`; `FD` → `sync_wait()`.
* **Wait-any** (`:894-1006`): TS objects from one queue collapse to the
  lowest timestamp; TS from different queues or any FD → convert TS to
  sync_file via `TIMESTAMP_EVENT` and `poll()`.
* **GPU wait**: merged into a single in-submit syncpoint (§2.5).
* **Timestamp comparison** is wrap-safe 32-bit (`(int32_t)(a-b) >= 0`,
  `:742-761`).

**Mandatory vs optional** for a Darwin port:

* *Mandatory:* per-context monotonically increasing 32-bit timestamp returned
  by SUBMIT; CPU `WAIT_TIMESTAMP(ctx, ts, timeout)`; the `UNSIGNALED` →
  condvar logic; the empty-submit merge.
* *Effectively optional with one queue:* the in-submit syncpoint. With
  `has_sparse = false` there is exactly one kernel context per VkDevice, and
  binary semaphores cannot cross VkDevices without external handles, so every
  `TS` wait is on the same context and already ordered by the ring. Still
  cheap to support (kernel-side "wait for ctx X ts N before running").
* *Optional:* everything fd-based — `TIMESTAMP_EVENT`, `sync_wait`,
  `sync_merge`, `poll`. It is only reached for (a) imported sync FDs, (b)
  `vkGetSemaphoreFdKHR/vkGetFenceFdKHR(SYNC_FD)`, (c) TS from **different**
  queues (only with a sparse queue). If the Darwin sync type leaves
  `import_sync_file`/`export_sync_file` NULL, the runtime advertises no
  SYNC_FD handle types (`src/vulkan/runtime/vk_semaphore.c:47-72`,
  `vk_fence.c:47-61`) — conformant.

### 2.8 Timestamps, queries, perf counters

* GPU timestamps for queries and u_trace are written **by the GPU itself**
  (`CP_REG_TO_MEM` of `CP_ALWAYS_ON_COUNTER`, `vk/tu_query_pool.cc:2113`),
  19.2 MHz (`vk/tu_device.cc:1147-1148,1270,2212-2218`). No kernel call.
  The kernel/GMU must keep the always-on counter running.
* Occlusion, pipeline-statistics, transform-feedback, primitives-generated
  queries: GPU-written into BOs; no kernel support needed.
* `vkGetCalibratedTimestamps(DEVICE)` → `device_get_gpu_timestamp`; not
  exposed on A650 (needs `has_persistent_counter`).
* Perfetto/u_trace CPU↔GPU correlation uses the KGSL profiling buffer and
  `PERFCOUNTER_READ(ALWAYSON)` (`:1615-1652`). Optional.
* `KHR_performance_query`: requires `TU_DEBUG=perfc|perfcraw` **and**
  `is_perf_cntr_selectable` (`vk/tu_device.cc:255`); KGSL sets it false
  because KGSL CP-protects the counter select registers. The perfcounter
  "reservation" library `fd_perfcntr_state_alloc` is given fd -1 on KGSL
  (`vk/tu_device.cc:3143-3145`) and without libdrm does nothing kernel-side
  (`perfcntrs/freedreno_perfcntr.c:94-105`). Multi-pass perf queries switch
  passes with `CP_SCRATCH_REG(4)` (`vk/tu_query_pool.h:18`,
  `vk/tu_device.cc:3134-3136`); autotune uses `CP_SCRATCH_REG(5)` — the
  kernel must not use scratch regs 4/5.

---

## 3. Minimal kernel UAPI for a KGSL-style "sweet" Darwin backend

### 3.1 Transport mapping

* Open: `IOServiceGetMatchingService(kIOMainPortDefault,
  IOServiceMatching("AdrenoGPU"))` → `IOServiceOpen(svc, mach_task_self(),
  type, &connect)`. Store `connect` in a backend field (don't put it in
  `local_fd`, see §4.2).
* Commands: `IOConnectCallMethod(connect, selector, scalars_in, n,
  struct_in, len, scalars_out, &n, struct_out, &len)`. Struct input > 4096
  bytes goes out-of-line (kernel gets an `IOMemoryDescriptor`) — matters for
  SUBMIT with many IBs (32 B each ⇒ >127 IBs). Alternatively pass
  `(user_ptr, count)` as scalars and `IOMemoryDescriptor::withAddressRange`
  in the kernel.
* CPU map: `IOConnectMapMemory64(connect, memoryType = bo_id, mach_task_self(),
  &addr, &size, kIOMapAnywhere | cache)` where the user client's
  `clientMemoryForType(type)` returns the BO's memory descriptor. Unmap with
  `IOConnectUnmapMemory64` (not `munmap`). Cache flags:
  `kIOMapWriteCombineCache` for WC, `kIOMapDefaultCache` for WB.
* Blocking waits: the external method sleeps (`IOCommandGate::commandSleep`
  with deadline); no EINTR semantics needed, so the retry logic of
  `wait_timestamp_safe` goes away.

### 3.2 Operations

Required for a functional device (no sparse, no sharing):

| selector | in | out | turnip consumer |
|---|---|---|---|
| `GET_INFO` | — | `chip_id (u64)`, `gmem_size`, `gmem_base`, `uche_trap_base`, `highest_bank_bit`, `ubwc_version`, `va_start`, `va_size`, `caps` bitmask (IOCOHERENT, SVM/FIXED_VA, SPARSE, PRR, PERFCNTR_SELECTABLE, RESET_STAT, TIMESTAMP_EVENT, PROFILING), `num_priorities`, `max_ibs_per_submit` | `_load()` (§1.5) |
| `CTX_CREATE` | `flags` (preemption allowed, save-GMEM), `priority` | `ctx_id` | `submitqueue_new` |
| `CTX_DESTROY` | `ctx_id` | — | `submitqueue_close` |
| `MEM_ALLOC` | `size`, `flags` {cache: WC \| WB, io_coherent, gpu_readonly, shareable} | `id` (non-zero, small, dense), `gpuaddr`, `size` (actual, page-rounded), `flags` (actual) | `bo_init`; echo of actual flags lets capability probing work like `kgsl_is_memory_type_supported` |
| `MEM_FREE` | `id` | — | `bo_finish`. Recommended: defer the SMMU unmap until the last timestamp submitted on every context of this client has retired (KGSL refcounts in-flight memory); turnip only frees after the app says it is idle, but internal BOs (e.g. the vis-stream BO replaced in `vk/tu_queue.cc:192-198`) are released by refcount, and an SMMU fault would wedge the GPU. |
| `MEM_MAP` (IOKit memory type = id) | `id`, cache mode | user VA | `bo_map` |
| `SUBMIT` | `ctx_id`, `ibs[] {gpuaddr, size_bytes, id}`, optional `wait {ctx_id, timestamp}`, `flags` | `timestamp` (u32, per-context, monotonic) | `queue_submit`. Kernel emits `CP_INDIRECT_BUFFER` per IB, then cache flush + timestamp write + IRQ. Zero IBs: either reject (turnip handles it, §2.5) or signal immediately. |
| `WAIT_TIMESTAMP` | `ctx_id`, `timestamp`, `timeout_ms` (UINT32_MAX = ∞, 0 = poll) | status: OK / TIMEOUT / **CONTEXT_FAULTED** (distinct!) | `queue_wait_fence`, sync wait |
| `GET_RESET_STATUS` | `ctx_id` | NO_ERROR / GUILTY / INNOCENT / UNKNOWN (KGSL `KGSL_CTX_STAT_*` semantics, reset-on-read) | `device_check_status` |

Strongly recommended:

| selector | in | out | consumer |
|---|---|---|---|
| `READ_ALWAYS_ON` | — | u64 19.2 MHz ticks | `device_get_gpu_timestamp` (replace KGSL's UNREACHABLE) |
| `READ_TIMESTAMP` | `ctx_id` | retired u32 | debugging / cheaper poll |
| `IMPORT` | `type`: `USERPTR {addr, len}` (wire pages of e.g. an `IOSurfaceLock`ed surface via `withAddressRange` + `prepare()`), or `IOSURFACE {IOSurfaceID or mach port}` | `id`, `gpuaddr`, `size` | `bo_init_dmabuf` (re-purposed; the "fd" becomes a handle) |
| `EXPORT` | `id` | mach port / IOSurfaceID | `bo_export_dmabuf` |

Optional (feature-gating):

| selector | enables |
|---|---|
| `MEM_ALLOC` flag `FIXED_VA`/`SVM` with `client_iova` | `has_set_iova` → `bufferDeviceAddressCaptureReplay`, `descriptorBufferCaptureReplay`, `accelerationStructureCaptureReplay` (`vk/tu_device.cc:544,575,696`) |
| `VA_RESERVE(size, flags{MAP_ZERO}) → {id, gpuaddr}`, `VA_FREE`, `BIND_RANGES(target, ranges[] {child_id, child_off, target_off, len, op})` sync + queued-on-context variant returning a timestamp | `has_sparse`, `has_sparse_prr`, `has_lazy_bos` → sparse binding/residency, sparse queue family, lazily-allocated memory type |
| `TIMESTAMP_EVENT(ctx, ts) → notification` (mach port via `IOConnectCallAsyncMethod`, or an fd if you provide a kqueue-able object) | external SYNC_FD semaphores/fences, multi-queue wait-any |
| `SUBMIT` profiling object `{id, offset}` → kernel fills `{wall_clock_ns, gpu_ticks_queued, submitted, retired}` | Perfetto GPU timeline correlation |
| Context priority levels | `VK_EXT_global_priority` > MEDIUM |
| Leave `*_PERFCTR_*_SEL` unprotected | `is_perf_cntr_selectable` → `KHR_performance_query` |
| Per-BO metadata get/set | modifier transfer for shared images |

### 3.3 What breaks/disappears if an op is missing

| missing | consequence |
|---|---|
| GET_RESET_STATUS / faulted status from WAIT | hangs never become `VK_ERROR_DEVICE_LOST`; KGSL code treats any wait error as timeout (`vk/tu_knl_kgsl.cc:807-809`) — don't copy that |
| READ_ALWAYS_ON | must stub `device_get_gpu_timestamp`; A650 doesn't expose calibrated timestamps, so only Perfetto suffers |
| sparse ops | no sparse features, no SPARSE queue family, no `LAZILY_ALLOCATED` memory type |
| FIXED_VA/SVM | no capture/replay features (core BDA still works) |
| IO-coherent caching | no HOST_CACHED+COHERENT type; WC type and cached-non-coherent type remain |
| IMPORT/EXPORT | external memory must be un-advertised by patching (see §4.2) |
| TIMESTAMP_EVENT | no SYNC_FD external semaphores/fences (leave sync_file callbacks NULL) |
| profiling buffer | no Perfetto GPU/CPU correlation |
| priorities | only MEDIUM global priority |
| zero-IB submit | nothing (turnip already avoids it) |

### 3.4 Kernel-side duties that turnip silently assumes

* Per-process GPU pagetable (SMMU TTBR0 switch per context, e.g.
  `CP_SMMU_TABLE_UPDATE`), with every live BO mapped; returned `gpuaddr` must
  avoid the GMEM aperture (`[gmem_base, gmem_base+gmem_size)`, i.e. VA 0–1.125
  MiB on A650 if `gmem_base = 0`) and the UCHE trap page.
* SQE/GMU firmware, zap shader / secure-mode exit, HWCG, `CP_PROTECT`,
  UBWC `*_NC_MODE_CNTL` registers programmed for HBB 16 / UBWC 4.0 (see
  upstream `a6xx_set_ubwc_config`), `UCHE_GMEM_RANGE_*`, UCHE trap base, always-on
  counter. Turnip assumes all of these.
* After each submission's IBs: cache flush event + timestamp write + IRQ;
  context switch/preemption with GMEM save if preemption is enabled.
* `CP_SCRATCH_REG(4)` and `(5)` belong to userspace.

---

## 4. Linux/Android-specific bits outside `tu_knl*.cc`

### 4.1 Already handled by build options / stubs

| item | where | how it goes away |
|---|---|---|
| libdrm (`xf86drm.h`) | `tu_knl.cc:22,373-493` (DRM enumeration), `vk_instance.c:26,414-441`, `perfcntrs/freedreno_perfcntr.c:12` | `util/libdrm.h` stubs when `HAVE_LIBDRM` undefined (automatic on Darwin, `meson.build:162,1920-1940`) |
| `vk_drm_syncobj.c`, `u_sync_provider.c` | `src/vulkan/runtime/meson.build:57-60`, `src/util/meson.build:317-320` | only built with libdrm |
| `drm-uapi/drm.h`, `drm_fourcc.h`, `msm_drm.h` (headers only) | `vk/tu_device.cc:12`, perfcntrs | non-Linux branch uses `<sys/ioccom.h>` (`include/drm-uapi/drm.h:36-60`), exists on Darwin |
| WSI (X11/Wayland/display) | `tu_wsi.cc` built only for x11/wayland/KMS (`vk/meson.build:106-124`); `TU_USE_WSI_PLATFORM` only for WAYLAND/XCB/XLIB/DISPLAY (`vk/tu_wsi.h:15-20`) | `-Dplatforms=` (empty). `wsi_common.c` is still built/linked (`src/vulkan/wsi/meson.build:4`) and is portable. Note `platforms=macos` would pull Metal/QuartzCore and Metal WSI, which turnip does not wire up anyway. |
| Android gralloc / AHB | `vk/tu_device.cc:53-55,3715-3725`; `vk_android.c` only with `platforms=android` (`runtime/meson.build:62-65`) | `DETECT_OS_ANDROID` false |
| `sys/sysmacros.h`/`sys/mkdev.h` | `vk/tu_knl.cc:12-17` | guarded; Darwin has `major/minor` in `<sys/types.h>` |
| xmlconfig (driconf files `DATADIR/drirc.d`, `SYSCONFDIR/drirc`, `~/.drirc`) | `src/util/xmlconfig.c:1360-1387` | `-Dxmlconfig=disabled` → compiled-in `driconf_static.h` (`:1228-1229`) incl. `00-turnip-defaults.conf` |
| inotify (`TU_DEBUG_FILE`) | `src/util/os_file_notify.c:11-13,235` | Linux-only, stubbed elsewhere |
| libelf | `vk/meson.build:215` `dep_elf` | optional (`meson.build:2142-2152`) |
| valgrind, libunwind, perfetto | | `-Dvalgrind=disabled -Dlibunwind=disabled -Dperfetto=false` |
| memfd/anon files | `src/util/anon_file.c` | falls back to `mkostemp`/tmp on Darwin; turnip doesn't call it on the KGSL path |
| `/proc/device-tree` crawl | `perfcntrs/freedreno_dt.c` | only used by fdperf tooling |

The Vulkan runtime + util already compile on macOS (the `kosmickrisp`
Vulkan-on-Metal driver lives in this tree, `meson.build:317`), which
de-risks the generic layers.

### 4.2 Must be patched for Darwin (not removable by options)

1. **`pthread_condattr_setclock(CLOCK_MONOTONIC)`** in `tu_CreateDevice`
   (`vk/tu_device.cc:3165-3187`) — not in Darwin libc ⇒ compile error. And
   `kgsl_syncobj_wait` uses `pthread_cond_timedwait` with a monotonic absolute
   time (`vk/tu_knl_kgsl.cc:840-858`), which Darwin interprets as REALTIME.
   Use `util/cnd_monotonic.h` (`u_cnd_monotonic`, which already special-cases
   Apple, `src/util/cnd_monotonic.c:56-59,127`) or
   `pthread_cond_timedwait_relative_np`.
2. **fd-typed device handle**: `tu_physical_device_finish` does
   `close(local_fd)` and closes `master_fd`/`kgsl_dma_fd` if ≠ -1
   (`vk/tu_device.cc:1889-1902`). Set both extras to -1 and either give
   `local_fd` a harmless real fd or add an `#ifdef`. `device->fd` is also
   handed to `vk_device_set_drm_fd` when `!is_kgsl()` (`vk/tu_device.cc:2917-2919`).
3. **`is_kgsl()` string check** (`vk/tu_device.cc:174-177`) gates
   `EXT_physical_device_drm` (`:356`), `vk_device_set_drm_fd` (`:2918`), the
   KGSL profiling suballocator (`:3024`), the perfcntr fd (`:3145`). Replace
   with a "not DRM" predicate that also covers the Darwin backend.
4. **CPU unmap is generic and POSIX**: `tu_bo_unmap` calls `munmap` /
   `mmap(MAP_FIXED|MAP_ANONYMOUS, PROT_NONE)` itself (`vk/tu_knl.cc:146-167`),
   and placed maps need `MAP_FIXED` semantics (`EXT_map_memory_placed`,
   `memoryMapPlaced`, `memoryUnmapReserve` hard-coded true,
   `vk/tu_device.cc:349,781-784,1424-1426`). An `IOConnectMapMemory64`
   mapping torn down by `munmap` leaks the kernel `IOMemoryMap`. Either add a
   `bo_unmap` hook to `tu_knl`, or set `bo->never_unmap = true` for every
   Darwin BO (map once, unmap in `bo_finish` via `IOConnectUnmapMemory64`) and
   gate `EXT_map_memory_placed` off.
5. **External memory is advertised unconditionally**: `KHR_external_memory_fd`,
   `KHR_external_semaphore_fd`, `KHR_external_fence_fd` (`vk/tu_device.cc:226-230`),
   `EXT_external_memory_dma_buf`, `EXT_image_drm_format_modifier`,
   `EXT_queue_family_foreign` (`:329,340,367`), and
   `tu_GetPhysicalDeviceExternalBufferProperties` reports OPAQUE_FD/DMA_BUF
   (`vk/tu_buffer.cc:152-170`, `vk/tu_formats.cc:741-749`). The import path
   `close()`s the user's fd (`vk/tu_device.cc:3711-3713`). Gate these on a
   backend capability or the app will get runtime failures.
6. **Page size**: `os_page_size` is re-read from the OS
   (`vk/tu_device.cc:57,1425`) → 16384 on iOS arm64. BO sizes/alignments
   become 16 KiB multiples; `minPlacedMemoryMapAlignment` = 16 KiB. Fine for
   a 4 KiB SMMU, but the kernel must accept 16 KiB-multiple sizes and
   `IOConnectMapMemory64` sizes; sparse binds still assume 4 KiB
   (`vk/tu_knl.cc:315-316`).
7. **The KGSL backend file itself** (`<linux/dma-heap.h>`, `ion/*.h`,
   `<linux/types.h>`/`<linux/ioctl.h>` via `msm_kgsl.h`) — don't build it;
   copy `kgsl_syncobj` + `vk_kgsl_sync_type` into the new backend, minus the
   `FD` state (or keep it compiling: `util/libsync.h` only needs
   `<sys/ioctl.h>`/`<poll.h>`).
8. **Userspace cache maintenance**: cached-non-coherent memory is exposed on
   aarch64 (`has_cached_non_coherent_memory = level1_dcache_size > 0 &&
   !DETECT_ARCH_ARM`, `vk/tu_device.cc:1758-1760`; `DETECT_ARCH_ARM` is
   32-bit only). That needs EL0 `mrs ctr_el0` and `dc cvac/civac`
   (`src/util/cache_ops_aarch64.c:33,99,106`) — the XNU port must set
   `SCTLR_EL1.UCT/UCI` (verify on your kernel), otherwise drop that memory
   type.
9. **Meson host system**: Mesa keys Darwin behaviour on
   `host_machine.system() == 'darwin'` (`meson.build:162,200,288,418,435,476,1842`).
   In the iOS cross file use `system = 'darwin'` (with `subsystem = 'ios'` on
   meson ≥ 1.2) — `'ios'` would hit "Unknown OS" errors.
10. Minor/benign: `tu_cs_breadcrumbs.cc` uses BSD sockets (debug only);
    `FD_RD_DUMP` defaults to `/tmp` (`common/freedreno_rd_output.c:50-57`) —
    set `FD_RD_DUMP_PATH` to a sandbox dir; env vars (`TU_DEBUG`,
    `MESA_SHADER_CACHE_DIR`, …) must be `setenv()`'d by the app on iOS.
11. Shipping: `libvulkan_freedreno` is a `shared_library`
    (`vk/meson.build:202-232`); on iOS it must be an embedded, signed dylib/
    framework loaded via the Vulkan loader with `VK_DRIVER_FILES`, or linked
    directly and called through `vk_icdGetInstanceProcAddr`.

### 4.3 Meson options for a minimal turnip-only Darwin build

```
meson setup build-ios --cross-file ios-arm64.ini \
  -Dvulkan-drivers=freedreno \
  -Dfreedreno-kmds=darwin \
  -Dgallium-drivers= \
  -Dplatforms= \
  -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
  -Degl=disabled -Dglx=disabled -Dgbm=disabled -Dglvnd=disabled \
  -Dllvm=disabled -Dspirv-tools=disabled \
  -Dxmlconfig=disabled -Dexpat=disabled \
  -Dshader-cache=disabled \
  -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dzstd=disabled -Ddisplay-info=disabled \
  -Dperfetto=false -Dbuild-tests=false -Dtools= -Dvulkan-layers= \
  -Dvideo-codecs= -Dandroid-libbacktrace=disabled \
  -Dtu-build-id=0123456789abcdef
```

* `-Dfreedreno-kmds=darwin` is the new choice you add; before it exists,
  `-Dfreedreno-kmds=` (empty) gives a compile-only smoke test (turnip builds
  but enumerates no devices).
* `-Dshader-cache=enabled` also works (needs zlib, the default), see §5.
* `-Dtu-build-id=<even-length hex>` removes the dladdr/build-id dependency of
  the cache UUID; bump it whenever the compiler changes.
* `gallium-drivers`, `platforms` and `vulkan-drivers` **must** be explicit:
  `auto` on Darwin picks `['x11','macos']` platforms and no Vulkan driver
  (`meson.build:288-290,476-477`).

Build-host requirements: Python 3 + mako/pyyaml, and **`glslangValidator`
(native)**, which is mandatory because turnip's BVH and float64 shaders are
compiled at build time (`meson.build:735,740-744`; `vk/meson.build:68-92`).
LLVM is not needed (ir3 has its own compiler). `vulkan-beta` is forced on
for Darwin (`meson.build:435-438`) — harmless.

---

## 5. Shader compilation and runtime files

* Pipeline: SPIR-V → NIR (`vk_pipeline_shader_stage_to_nir`, spirv_to_nir;
  `vk/tu_shader.cc:197-214`) → turnip NIR lowering → `ir3_shader_from_nir` +
  `ir3_shader_create_variant` (`vk/tu_shader.cc:3432-3443`) → binary
  `memcpy`'d into a CPU mapping of the `pipeline_suballoc` BO (GPU read-only,
  `vk/tu_shader.cc:2913,2952`; suballocator flags `vk/tu_device.cc:3016-3021`).
  Built-in float32/float64 soft-float libraries and BVH shaders are SPIR-V
  embedded at build time (`vk/tu_shader.cc:142-160`, `vk/meson.build:68-94`).
* **No kernel involvement** and **no host JIT** (no RWX CPU pages): GPU code
  is just data in a GPU-readonly BO. The only kernel requirements are
  MEM_ALLOC + MEM_MAP.
* Caching: ir3's own disk cache is disabled for turnip
  (`ir3_options.disable_cache = true`, `vk/tu_device.cc:2980`;
  `ir3/ir3_compiler.c:447-448`). Turnip uses the runtime
  `vk_pipeline_cache` in memory, plus an on-disk `disk_cache` created per
  physical device (`vk/tu_device.cc:1871-1875`) when built with
  `ENABLE_SHADER_CACHE` (`meson.build:1173-1181`; requires zlib or zstd,
  `:1811-1816`). The on-disk location is `$MESA_SHADER_CACHE_DIR`, else
  `$XDG_CACHE_HOME`/`~/.cache` — in an iOS sandbox set
  `MESA_SHADER_CACHE_DIR` to `<container>/Library/Caches/mesa`, or build with
  `-Dshader-cache=disabled`.
* Cache UUID: build id via `dladdr` + Mach-O `LC_UUID` (`src/util/build_id.c`
  has an Apple path; `src/util/disk_cache.h:81-122`) or `-Dtu-build-id`.
  Failure here kills device enumeration (`vk/tu_device.cc:1749-1753`).
* Files read at runtime: none required. Optional: driconf XML (only with
  `xmlconfig` enabled), `IR3_SHADER_OVERRIDE_PATH/*.asm` (debug,
  `ir3/ir3_shader.c:185-197`), shader cache dir, `TU_DEBUG_FILE`,
  `FD_RD_DUMP_PATH` output. No firmware or register database is loaded by
  turnip (registers are generated headers).

---

## Appendix A — msm (DRM) backend, for contrast

`tu_knl_drm_msm_load` (`vk/tu_knl_drm_msm.cc:1588-1702`): requires msm
≥ 1.6, queries `MSM_PARAM_GPU_ID/CHIP_ID/GMEM_SIZE/GMEM_BASE/VA_START/
VA_SIZE/PRIORITIES/HIGHEST_BANK_BIT/MACROTILE_MODE/UBWC_SWIZZLE/
UCHE_TRAP_BASE/RAYTRACING/HAS_PRR`, tries `MSM_PARAM_EN_VM_BIND`, uses
`vk_drm_syncobj` types, userspace-managed GPU VA (`util_vma_heap`, zombie VMA
recycling `tu_knl_drm.cc:139-162`), a per-device submit BO list
(`tu_bo_add_to_bo_list`), and `DRM_IOCTL_MSM_GEM_SUBMIT`/`VM_BIND`. It is a
worse fit for XNU because it needs DRM syncobjs, sync_file, PRIME and either a
BO list per submit or VM_BIND.

## Appendix B — checklist for `tu_knl_darwin.cc`

1. `tu_knl_darwin_load(instance)`: IOServiceOpen; GET_INFO; fill §1.5 table
   (`master_fd = kgsl_dma_fd = -1`, `has_preemption = true`,
   `submitqueue_priority_count = 1..N`, `sync_types` from a copied
   `vk_kgsl_sync_type` with NULL sync_file hooks and an emulated timeline);
   `instance->knl = &darwin_knl_funcs`; `tu_physical_device_init`;
   `list_addtail`.
2. vtable: implement 2–11, 13–15, 18–20, 22–23; stub 12, 16, 17, 21, 24, 25,
   26 (NULL) while `has_sparse = has_lazy_bos = false`.
3. `bo_init`: MEM_ALLOC; slot = `tu_device_lookup_bo(dev, id)`; fill
   `{gem_handle=id, size, iova=gpuaddr, name=tu_debug_bos_add(), refcnt=1,
   base}`; `tu_dump_bo_init`; reject non-zero `client_iova` unless FIXED_VA
   is supported.
4. `bo_map`: IOConnectMapMemory64(type=id); consider `never_unmap = true`.
5. `bo_finish`: refcount, `tu_debug_bos_del`, `tu_dump_bo_del`, unmap,
   `memset(bo,0)`, MEM_FREE.
6. `queue_submit`: copy the KGSL logic (empty-submit merge, single wait
   syncpoint, set `queue->fence`, set signals to `TS`), call SUBMIT; on error
   `vk_device_set_lost`.
7. `queue_wait_fence` / sync waits: WAIT_TIMESTAMP with relative ms; map
   CONTEXT_FAULTED to `vk_device_set_lost`.
8. `device_check_status`: GET_RESET_STATUS per non-emulated queue.
9. `device_get_gpu_timestamp`: READ_ALWAYS_ON (don't leave it UNREACHABLE).
10. Patches in generic code: §4.2 items 1–6.
