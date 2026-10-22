/*
 * sweet_gpu.h - internal interface of the SWEET Adreno 650 driver
 * (Xiaomi Mi 10T / 10T Pro "apollo", Qualcomm SM8250).
 *
 * Layering (see docs/GPU-DRIVER.md):
 *
 *   SweetAdreno.cpp      IOService + IOUserClient: UAPI, per-task mappings,
 *                        IOSurface import, bring-up thread
 *   sweet_gpu_cp.c       GPU core: hw_init, SQE, ringbuffer, submit, timestamps
 *   sweet_gpu_gmu.c      GMU: RSC/PDC, firmware, HFI, OOB (GX power), DCVS
 *   sweet_gpu_smmu.c     Adreno SMMU (MMU-500): context banks + LPAE tables
 *   sweet_gpu_scm.c      TrustZone SCM calls + zap shader (PAS) loader
 *   sweet_gpu_clk.c      GCC/GPU_CC clocks, PLL, CX GDSC
 *   sweet_gpu_cmddb.c    RPMh command DB reader (ARC levels for GMU votes)
 *   sweet_gpu_mem.c      GPU VA allocator, kernel buffers
 *   sweet_gpu_os_xnu.c   the only file that knows XNU (hosttest/ swaps it)
 *
 * Everything except SweetAdreno.cpp and sweet_gpu_os_xnu.c is plain C
 * against the sgpu_os_* API below, so the whole bring-up sequence also
 * builds and runs on Linux against a register model (hosttest/).
 */
#ifndef SWEET_GPU_H
#define SWEET_GPU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "sweet_gpu_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* OS layer                                                           */
/* ------------------------------------------------------------------ */

void     sgpu_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void     sgpu_udelay(uint32_t us);
void     sgpu_msleep(uint32_t ms);              /* may block (thread context) */
uint64_t sgpu_now_us(void);
volatile uint8_t *sgpu_ioremap(uint64_t pa, size_t size);   /* Device-nGnRE */

/*
 * Physically contiguous, zeroed memory for the GPU, GMU, SMMU walker or
 * TrustZone. Default CPU mapping is write-back cached (callers clean/
 * invalidate around device access). SGPU_DMA_WC gives a Normal
 * non-cacheable (write-combine) CPU mapping instead: the same thing Linux
 * uses for every msm kernel BO (MSM_BO_WC). The cacheable alias of the
 * pages is cleaned+invalidated before the WC mapping is handed out and is
 * never touched again, so no dirty line can ever overwrite device data.
 * WC memory still needs sgpu_wmb() before telling the device about it.
 */
#define SGPU_DMA_WC             (1u << 0)
struct sgpu_dma {
	void     *va;
	uint64_t  pa;
	size_t    size;
	void     *os_md;                /* XNU: IOBufferMemoryDescriptor */
	void     *os_map;               /* XNU: IOMemoryMap of the WC view */
};
int      sgpu_dma_alloc(size_t size, size_t align, uint32_t flags, struct sgpu_dma *out);
void     sgpu_dma_free(struct sgpu_dma *dma);
void     sgpu_cache_clean(const void *va, size_t len);  /* CPU -> device */
void     sgpu_cache_inval(const void *va, size_t len);  /* device -> CPU */

struct sgpu_smc {
	uint64_t x[8];                  /* in: x0..x7, out: x0..x3 */
};
void     sgpu_smc_call(struct sgpu_smc *args);

bool     sgpu_bootarg_u32(const char *name, uint32_t *out);
/* Property of the ADT node /sweet-gpu (firmware blobs, regions). */
bool     sgpu_dt_prop(const char *prop, const void **data, uint32_t *len);

/* ------------------------------------------------------------------ */
/* MMIO                                                               */
/* ------------------------------------------------------------------ */

#if defined(SGPU_HOST)
/* hosttest/: every register access goes to the hardware model. */
uint32_t sgpu_host_rd(volatile uint8_t *base, uint32_t off);
void     sgpu_host_wr(volatile uint8_t *base, uint32_t off, uint32_t v);
uint64_t sgpu_host_rd64(volatile uint8_t *base, uint32_t off);
void     sgpu_host_wr64(volatile uint8_t *base, uint32_t off, uint64_t v);
static inline uint32_t sgpu_rd(volatile uint8_t *b, uint32_t o) { return sgpu_host_rd(b, o); }
static inline void sgpu_wr(volatile uint8_t *b, uint32_t o, uint32_t v) { sgpu_host_wr(b, o, v); }
static inline uint64_t sgpu_rd64(volatile uint8_t *b, uint32_t o) { return sgpu_host_rd64(b, o); }
static inline void sgpu_wr64(volatile uint8_t *b, uint32_t o, uint64_t v) { sgpu_host_wr64(b, o, v); }
#else
/*
 * Single 32/64-bit LDR/STR with a base register and no writeback: the
 * Adreno SMMU global space on SM8250 is trapped and emulated by the
 * Qualcomm hypervisor (QHEE), which can only decode such accesses
 * (ESR.ISV = 1), and it mishandles stores from WZR/XZR (Linux works
 * around this with QCOM_DUMMY_VAL). The "r" constraint forces the value
 * into a real register even when it is the constant 0, so every MMIO
 * store in this driver is safe for trapped regions.
 */
static inline uint32_t
sgpu_rd(volatile uint8_t *base, uint32_t off)
{
	uint32_t v;
	__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(base + off) : "memory");
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline void
sgpu_wr(volatile uint8_t *base, uint32_t off, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	__asm__ volatile("str %w0, [%1]" :: "r"(v), "r"(base + off) : "memory");
}

static inline uint64_t
sgpu_rd64(volatile uint8_t *base, uint32_t off)
{
	uint64_t v;
	__asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(base + off) : "memory");
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline void
sgpu_wr64(volatile uint8_t *base, uint32_t off, uint64_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	__asm__ volatile("str %0, [%1]" :: "r"(v), "r"(base + off) : "memory");
}
#endif

static inline void
sgpu_rmw(volatile uint8_t *base, uint32_t off, uint32_t clear, uint32_t set)
{
	sgpu_wr(base, off, (sgpu_rd(base, off) & ~clear) | set);
}

/* Make prior normal-memory writes (ring, PTEs, HFI) visible to devices. */
static inline void
sgpu_wmb(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dsb sy" ::: "memory");
#else
	__sync_synchronize();
#endif
}

/* Read shared memory the device writes (fences, rptr shadow, HFI). */
static inline uint32_t
sgpu_mem_rd32(const volatile void *p)
{
	uint32_t v = *(const volatile uint32_t *)p;
#if defined(__aarch64__)
	__asm__ volatile("dmb ld" ::: "memory");
#endif
	return v;
}

static inline void
sgpu_mem_wr32(volatile void *p, uint32_t v)
{
	*(volatile uint32_t *)p = v;
}

/* Poll until (reg & mask) == want; returns 0 or -ETIMEDOUT-like -1. */
int sgpu_poll(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t want,
    uint32_t timeout_us);

/* ------------------------------------------------------------------ */
/* Bring-up stages (boot-arg sweetgpu=N; 0 = driver does nothing)     */
/* ------------------------------------------------------------------ */

enum sgpu_stage {
	SGPU_STAGE_OFF          = 0,
	SGPU_STAGE_PROBE        = 1,    /* map MMIO, read always-on IDs */
	SGPU_STAGE_CLOCKS       = 2,    /* GCC/GPU_CC clocks, CX GDSC; GMU regs readable */
	SGPU_STAGE_SMMU         = 3,    /* Adreno SMMU context banks for GPU + GMU */
	SGPU_STAGE_GMU          = 4,    /* GMU firmware running, HFI started */
	SGPU_STAGE_GX           = 5,    /* GX powered via GMU OOB, GPU regs readable */
	SGPU_STAGE_CP           = 6,    /* zap, hw_init, SQE, ringbuffer, CP_ME_INIT */
	SGPU_STAGE_SELFTEST     = 7,    /* kernel submits a memwrite + timestamp */
	SGPU_STAGE_USER         = 8,    /* user client published */
	SGPU_STAGE_MAX          = SGPU_STAGE_USER,
};

/* ------------------------------------------------------------------ */
/* Device state                                                       */
/* ------------------------------------------------------------------ */

struct sgpu_bo {                        /* kernel-owned GPU buffer */
	struct sgpu_dma dma;
	uint64_t        iova;
};

struct sgpu_ctx_slot {                  /* per-context memstore slot */
	bool     used;
	uint32_t queued;                /* last timestamp handed out */
};

#define SGPU_MAX_CTX            64

struct sgpu {
	/* MMIO */
	volatile uint8_t *gpu;          /* 0x03d00000 GPU core (GX) */
	volatile uint8_t *gmu;          /* 0x03d6a000 GMU (CX) */
	volatile uint8_t *rscc;         /* 0x03de0000 GMU RSC */
	volatile uint8_t *pdc;          /* 0x0b290000 GMU PDC config */
	volatile uint8_t *pdc_seq;      /* 0x0b490000 GMU PDC sequencer */
	volatile uint8_t *gpucc;        /* 0x03d90000 GPU clock controller */
	volatile uint8_t *gcc;          /* 0x00100000 global clock controller */
	volatile uint8_t *smmu;         /* 0x03da0000 Adreno SMMU */
	volatile uint8_t *cx_dbgc;      /* 0x03d61000 CX debug controller */

	uint32_t stage_requested;
	uint32_t stage_reached;
	int      last_error;
	char     message[128];

	/* firmware (point into the ADT) */
	const uint8_t *sqe_fw;   uint32_t sqe_fw_len;
	const uint8_t *gmu_fw;   uint32_t gmu_fw_len;
	const uint8_t *zap_fw;   uint32_t zap_fw_len;
	uint64_t zap_pa;         uint32_t zap_size;     /* pil_gpu_region */

	uint32_t chip_id;               /* 0x06050002 */
	bool     io_coherent;           /* boot-arg sweetgpu_coherent=1: SMMU snoops CPU caches */
	uint32_t speed_bin;
	uint32_t gpu_freq_index;        /* index into the GPU OPP table */

	/* SMMU */
	struct sgpu_smmu *smmu_state;

	/* GMU */
	struct sgpu_gmu  *gmu_state;

	/* CP */
	struct sgpu_bo   sqe;           /* SQE microcode */
	struct sgpu_bo   rb;            /* ringbuffer */
	struct sgpu_bo   memptrs;       /* rptr shadow, fence, per-ctx memstore */
	struct sgpu_bo   scratch;       /* self-test target */
	uint32_t         rb_wptr;       /* in dwords */
	uint32_t         rb_dwords;
	uint32_t         fence_seqno;   /* global ring fence */
	struct sgpu_ctx_slot ctx[SGPU_MAX_CTX];
	uint32_t         faults, hangs;

	/* TrustZone / zap: PAS image authenticated (once per boot on a6xx) */
	bool             zap_loaded;
	/* hang check bookkeeping (sgpu_cp_hang_check) */
	uint32_t         hc_fence, hc_ib1, hc_rem, hc_strikes;
	uint64_t         hc_us;
};

/* Set message + error, log it, return err (negative errno style). */
int sgpu_fail(struct sgpu *g, int err, const char *fmt, ...)
__attribute__((format(printf, 3, 4)));

/* ---- sweet_gpu_mem.c: GPU virtual addresses ---- */
/*
 * Kernel buffers start at 16 MiB like Linux (max(SZ_16M, aperture)):
 * nothing may live in the GMEM aperture [gmem_base, +1.125 MiB) = [0, ..)
 * that turnip assumes on A650, and VA 0 stays unmapped so NULL faults.
 */
#define SGPU_VA_KERNEL_BASE     0x0000000001000000ull   /* kernel buffers */
#define SGPU_VA_KERNEL_SIZE     0x000000000f000000ull   /* 240 MiB */
#define SGPU_VA_USER_BASE       0x0000000100000000ull
#define SGPU_VA_USER_SIZE       0x0000003f00000000ull   /* 252 GiB (39-bit VA) */
int  sgpu_va_init(struct sgpu *g);
int  sgpu_va_alloc(struct sgpu *g, bool kernel, uint64_t size, uint64_t align, uint64_t *iova);
void sgpu_va_free(struct sgpu *g, uint64_t iova, uint64_t size);
/* contiguous kernel buffer mapped at a kernel GPU VA */
int  sgpu_bo_alloc(struct sgpu *g, size_t size, uint32_t prot, struct sgpu_bo *bo);
void sgpu_bo_free(struct sgpu *g, struct sgpu_bo *bo);

/* ---- sweet_gpu_clk.c ---- */
int  sgpu_clk_probe(struct sgpu *g);    /* read-only state dump */
int  sgpu_clk_enable(struct sgpu *g);   /* clocks + CX GDSC */
void sgpu_clk_disable(struct sgpu *g);

/* ---- sweet_gpu_smmu.c ---- */
#define SGPU_PROT_READ          (1u << 0)
#define SGPU_PROT_WRITE         (1u << 1)
#define SGPU_PROT_EXEC          (1u << 2)
#define SGPU_PROT_CACHED        (1u << 3)   /* MAIR idx 1 (WB, inner-shareable): IO-coherent
	                                     * when the interconnect snoops; otherwise the
	                                     * CPU must clean/invalidate (always correct) */
#define SGPU_PROT_PRIV          (1u << 4)   /* privileged-only (kernel ring etc.) */
enum sgpu_smmu_domain { SGPU_DOM_GPU = 0, SGPU_DOM_GMU = 1, SGPU_DOM_COUNT };
int  sgpu_smmu_init(struct sgpu *g);
int  sgpu_smmu_restore(struct sgpu *g);  /* reprogram after a CX power collapse */
int  sgpu_smmu_map(struct sgpu *g, enum sgpu_smmu_domain d, uint64_t iova,
    uint64_t pa, uint64_t size, uint32_t prot);
int  sgpu_smmu_unmap(struct sgpu *g, enum sgpu_smmu_domain d, uint64_t iova, uint64_t size);
void sgpu_smmu_tlb_sync(struct sgpu *g, enum sgpu_smmu_domain d);
uint32_t sgpu_smmu_check_faults(struct sgpu *g);   /* logs + clears, returns count */
uint64_t sgpu_smmu_ttbr0(struct sgpu *g, enum sgpu_smmu_domain d);

/* ---- sweet_gpu_scm.c ---- */
int  sgpu_scm_probe(struct sgpu *g);
int  sgpu_zap_load(struct sgpu *g);     /* first load: PAS init/auth */
int  sgpu_zap_resume(struct sgpu *g);   /* later power-ups */

/* ---- sweet_gpu_cmddb.c ---- */
#define SGPU_ARC_MAX_LEVELS     16
struct sgpu_arc {
	uint32_t addr;                  /* RPMh address of the resource */
	uint32_t count;
	uint16_t level[SGPU_ARC_MAX_LEVELS];    /* vlvl per hlvl index */
};
int  sgpu_cmddb_arc(const char *res, struct sgpu_arc *out);
int  sgpu_cmddb_addr(const char *res, uint32_t *addr);

/* ---- sweet_gpu_gmu.c ---- */
int  sgpu_gmu_init(struct sgpu *g);     /* memory, firmware, RSC/PDC, start, HFI */
int  sgpu_gmu_gx_on(struct sgpu *g);    /* OOB GPU_SET */
void sgpu_gmu_gx_off(struct sgpu *g);
int  sgpu_gmu_set_freq(struct sgpu *g, uint32_t index);
uint32_t sgpu_gmu_state(struct sgpu *g);
uint32_t sgpu_gmu_max_freq_hz(struct sgpu *g);
enum sgpu_oob { SGPU_OOB_GPU_SET, SGPU_OOB_PERFCOUNTER };
int  sgpu_gmu_oob_set(struct sgpu *g, enum sgpu_oob oob);
void sgpu_gmu_oob_clear(struct sgpu *g, enum sgpu_oob oob);
uint32_t sgpu_gmu_irq(struct sgpu *g);  /* poll/IRQ: logs + clears GMU faults */
void sgpu_gmu_shutdown(struct sgpu *g);

/* ---- A650 constants shared by cp/core/info ---- */
#define SGPU_A650_GMEM_SIZE     0x120000u   /* 1 MiB + 128 KiB */
/*
 * UCHE trap base: GPU accesses outside the UCHE range land here. Linux
 * uses 0x1_ffff_ffff_f000 (top of its TTBR1 window); we run a single
 * 39-bit TTBR0 space (SMMU option A), so use the top page of that space,
 * which the VA allocator never hands out.
 */
#define SGPU_UCHE_TRAP_BASE     0x0000007ffffff000ull

/* ---- sweet_gpu_cp.c ---- */
int  sgpu_cp_init(struct sgpu *g);      /* hw_init, sqe, rb, me_init, secure exit */
int  sgpu_cp_selftest(struct sgpu *g);
int  sgpu_cp_ctx_alloc(struct sgpu *g, uint32_t *ctx_id);
void sgpu_cp_ctx_free(struct sgpu *g, uint32_t ctx_id);
int  sgpu_cp_submit(struct sgpu *g, uint32_t ctx_id, const struct sgpu_ib *ibs,
    uint32_t n, uint32_t flags, uint32_t *ts_out);
uint32_t sgpu_cp_retired(struct sgpu *g, uint32_t ctx_id);
uint32_t sgpu_cp_queued(struct sgpu *g, uint32_t ctx_id);
uint64_t sgpu_cp_always_on(struct sgpu *g);
void sgpu_cp_status(struct sgpu *g, struct sgpu_status *st);
bool sgpu_cp_hang_check(struct sgpu *g);   /* periodic watchdog */
uint32_t sgpu_cp_ring_fence_submitted(struct sgpu *g);
uint32_t sgpu_cp_ring_fence_retired(struct sgpu *g);
uint32_t sgpu_cp_irq(struct sgpu *g);      /* RBBM_INT_0 handler, returns status */

/* ---- top level (sweet_gpu_core.c) ---- */
int  sgpu_bringup(struct sgpu *g, uint32_t stage);
void sgpu_fill_info(struct sgpu *g, struct sgpu_info *info);

#ifdef __cplusplus
}
#endif
#endif /* SWEET_GPU_H */
