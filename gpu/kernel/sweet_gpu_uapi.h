/*
 * sweet_gpu_uapi.h - user/kernel interface of the SWEET Adreno driver.
 *
 * Shared verbatim by the XNU driver (iokit/Kernel/arm/SweetAdreno.cpp),
 * libsweetgpu and the Mesa turnip "sweet" backend. Plain C, fixed-width
 * fields, no pointers: every struct crosses the user/kernel boundary as an
 * IOConnectCallStructMethod in/out blob, so layout is ABI.
 *
 * Model: Qualcomm KGSL, not Linux msm DRM. Reasons:
 *   - turnip's KGSL backend needs no DRM syncobj, dma-buf or GEM handles:
 *     synchronisation is "context + monotonically increasing timestamp",
 *     which needs no extra kernel objects;
 *   - the kernel assigns GPU virtual addresses (one call returns id,
 *     gpuaddr and the CPU mapping), which keeps the first driver simple;
 *   - KGSL is what Qualcomm's own stack used on this exact SoC, so the
 *     semantics are known to fit the hardware (memstore timestamps written
 *     by CP_EVENT_WRITE at the end of every submission).
 *
 * Service: IOServiceMatching("SweetAdreno"), IOServiceOpen type 0.
 * Every method: IOConnectCallStructMethod(conn, selector, in, sizeof in,
 * out, &outsize). Methods without input pass NULL/0.
 */
#ifndef SWEET_GPU_UAPI_H
#define SWEET_GPU_UAPI_H

#include <stdint.h>

#define SGPU_SERVICE_CLASS      "SweetAdreno"
#define SGPU_UAPI_VERSION       2

enum sgpu_selector {
	SGPU_SEL_GET_INFO       = 0,    /* -> sgpu_info */
	SGPU_SEL_CTX_CREATE     = 1,    /* sgpu_ctx_create -> sgpu_ctx_create */
	SGPU_SEL_CTX_DESTROY    = 2,    /* sgpu_ctx_destroy */
	SGPU_SEL_MEM_ALLOC      = 3,    /* sgpu_mem_alloc -> sgpu_mem_alloc */
	SGPU_SEL_MEM_FREE       = 4,    /* sgpu_mem_free */
	SGPU_SEL_SUBMIT         = 5,    /* sgpu_submit -> sgpu_submit */
	SGPU_SEL_WAIT_TS        = 6,    /* sgpu_wait_ts */
	SGPU_SEL_READ_TS        = 7,    /* sgpu_read_ts -> sgpu_read_ts */
	SGPU_SEL_IMPORT_SURFACE = 8,    /* sgpu_import_surface -> same */
	SGPU_SEL_SYNC_CACHE     = 9,    /* sgpu_sync_cache */
	SGPU_SEL_GPU_TIME       = 10,   /* -> sgpu_gpu_time */
	SGPU_SEL_GET_STATUS     = 11,   /* -> sgpu_status (bring-up diagnostics) */
	SGPU_SEL_IMPORT_USER    = 12,   /* sgpu_import_user -> same (v2) */
	SGPU_SEL_COUNT
};

/* ---- GET_INFO ---- */
struct sgpu_info {
	uint32_t uapi_version;          /* SGPU_UAPI_VERSION */
	uint32_t gpu_id;                /* 650 */
	uint32_t chip_id;               /* 0x06050002 (core.major.minor.patch) */
	uint32_t gmem_size;             /* bytes of on-chip GMEM */
	uint64_t gmem_base;             /* GPU address the CP uses for GMEM */
	uint64_t va_start;              /* user GPU VA range handed out */
	uint64_t va_size;
	uint32_t highest_bank_bit;      /* UBWC/tiling config (stock DT: 16) */
	uint32_t ubwc_mode;             /* stock DT qcom,ubwc-mode (4) */
	uint32_t min_access_length;     /* 32 */
	uint32_t timer_freq_hz;         /* CP always-on counter: 19200000 */
	uint32_t max_freq_hz;           /* current GPU clock ceiling */
	uint32_t max_contexts;
	uint32_t flags;                 /* SGPU_INFO_* */
	uint32_t reserved0;
	uint64_t uche_trap_base;        /* unmapped GPU VA programmed as UCHE_TRAP_BASE */
	uint32_t reserved[6];
};
#define SGPU_INFO_IO_COHERENT   (1u << 0)   /* SMMU snoops CPU caches */
#define SGPU_INFO_PER_PROCESS_PT (1u << 1)  /* each client has its own GPU VA space */

/* ---- contexts (one hardware timeline each) ---- */
struct sgpu_ctx_create {
	uint32_t flags;                 /* in: SGPU_CTX_* */
	uint32_t priority;              /* in: 0 = highest .. 15 */
	uint32_t ctx_id;                /* out */
	uint32_t reserved;
};
#define SGPU_CTX_NO_FAULT_TOLERANCE (1u << 0)

struct sgpu_ctx_destroy {
	uint32_t ctx_id;
	uint32_t reserved;
};

/* ---- memory ---- */
struct sgpu_mem_alloc {
	uint64_t size;                  /* in: bytes (rounded up to 16 KiB) */
	uint32_t flags;                 /* in: SGPU_MEM_* */
	uint32_t align_log2;            /* in: GPU VA alignment, 0 = default 16 KiB */
	uint32_t id;                    /* out: handle for free/sync */
	uint32_t reserved;
	uint64_t gpuaddr;               /* out */
	uint64_t cpu_addr;              /* out: mapping in the caller's task, 0 if NO_CPU_MAP */
	uint64_t alloc_size;            /* out: actual size */
};
/*
 * MEM_ALLOC: CPU mapping write-back cached. Imports (the caller's pages are
 * cached already): the GPU mapping is write-back inner-shareable, which the
 * GPU's accesses snoop the CPU caches through on an IO-coherent device
 * (SGPU_INFO_IO_COHERENT); without the flag no snoop happens.
 */
#define SGPU_MEM_CACHED         (1u << 0)
#define SGPU_MEM_GPU_READONLY   (1u << 1)
#define SGPU_MEM_NO_CPU_MAP     (1u << 2)
#define SGPU_MEM_GPU_EXEC       (1u << 3)   /* holds command streams / shaders */

struct sgpu_mem_free {
	uint32_t id;
	uint32_t reserved;
};

/* Import an IOSurface (SweetSurface id) so the GPU can render into it. */
struct sgpu_import_surface {
	uint32_t surface_id;            /* in */
	uint32_t flags;                 /* in: SGPU_MEM_GPU_READONLY, SGPU_MEM_CACHED */
	uint32_t id;                    /* out: memory handle (free with MEM_FREE) */
	uint32_t reserved;
	uint64_t gpuaddr;               /* out */
	uint64_t size;                  /* out */
};

/*
 * Map the caller's own pages into the GPU (VK_EXT_external_memory_host,
 * newBufferWithBytesNoCopy:, the mapping of an IOSurface). The pages are
 * wired until MEM_FREE.
 */
struct sgpu_import_user {
	uint64_t addr;                  /* in: page-aligned address in the caller */
	uint64_t size;                  /* in: whole pages */
	uint32_t flags;                 /* in: SGPU_MEM_GPU_READONLY, SGPU_MEM_CACHED */
	uint32_t id;                    /* out: memory handle (free with MEM_FREE) */
	uint64_t gpuaddr;               /* out */
};

struct sgpu_sync_cache {
	uint32_t id;
	uint32_t op;                    /* SGPU_CACHE_* */
	uint64_t offset;
	uint64_t length;                /* 0 = to the end */
};
#define SGPU_CACHE_CLEAN        1   /* CPU wrote, GPU will read */
#define SGPU_CACHE_INVALIDATE   2   /* GPU wrote, CPU will read */
#define SGPU_CACHE_FLUSH        3   /* both */

/* ---- submission ---- */
#define SGPU_MAX_IBS            64

struct sgpu_ib {
	uint64_t gpuaddr;               /* command stream in GPU memory */
	uint32_t size_dwords;
	uint32_t flags;                 /* reserved, 0 */
};

struct sgpu_submit {
	uint32_t ctx_id;                /* in */
	uint32_t num_ibs;               /* in: 0..SGPU_MAX_IBS (0 = marker) */
	uint32_t flags;                 /* in: SGPU_SUBMIT_* */
	uint32_t timestamp;             /* out: this submission's timestamp */
	struct sgpu_ib ibs[SGPU_MAX_IBS];
};
#define SGPU_SUBMIT_NO_CACHE_FLUSH (1u << 0)

struct sgpu_wait_ts {
	uint32_t ctx_id;
	uint32_t timestamp;
	uint32_t timeout_ms;            /* 0 = poll only */
	uint32_t reserved;
};

struct sgpu_read_ts {
	uint32_t ctx_id;                /* in */
	uint32_t retired;               /* out: last completed timestamp */
	uint32_t queued;                /* out: last submitted timestamp */
	uint32_t reserved;
};

/* Timestamps wrap: a is at or after b. */
static inline int
sgpu_ts_ge(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) >= 0;
}

struct sgpu_gpu_time {
	uint64_t gpu_ticks;             /* CP always-on counter */
	uint64_t cpu_ticks;             /* mach_absolute_time() sampled together */
};

/* ---- diagnostics ---- */
struct sgpu_status {
	uint32_t stage_reached;         /* last bring-up stage that succeeded */
	uint32_t stage_requested;       /* sweetgpu=N */
	int32_t  last_error;            /* errno-style */
	uint32_t faults;                /* SMMU / CP faults seen */
	uint32_t hangs;
	uint32_t rbbm_status;
	uint32_t cp_rb_rptr;
	uint32_t cp_rb_wptr;
	uint32_t gmu_state;
	uint32_t reserved[7];
	char     message[128];          /* last error text */
};

#endif /* SWEET_GPU_UAPI_H */
