/*
 * sweet_gpu_cp.c - Adreno 650 command processor: hw_init, SQE microcode,
 * ringbuffer, submission, timestamps, self-test, hang recovery.
 *
 * Spec: specs/cp-hwinit-a650.md (every register below carries the step
 * number of the spec's section 1.1 table, so the sequence can be diffed
 * against Linux a6xx_hw_init line by line).
 *
 * How work reaches the GPU
 * ------------------------
 * The SQE (a650_sqe.fw) is the CP's microcode; it fetches PM4 packets from
 * the kernel ringbuffer (privileged, CP_APRIV_CNTL) and from user indirect
 * buffers (unprivileged). Userspace (turnip) builds complete command
 * streams in its own GPU buffers; the kernel only appends, per submit:
 *
 *   CP_REG_TO_MEM x2          CP cycles / always-on counter at start (stats)
 *   CP_EVENT_WRITE x2         invalidate CCU depth/color
 *   CP_INDIRECT_BUFFER x N    the user's command streams
 *   CP_REG_TO_MEM x2          counters at the end
 *   PKT4 CP_SCRATCH_REG(2)    seqno (debug: last parsed submit)
 *   CP_EVENT_WRITE ts         CACHE_FLUSH_TS -> context timestamp slot
 *   CP_EVENT_WRITE ts|IRQ     CACHE_FLUSH_TS -> ring fence (memptrs+4)
 *
 * CACHE_FLUSH_TS writes its value only after all preceding work has
 * drained and UCHE is flushed, so "slot >= ts" means "everything up to ts
 * is done and visible in memory" - the KGSL timestamp model the UAPI
 * exposes. Completion is always read from memory (Linux does the same; the
 * IRQ is only a wake-up), which is why the driver works without the GPU
 * interrupt wired up.
 *
 * Address space: single shared GPU VA space (SMMU CB0, TTBR0 only), so no
 * CP_SMMU_TABLE_UPDATE, no SCM aperture call and no protected-mode dance.
 * Kernel buffers are mapped privileged (PTE AP[1] = 0): user IBs execute
 * unprivileged and fault on them, which protects the ring and memptrs.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(SGPU_HOST)
#include "sweet_gpu.h"

#define R(dw)   ((uint32_t)(dw) * 4u)   /* dword offset -> byte offset */

/* ---- GPU registers (dword offsets, a6xx.xml) ---- */
#define RBBM_GBIF_CLIENT_QOS_CNTL       0x00011
#define RBBM_GBIF_HALT                  0x00016
#define RBBM_GBIF_HALT_ACK              0x00017
#define RBBM_INTERFACE_HANG_INT_CNTL    0x0001f
#define RBBM_INT_CLEAR_CMD              0x00037
#define RBBM_INT_0_MASK                 0x00038
#define RBBM_SW_RESET_CMD               0x00043
#define RBBM_CLOCK_CNTL                 0x000ae
#define RBBM_INT_0_STATUS               0x00201
#define RBBM_STATUS                     0x00210
#define RBBM_STATUS3                    0x00213
#define RBBM_PERFCTR_CP_0               0x00400
#define RBBM_PERFCTR_CNTL               0x00500
#define RBBM_PERFCTR_GPU_BUSY_MASKED    0x0050b
#define RBBM_NC_MODE_CNTL               0x00534
#define CP_RB_BASE                      0x00800
#define CP_RB_CNTL                      0x00802
#define CP_RB_RPTR_ADDR                 0x00804
#define CP_RB_RPTR                      0x00806
#define CP_RB_WPTR                      0x00807
#define CP_SQE_CNTL                     0x00808
#define CP_HW_FAULT                     0x00821
#define CP_INTERRUPT_STATUS             0x00823
#define CP_PROTECT_STATUS               0x00824
#define CP_SQE_INSTR_BASE               0x00830
#define CP_ADDR_MODE_CNTL               0x00842
#define CP_APRIV_CNTL                   0x00844
#define CP_PROTECT_CNTL                 0x0084f
#define CP_PROTECT(n)                   (0x00850 + (n))
#define CP_SCRATCH_REG(n)               (0x00883 + (n))
#define CP_ROQ_THRESHOLDS_1             0x008c1
#define CP_ROQ_THRESHOLDS_2             0x008c2
#define CP_MEM_POOL_SIZE                0x008c3
#define CP_PERFCTR_CP_SEL_0             0x008d0
#define CP_SQE_STAT_ADDR                0x00908
#define CP_SQE_STAT_DATA                0x00909
#define CP_IB1_BASE                     0x00928
#define CP_IB1_REM_SIZE                 0x0092a
#define CP_IB2_BASE                     0x0092b
#define CP_IB2_REM_SIZE                 0x0092d
#define CP_ALWAYS_ON_COUNTER            0x00980
#define CP_AHB_CNTL                     0x0098d
#define VSC_ADDR_MODE_CNTL              0x00c01
#define UCHE_ADDR_MODE_CNTL             0x00e00
#define UCHE_MODE_CNTL                  0x00e01
#define UCHE_WRITE_RANGE_MAX            0x00e05
#define UCHE_WRITE_THRU_BASE            0x00e07
#define UCHE_TRAP_BASE                  0x00e09
#define UCHE_CACHE_WAYS                 0x00e17
#define UCHE_FILTER_CNTL                0x00e18
#define UCHE_CLIENT_PF                  0x00e19
#define GBIF_QSB_SIDE(n)                (0x03c03 + (n))
#define GBIF_HALT                       0x03c45
#define GBIF_HALT_ACK                   0x03c46
#define GRAS_ADDR_MODE_CNTL             0x08601
#define RB_ADDR_MODE_CNTL               0x08e05
#define RB_NC_MODE_CNTL                 0x08e08
#define VPC_ADDR_MODE_CNTL              0x09601
#define PC_DBG_ECO_CNTL                 0x09e00
#define PC_ADDR_MODE_CNTL               0x09e01
#define VFD_ADDR_MODE_CNTL              0x0a601
#define SP_ADDR_MODE_CNTL               0x0ae01
#define SP_NC_MODE_CNTL                 0x0ae02
#define TPL1_ADDR_MODE_CNTL             0x0b601
#define TPL1_NC_MODE_CNTL               0x0b604
#define TPL1_BICUBIC_WEIGHTS_TABLE(n)   (0x0b608 + (n))
#define HLSQ_ADDR_MODE_CNTL             0x0be05
#define RBBM_SECVID_TRUST_CNTL          0x0f400
#define RBBM_SECVID_TSB_TRUSTED_BASE    0x0f800
#define RBBM_SECVID_TSB_TRUSTED_SIZE    0x0f802
#define RBBM_SECVID_TSB_CNTL            0x0f803
#define RBBM_SECVID_TSB_ADDR_MODE_CNTL  0x0f810

/* ---- GMU registers touched by hw_init (byte offsets from 0x03d6a000) ---- */
#define GMU_GX_SPTPRAC_CLOCK_CONTROL    0x00200
#define GMU_POWER_COUNTER_ENABLE        0x14100
#define GMU_POWER_COUNTER_SELECT_0      0x14104
#define GMU_CGC_MODE_CNTL               0x24c24
#define GMU_CGC_DELAY_CNTL              0x24c28
#define GMU_CGC_HYST_CNTL               0x24c2c
#define GMU_CX_BUSY_STATUS              0x24c30
#define GMU_CX_BUSY_MASK                0x24c38

/* ---- PM4 ---- */
#define CP_NOP                  0x10
#define CP_WAIT_REG_MEM         0x3c
#define CP_MEM_WRITE            0x3d
#define CP_REG_TO_MEM           0x3e
#define CP_INDIRECT_BUFFER      0x3f
#define CP_EVENT_WRITE          0x46
#define CP_ME_INIT              0x48
#define CP_SET_SECURE_MODE      0x66
#define EV_CACHE_FLUSH_TS       4
#define EV_PC_CCU_INVAL_DEPTH   24
#define EV_PC_CCU_INVAL_COLOR   25
#define EV_IRQ                  (1u << 31)

#define A6XX_INT_MASK           0x03d0e242u
#define INT_CP_HW_ERROR         (1u << 9)
#define INT_CP_CACHE_FLUSH_TS   (1u << 20)
#define INT_RBBM_HANG_DETECT    (1u << 23)

/* ---- kernel buffers ---- */
#define RB_BYTES                0x8000u         /* 32 KiB = 8192 dwords */
#define RB_CNTL_VALUE           0x0000020cu     /* BUFSZ 12, BLKSZ 2; no NO_UPDATE (APRIV) */
#define MEMPTRS_BYTES           0x4000u
#define MP_RPTR_SHADOW          0x000           /* CP_RB_RPTR_ADDR target */
#define MP_FENCE                0x004           /* ring fence (CACHE_FLUSH_TS) */
#define MP_STATS                0x010           /* 64 x { cp start, cp end, ao start, ao end } */
#define MP_CTX                  0x1000          /* per-context slots, 16 bytes each */
#define MP_CTX_EOP(i)           (MP_CTX + 16u * (i))
#define KERNEL_CTX              0               /* slot 0: kernel self-test */

static inline uint32_t gpu_rd(struct sgpu *g, uint32_t dw) { return sgpu_rd(g->gpu, R(dw)); }
static inline void gpu_wr(struct sgpu *g, uint32_t dw, uint32_t v) { sgpu_wr(g->gpu, R(dw), v); }
static inline void gpu_wr64(struct sgpu *g, uint32_t dw, uint64_t v)
{
	gpu_wr(g, dw, (uint32_t)v);
	gpu_wr(g, dw + 1, (uint32_t)(v >> 32));
}

static inline uint32_t
pm4_parity(uint32_t v)
{
	return (0x9669u >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^
	       v >> 24 ^ v >> 28))) & 1u;
}

static inline uint32_t
pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | (cnt & 0x7fff) | pm4_parity(cnt) << 15 | (op & 0x7f) << 16 |
	       pm4_parity(op) << 23;
}

static inline uint32_t
pkt4(uint32_t reg, uint32_t cnt)
{
	return 0x40000000u | (cnt & 0x7f) | pm4_parity(cnt) << 7 | (reg & 0x3ffff) << 8 |
	       pm4_parity(reg) << 27;
}

/* ------------------------------------------------------------------ */
/* static tables (spec 1.2 / 1.3)                                     */
/* ------------------------------------------------------------------ */

static const uint32_t a650_hwcg[][2] = {
	{ 0x000b0, 0x02222222 }, { 0x000b4, 0x02222220 }, { 0x000b8, 0x00000080 },
	{ 0x000bc, 0x0000f3cf }, { 0x000c0, 0x02222222 }, { 0x000c4, 0x22222222 },
	{ 0x000c8, 0x22222222 }, { 0x000cc, 0x00022222 }, { 0x000d0, 0x11111111 },
	{ 0x000d4, 0x11111111 }, { 0x000d8, 0x11111111 }, { 0x000dc, 0x00011111 },
	{ 0x000e0, 0x77777777 }, { 0x000e4, 0x77777777 }, { 0x000e8, 0x77777777 },
	{ 0x000ec, 0x00077777 }, { 0x000f0, 0x22222222 }, { 0x000f4, 0x01002222 },
	{ 0x000f8, 0x00002220 }, { 0x00100, 0x00040f00 }, { 0x00104, 0x25222022 },
	{ 0x00105, 0x00005555 }, { 0x00106, 0x00000011 }, { 0x00107, 0x00445044 },
	{ 0x00108, 0x04222222 }, { 0x00111, 0x00002222 }, { 0x00114, 0x00222222 },
	{ 0x00117, 0x00000002 }, { 0x0011b, 0x00002222 }, { 0x00109, 0x00004000 },
	{ 0x00112, 0x00002222 }, { 0x00115, 0x00000200 }, { 0x0011c, 0x00000000 },
	{ 0x0010a, 0x00000000 }, { 0x00113, 0x00000000 }, { 0x00116, 0x04104004 },
	{ 0x0011d, 0x00000000 }, { 0x00120, 0x00000222 }, { 0x00121, 0x00000111 },
	{ 0x00122, 0x00000777 }, { 0x0010b, 0x22222222 }, { 0x00110, 0x00000004 },
	{ 0x0010f, 0x00000002 }, { 0x00533, 0x00000182 }, { 0x00044, 0x00000000 },
	{ 0x00042, 0x00000000 }, { 0x00118, 0x00000222 }, { 0x00119, 0x00000111 },
	{ 0x0011a, 0x00000555 },
};
#define RBBM_CLOCK_CNTL_ON      0x8aa8aa82u

/* entries 0..37 -> CP_PROTECT(i); the last one -> CP_PROTECT(47) (infinite span) */
static const uint32_t a650_protect[] = {
	0x13fc0000, 0x00140501, 0x0bd0050b, 0x8000050e, 0x80000510, 0x80000534,
	0x82080800, 0x802008a0, 0x809008ab, 0x02b808de, 0x81340900, 0x89c8098d,
	0x80040e00, 0x80300e03, 0x830c3c00, 0x7ffc3cc4, 0x873c8630, 0x80008e00,
	0x80008e08, 0x807c8e50, 0x89fc8e80, 0x876c9624, 0x80449e60, 0x861c9e78,
	0x873ca630, 0x8000ae02, 0x8cbcae50, 0x8000b604, 0x801cb608, 0x8004be02,
	0xdf7cbe20, 0xaffcf000, 0x7ffcfc00, 0xfffd8400, 0xfffda800, 0x910df400,
	0x01edf844, 0x806df887,
	0x8001f8c0,
};

/* ------------------------------------------------------------------ */
/* ring                                                               */
/* ------------------------------------------------------------------ */

static volatile uint8_t *mp(struct sgpu *g) { return (volatile uint8_t *)g->memptrs.dma.va; }

static uint32_t
rptr(struct sgpu *g)
{
	return sgpu_mem_rd32(mp(g) + MP_RPTR_SHADOW) % g->rb_dwords;
}

static uint32_t
ring_free(struct sgpu *g)
{
	return (rptr(g) + g->rb_dwords - 1 - g->rb_wptr) % g->rb_dwords;
}

static int
ring_reserve(struct sgpu *g, uint32_t dwords)
{
	uint64_t start = sgpu_now_us();
	while (ring_free(g) < dwords) {
		if (sgpu_now_us() - start > 1000000) {
			return sgpu_fail(g, -16, "cp: ring full (rptr %u wptr %u, %u needed)",
			           rptr(g), g->rb_wptr, dwords);
		}
		sgpu_udelay(10);
	}
	return 0;
}

static inline void
emit(struct sgpu *g, uint32_t v)
{
	volatile uint32_t *rb = (volatile uint32_t *)g->rb.dma.va;
	rb[g->rb_wptr] = v;
	g->rb_wptr = (g->rb_wptr + 1) % g->rb_dwords;
}

static inline void
emit64(struct sgpu *g, uint64_t v)
{
	emit(g, (uint32_t)v);
	emit(g, (uint32_t)(v >> 32));
}

static void
ring_flush(struct sgpu *g)
{
	sgpu_wmb();                     /* ring contents before the doorbell */
	gpu_wr(g, CP_RB_WPTR, g->rb_wptr);
}

static void
dump_state(struct sgpu *g, const char *why)
{
	uint32_t cpint = gpu_rd(g, CP_INTERRUPT_STATUS);

	gpu_wr(g, CP_SQE_STAT_ADDR, 1);
	sgpu_log("cp: %s: RBBM_STATUS %#x STATUS3 %#x INT_0 %#x CX_BUSY %#x rptr %u (reg %u) "
	    "wptr %u fence %u/%u", why, gpu_rd(g, RBBM_STATUS), gpu_rd(g, RBBM_STATUS3),
	    gpu_rd(g, RBBM_INT_0_STATUS), sgpu_rd(g->gmu, GMU_CX_BUSY_STATUS), rptr(g),
	    gpu_rd(g, CP_RB_RPTR), g->rb_wptr, sgpu_cp_ring_fence_retired(g), g->fence_seqno);
	sgpu_log("cp:   CP_INTERRUPT_STATUS %#x HW_FAULT %#x PROTECT_STATUS %#x SQE_STAT %#x "
	    "IB1 %#x%08x rem %u IB2 %#x%08x rem %u SCRATCH2 %u", cpint, gpu_rd(g, CP_HW_FAULT),
	    gpu_rd(g, CP_PROTECT_STATUS), gpu_rd(g, CP_SQE_STAT_DATA),
	    gpu_rd(g, CP_IB1_BASE + 1), gpu_rd(g, CP_IB1_BASE), gpu_rd(g, CP_IB1_REM_SIZE),
	    gpu_rd(g, CP_IB2_BASE + 1), gpu_rd(g, CP_IB2_BASE), gpu_rd(g, CP_IB2_REM_SIZE),
	    gpu_rd(g, CP_SCRATCH_REG(2)));
	sgpu_smmu_check_faults(g);
	sgpu_gmu_irq(g);
}

/* a6xx_idle: ring drained, then GPU and CX not busy, no hang detect */
static int
cp_idle(struct sgpu *g, const char *what)
{
	uint64_t start = sgpu_now_us();

	while (rptr(g) != g->rb_wptr) {
		if (sgpu_now_us() - start > 1000000) {
			dump_state(g, what);
			return sgpu_fail(g, -60, "cp: %s: ring did not drain", what);
		}
		sgpu_udelay(10);
	}
	for (;;) {
		bool busy = (sgpu_rd(g->gmu, GMU_CX_BUSY_STATUS) & (1u << 23)) ||
		    (gpu_rd(g, RBBM_STATUS) & ~1u);
		if (gpu_rd(g, RBBM_INT_0_STATUS) & INT_RBBM_HANG_DETECT) {
			dump_state(g, what);
			return sgpu_fail(g, -5, "cp: %s: hang detect", what);
		}
		if (!busy) {
			return 0;
		}
		if (sgpu_now_us() - start > 1000000) {
			dump_state(g, what);
			return sgpu_fail(g, -60, "cp: %s: GPU stays busy", what);
		}
		sgpu_udelay(10);
	}
}

/* ------------------------------------------------------------------ */
/* one-time buffers                                                   */
/* ------------------------------------------------------------------ */

static int
cp_buffers(struct sgpu *g)
{
	int err;

	if (g->rb.dma.va != NULL) {
		return 0;
	}
	if (g->sqe_fw_len < 8 || (g->sqe_fw_len & 3)) {
		return sgpu_fail(g, -22, "cp: sqe firmware is %u bytes", g->sqe_fw_len);
	}
	/* SQE: the file minus its first dword; GPU read-only, NOT privileged
	 * (CP_APRIV_CNTL.ICACHE is off on A650) */
	uint32_t sqe_len = g->sqe_fw_len - 4;
	uint32_t ver = (uint32_t)g->sqe_fw[4] | (uint32_t)g->sqe_fw[5] << 8 |
	    (uint32_t)g->sqe_fw[6] << 16 | (uint32_t)g->sqe_fw[7] << 24;
	if ((ver & 0xfff) < 0x095) {
		return sgpu_fail(g, -1, "cp: a650_sqe.fw version %#x is older than 0.95 "
		           "(security fixes required)", ver & 0xfff);
	}
	if ((err = sgpu_bo_alloc(g, sqe_len, SGPU_PROT_READ, &g->sqe)) != 0) {
		return sgpu_fail(g, err, "cp: no memory for the SQE microcode");
	}
	volatile uint8_t *dst = g->sqe.dma.va;
	for (uint32_t i = 0; i < sqe_len; i++) {
		dst[i] = g->sqe_fw[4 + i];
	}
	if ((err = sgpu_bo_alloc(g, RB_BYTES, SGPU_PROT_READ | SGPU_PROT_PRIV, &g->rb)) != 0) {
		return sgpu_fail(g, err, "cp: no memory for the ringbuffer");
	}
	if ((err = sgpu_bo_alloc(g, MEMPTRS_BYTES,
	    SGPU_PROT_READ | SGPU_PROT_WRITE | SGPU_PROT_PRIV, &g->memptrs)) != 0) {
		return sgpu_fail(g, err, "cp: no memory for memptrs");
	}
	g->rb_dwords = RB_BYTES / 4;
	sgpu_wmb();
	sgpu_smmu_tlb_sync(g, SGPU_DOM_GPU);
	sgpu_log("cp: SQE version %#x, %u bytes at %#llx; ring %#llx; memptrs %#llx", ver,
	    sqe_len, (unsigned long long)g->sqe.iova, (unsigned long long)g->rb.iova,
	    (unsigned long long)g->memptrs.iova);
	return 0;
}

/* ------------------------------------------------------------------ */
/* hw_init (spec 1.1 steps 4..69; 1..3 = OOB GPU_SET, done at GX stage) */
/* ------------------------------------------------------------------ */

static void
set_hwcg(struct sgpu *g)
{
	sgpu_wr(g->gmu, GMU_CGC_MODE_CNTL, 0x00020202);
	sgpu_wr(g->gmu, GMU_CGC_DELAY_CNTL, 0x00010111);
	sgpu_wr(g->gmu, GMU_CGC_HYST_CNTL, 0x00005555);
	if (gpu_rd(g, RBBM_CLOCK_CNTL) == RBBM_CLOCK_CNTL_ON) {
		return;
	}
	sgpu_rmw(g->gmu, GMU_GX_SPTPRAC_CLOCK_CONTROL, 1, 0);
	for (uint32_t i = 0; i < sizeof(a650_hwcg) / sizeof(a650_hwcg[0]); i++) {
		gpu_wr(g, a650_hwcg[i][0], a650_hwcg[i][1]);
	}
	sgpu_rmw(g->gmu, GMU_GX_SPTPRAC_CLOCK_CONTROL, 0, 1);
	gpu_wr(g, RBBM_CLOCK_CNTL, RBBM_CLOCK_CNTL_ON);
}

static void
set_cp_protect(struct sgpu *g)
{
	const uint32_t n = sizeof(a650_protect) / sizeof(a650_protect[0]);

	gpu_wr(g, CP_PROTECT_CNTL, 0x0000000b);         /* EN | FAULT_ON_VIOL | LAST_SPAN_INF */
	for (uint32_t i = 0; i + 1 < n; i++) {
		gpu_wr(g, CP_PROTECT(i), a650_protect[i]);
	}
	gpu_wr(g, CP_PROTECT(47), a650_protect[n - 1]);
}

static void
hw_init_regs(struct sgpu *g)
{
	/* 4-7: release GBIF halt (posting reads) */
	gpu_wr(g, GBIF_HALT, 0);
	(void)gpu_rd(g, GBIF_HALT);
	gpu_wr(g, RBBM_GBIF_HALT, 0);
	(void)gpu_rd(g, RBBM_GBIF_HALT);
	/* 8-10: no trusted (secure) buffer */
	gpu_wr(g, RBBM_SECVID_TSB_CNTL, 0);
	gpu_wr64(g, RBBM_SECVID_TSB_TRUSTED_BASE, 0);
	gpu_wr(g, RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
	/* 11-22: 64-bit addressing everywhere */
	gpu_wr(g, CP_ADDR_MODE_CNTL, 1);
	gpu_wr(g, VSC_ADDR_MODE_CNTL, 1);
	gpu_wr(g, GRAS_ADDR_MODE_CNTL, 1);
	gpu_wr(g, RB_ADDR_MODE_CNTL, 1);
	gpu_wr(g, PC_ADDR_MODE_CNTL, 1);
	gpu_wr(g, HLSQ_ADDR_MODE_CNTL, 1);
	gpu_wr(g, VFD_ADDR_MODE_CNTL, 1);
	gpu_wr(g, VPC_ADDR_MODE_CNTL, 1);
	gpu_wr(g, UCHE_ADDR_MODE_CNTL, 1);
	gpu_wr(g, SP_ADDR_MODE_CNTL, 1);
	gpu_wr(g, TPL1_ADDR_MODE_CNTL, 1);
	gpu_wr(g, RBBM_SECVID_TSB_ADDR_MODE_CNTL, 1);
	/* 23: hardware clock gating */
	set_hwcg(g);
	/* 24-28: GBIF QoS */
	for (int i = 0; i < 4; i++) {
		gpu_wr(g, GBIF_QSB_SIDE(i), 0x00071620);
	}
	gpu_wr(g, RBBM_GBIF_CLIENT_QOS_CNTL, 0x3);
	/* 29 */
	gpu_wr(g, RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
	/* 30-32: UCHE trap / write-through window (our unmapped top page) */
	gpu_wr64(g, UCHE_WRITE_RANGE_MAX, SGPU_UCHE_TRAP_BASE + 0xfc0);
	gpu_wr64(g, UCHE_TRAP_BASE, SGPU_UCHE_TRAP_BASE);
	gpu_wr64(g, UCHE_WRITE_THRU_BASE, SGPU_UCHE_TRAP_BASE);
	/* (UCHE_GMEM_RANGE_* not written on the a650 family) */
	/* 33-38 */
	gpu_wr(g, UCHE_FILTER_CNTL, 0x804);
	gpu_wr(g, UCHE_CACHE_WAYS, 0x4);
	gpu_wr(g, CP_ROQ_THRESHOLDS_2, 0x02000140);
	gpu_wr(g, CP_ROQ_THRESHOLDS_1, 0x8040362c);
	gpu_wr(g, CP_MEM_POOL_SIZE, 128);
	gpu_wr(g, PC_DBG_ECO_CNTL, 0x00300200);
	/* 39-41 */
	gpu_wr(g, CP_AHB_CNTL, 0x1);
	gpu_wr(g, RBBM_PERFCTR_CNTL, 0x1);
	gpu_wr(g, CP_PERFCTR_CP_SEL_0, 0);              /* PERF_CP_ALWAYS_COUNT */
	/* 42-46: UBWC 4.0, highest bank bit 16, swizzle 6, macrotile */
	gpu_wr(g, RB_NC_MODE_CNTL, 0x00000816);
	gpu_wr(g, TPL1_NC_MODE_CNTL, 0x00000006);
	gpu_wr(g, SP_NC_MODE_CNTL, 0x00000026);
	gpu_wr(g, UCHE_MODE_CNTL, 0x00600000);
	gpu_wr(g, RBBM_NC_MODE_CNTL, 1);
	/* 47-53 */
	gpu_wr(g, RBBM_INTERFACE_HANG_INT_CNTL, (1u << 30) | 0x1fffff);
	gpu_wr(g, UCHE_CLIENT_PF, 0x81);
	gpu_wr(g, TPL1_BICUBIC_WEIGHTS_TABLE(0), 0);
	gpu_wr(g, TPL1_BICUBIC_WEIGHTS_TABLE(1), 0x3fe05ff4);
	gpu_wr(g, TPL1_BICUBIC_WEIGHTS_TABLE(2), 0x3fa0ebee);
	gpu_wr(g, TPL1_BICUBIC_WEIGHTS_TABLE(3), 0x3f5193ed);
	gpu_wr(g, TPL1_BICUBIC_WEIGHTS_TABLE(4), 0x3f0243f0);
	/* 54-56: GMU busy/power counters */
	sgpu_wr(g->gmu, GMU_CX_BUSY_MASK, 0xff000000);
	sgpu_rmw(g->gmu, GMU_POWER_COUNTER_SELECT_0, 0xff, 0x20);
	sgpu_wr(g->gmu, GMU_POWER_COUNTER_ENABLE, 1);
	/* 57 */
	set_cp_protect(g);
	/* 58: privileged ring fetch, rptr writeback, ring packet memory accesses */
	gpu_wr(g, CP_APRIV_CNTL, 0x0000006e);
	/* 59 */
	gpu_wr(g, RBBM_INT_0_MASK, A6XX_INT_MASK);
	/* 60: adreno_hw_init. The SCM CP-SMMU-aperture call is only needed
	 * for CP_SMMU_TABLE_UPDATE, which this driver does not use. */
	sgpu_mem_wr32(mp(g) + MP_RPTR_SHADOW, 0);
	/* 61-64 */
	gpu_wr64(g, CP_SQE_INSTR_BASE, g->sqe.iova);
	gpu_wr64(g, CP_RB_BASE, g->rb.iova);
	gpu_wr(g, CP_RB_CNTL, RB_CNTL_VALUE);
	gpu_wr64(g, CP_RB_RPTR_ADDR, g->memptrs.iova + MP_RPTR_SHADOW);
	sgpu_mem_wr32(mp(g) + MP_RPTR_SHADOW, 0);
	g->rb_wptr = 0;
	sgpu_wmb();
	/* 66: start the SQE */
	gpu_wr(g, CP_SQE_CNTL, 1);
}

static int
cp_me_init(struct sgpu *g)
{
	emit(g, pkt7(CP_ME_INIT, 8));
	emit(g, 0x0000002f);            /* fields present: multi-ctx, err detect, hdr dump, reset, wa */
	emit(g, 0x00000003);            /* multiple hardware contexts */
	emit(g, 0x20000000);            /* error detection */
	emit(g, 0);                     /* no header dump */
	emit(g, 0);
	emit(g, 0);                     /* no ucode workarounds */
	emit(g, 0);
	emit(g, 0);
	ring_flush(g);
	return cp_idle(g, "CP_ME_INIT");
}

static int
secure_exit(struct sgpu *g)
{
	/*
	 * The GPU comes out of reset in secure mode. With a zap shader
	 * loaded (TrustZone PAS id 13) the CP leaves it itself; production
	 * TrustZone usually blocks the CPU from writing SECVID_TRUST_CNTL.
	 */
	int zerr = g->zap_fw ? sgpu_zap_load(g) : -2;
	if (zerr == 0) {
		emit(g, pkt7(CP_SET_SECURE_MODE, 1));
		emit(g, 0);
		ring_flush(g);
		return cp_idle(g, "CP_SET_SECURE_MODE");
	}
	g->last_error = 0;              /* zap failure is not fatal by itself */
	sgpu_log("cp: no zap shader (%d): writing RBBM_SECVID_TRUST_CNTL = 0 directly; "
	    "if TrustZone protects it, an XPU violation follows", zerr);
	gpu_wr(g, RBBM_SECVID_TRUST_CNTL, 0);
	return 0;
}

static int
hw_init(struct sgpu *g)
{
	int err;

	hw_init_regs(g);
	if ((err = cp_me_init(g)) != 0) {
		return err;
	}
	if ((err = secure_exit(g)) != 0) {
		return err;
	}
	return 0;
}

int
sgpu_cp_init(struct sgpu *g)
{
	int err;

	if ((err = cp_buffers(g)) != 0) {
		sgpu_gmu_oob_clear(g, SGPU_OOB_GPU_SET);
		return err;
	}
	err = hw_init(g);
	/* step 69: release the GPU_SET OOB taken at stage GX */
	sgpu_gmu_oob_clear(g, SGPU_OOB_GPU_SET);
	if (err) {
		return err;
	}
	sgpu_log("cp: running: RBBM_STATUS %#x, rptr %u, CLOCK_CNTL %#x, SECVID_TRUST %#x",
	    gpu_rd(g, RBBM_STATUS), rptr(g), gpu_rd(g, RBBM_CLOCK_CNTL),
	    gpu_rd(g, RBBM_SECVID_TRUST_CNTL));
	g->ctx[KERNEL_CTX].used = true;
	return 0;
}

/* ------------------------------------------------------------------ */
/* contexts and submission                                            */
/* ------------------------------------------------------------------ */

static uint64_t
ctx_eop_iova(struct sgpu *g, uint32_t c)
{
	return g->memptrs.iova + MP_CTX_EOP(c);
}

int
sgpu_cp_ctx_alloc(struct sgpu *g, uint32_t *ctx_id)
{
	for (uint32_t i = 1; i < SGPU_MAX_CTX; i++) {
		if (!g->ctx[i].used) {
			/* start the timeline where the slot already is, so a stale
			 * CACHE_FLUSH_TS from a previous owner can never complete
			 * a new timestamp early */
			g->ctx[i].used = true;
			g->ctx[i].queued = sgpu_mem_rd32(mp(g) + MP_CTX_EOP(i));
			*ctx_id = i;
			return 0;
		}
	}
	return -28;
}

void
sgpu_cp_ctx_free(struct sgpu *g, uint32_t ctx_id)
{
	if (ctx_id > 0 && ctx_id < SGPU_MAX_CTX) {
		g->ctx[ctx_id].used = false;
	}
}

uint32_t
sgpu_cp_retired(struct sgpu *g, uint32_t ctx_id)
{
	if (ctx_id >= SGPU_MAX_CTX || g->memptrs.dma.va == NULL) {
		return 0;
	}
	return sgpu_mem_rd32(mp(g) + MP_CTX_EOP(ctx_id));
}

uint32_t
sgpu_cp_queued(struct sgpu *g, uint32_t ctx_id)
{
	return ctx_id < SGPU_MAX_CTX ? g->ctx[ctx_id].queued : 0;
}

uint32_t
sgpu_cp_ring_fence_submitted(struct sgpu *g)
{
	return g->fence_seqno;
}

uint32_t
sgpu_cp_ring_fence_retired(struct sgpu *g)
{
	return g->memptrs.dma.va ? sgpu_mem_rd32(mp(g) + MP_FENCE) : g->fence_seqno;
}

static void
emit_stats(struct sgpu *g, uint32_t idx, bool end)
{
	uint64_t base = g->memptrs.iova + MP_STATS + 32u * idx + (end ? 8 : 0);

	emit(g, pkt7(CP_REG_TO_MEM, 3));
	emit(g, RBBM_PERFCTR_CP_0 | (2u << 18) | (1u << 30));
	emit64(g, base);
	emit(g, pkt7(CP_REG_TO_MEM, 3));
	emit(g, CP_ALWAYS_ON_COUNTER | (2u << 18) | (1u << 30));
	emit64(g, base + 16);
}

static void
emit_timestamp(struct sgpu *g, uint64_t iova, uint32_t value, bool irq)
{
	emit(g, pkt7(CP_EVENT_WRITE, 4));
	emit(g, EV_CACHE_FLUSH_TS | (irq ? EV_IRQ : 0));
	emit64(g, iova);
	emit(g, value);
}

int
sgpu_cp_submit(struct sgpu *g, uint32_t ctx_id, const struct sgpu_ib *ibs,
    uint32_t n, uint32_t flags, uint32_t *ts_out)
{
	int err;

	(void)flags;
	if (g->stage_reached < SGPU_STAGE_CP || g->rb.dma.va == NULL) {
		return -19;
	}
	if (ctx_id >= SGPU_MAX_CTX || !g->ctx[ctx_id].used || n > SGPU_MAX_IBS) {
		return -22;
	}
	if ((err = ring_reserve(g, 27 + 4 * n + 7)) != 0) {
		return err;
	}
	sgpu_dcvs_busy(g);
	uint32_t seqno = ++g->fence_seqno;
	uint32_t ts = ++g->ctx[ctx_id].queued;
	uint32_t idx = seqno % 64;

	emit_stats(g, idx, false);
	emit(g, pkt7(CP_EVENT_WRITE, 1));
	emit(g, EV_PC_CCU_INVAL_DEPTH);
	emit(g, pkt7(CP_EVENT_WRITE, 1));
	emit(g, EV_PC_CCU_INVAL_COLOR);
	for (uint32_t i = 0; i < n; i++) {
		emit(g, pkt7(CP_INDIRECT_BUFFER, 3));
		emit64(g, ibs[i].gpuaddr);
		emit(g, ibs[i].size_dwords & 0xfffff);
	}
	emit_stats(g, idx, true);
	emit(g, pkt4(CP_SCRATCH_REG(2), 1));
	emit(g, seqno);
	emit_timestamp(g, ctx_eop_iova(g, ctx_id), ts, false);
	emit_timestamp(g, g->memptrs.iova + MP_FENCE, seqno, true);
	ring_flush(g);
	*ts_out = ts;
	return 0;
}

/* ------------------------------------------------------------------ */
/* self-test (stage 7)                                                */
/* ------------------------------------------------------------------ */

int
sgpu_cp_selftest(struct sgpu *g)
{
	const uint32_t magic_ring = 0x5eed0650, magic_ib = 0xc0de0650;
	struct sgpu_ib ib;
	uint32_t ts;
	int err;

	/* scratch: [0] ring write target, [1] IB write target, [64..] the IB.
	 * Unprivileged and writable, like a user buffer, so this also proves
	 * that user IBs can be fetched and can write through the SMMU. */
	if (g->scratch.dma.va == NULL &&
	    (err = sgpu_bo_alloc(g, 0x4000, SGPU_PROT_READ | SGPU_PROT_WRITE, &g->scratch)) != 0) {
		return sgpu_fail(g, err, "selftest: no scratch buffer");
	}
	volatile uint32_t *s = g->scratch.dma.va;
	s[0] = 0xdeaddead;
	s[1] = 0xdeaddead;
	volatile uint32_t *ibp = s + 64;
	ibp[0] = pkt7(CP_MEM_WRITE, 3);
	ibp[1] = (uint32_t)(g->scratch.iova + 4);
	ibp[2] = (uint32_t)((g->scratch.iova + 4) >> 32);
	ibp[3] = magic_ib;
	ibp[4] = pkt7(CP_NOP, 0);
	sgpu_wmb();

	/* 1: a CP_MEM_WRITE from the ring itself */
	if ((err = ring_reserve(g, 8)) != 0) {
		return err;
	}
	emit(g, pkt7(CP_MEM_WRITE, 3));
	emit64(g, g->scratch.iova);
	emit(g, magic_ring);
	ring_flush(g);

	/* 2: a normal submission with one IB, timestamp on the kernel context */
	uint64_t t0 = sgpu_cp_always_on(g);
	ib.gpuaddr = g->scratch.iova + 64 * 4;
	ib.size_dwords = 5;
	ib.flags = 0;
	if ((err = sgpu_cp_submit(g, KERNEL_CTX, &ib, 1, 0, &ts)) != 0) {
		return sgpu_fail(g, err, "selftest: submit failed");
	}
	uint64_t start = sgpu_now_us();
	while (!sgpu_ts_ge(sgpu_cp_retired(g, KERNEL_CTX), ts)) {
		if (sgpu_now_us() - start > 1000000) {
			dump_state(g, "selftest");
			return sgpu_fail(g, -60, "selftest: timestamp %u never retired "
			           "(slot %u, ring write %#x, IB write %#x)", ts,
			           sgpu_cp_retired(g, KERNEL_CTX), sgpu_mem_rd32(&s[0]),
			           sgpu_mem_rd32(&s[1]));
		}
		sgpu_udelay(10);
	}
	uint64_t us = sgpu_now_us() - start;
	uint64_t t1 = sgpu_cp_always_on(g);
	uint32_t r0 = sgpu_mem_rd32(&s[0]), r1 = sgpu_mem_rd32(&s[1]);
	if (r0 != magic_ring || r1 != magic_ib) {
		return sgpu_fail(g, -5, "selftest: ring write %#x (want %#x), IB write %#x "
		           "(want %#x)", r0, magic_ring, r1, magic_ib);
	}
	sgpu_log("selftest: OK - ring and IB writes landed, ts %u retired in %llu us, "
	    "fence %u, always-on %llu -> %llu", ts, (unsigned long long)us,
	    sgpu_cp_ring_fence_retired(g), (unsigned long long)t0, (unsigned long long)t1);
	return sgpu_smmu_check_faults(g) ? sgpu_fail(g, -5, "selftest: SMMU faults seen") : 0;
}

/* ------------------------------------------------------------------ */
/* time, status, interrupts, hangs                                    */
/* ------------------------------------------------------------------ */

uint64_t
sgpu_cp_always_on(struct sgpu *g)
{
	if (g->stage_reached < SGPU_STAGE_GX) {
		return 0;
	}
	/* keep GX up while reading (OOB PERFCOUNTER); LO read latches HI */
	bool oob = sgpu_gmu_oob_set(g, SGPU_OOB_PERFCOUNTER) == 0;
	uint32_t lo = gpu_rd(g, CP_ALWAYS_ON_COUNTER);
	uint32_t hi = gpu_rd(g, CP_ALWAYS_ON_COUNTER + 1);
	if (oob) {
		sgpu_gmu_oob_clear(g, SGPU_OOB_PERFCOUNTER);
	}
	return ((uint64_t)hi << 32) | lo;
}

void
sgpu_cp_status(struct sgpu *g, struct sgpu_status *st)
{
	uint32_t i;

	/* no GPU/SMMU interrupt is wired yet: collect pending faults here */
	sgpu_cp_irq(g);
	sgpu_smmu_check_faults(g);
	sgpu_gmu_irq(g);
	st->stage_reached = g->stage_reached;
	st->stage_requested = g->stage_requested;
	st->last_error = g->last_error;
	st->faults = g->faults;
	st->hangs = g->hangs;
	st->gmu_state = sgpu_gmu_state(g);
	if (g->stage_reached >= SGPU_STAGE_CP) {
		st->rbbm_status = gpu_rd(g, RBBM_STATUS);
		st->cp_rb_rptr = rptr(g);
		st->cp_rb_wptr = g->rb_wptr;
	}
	for (i = 0; i + 1 < sizeof(st->message) && g->message[i]; i++) {
		st->message[i] = g->message[i];
	}
	st->message[i] = 0;
}

uint32_t
sgpu_cp_irq(struct sgpu *g)
{
	if (g->stage_reached < SGPU_STAGE_CP) {
		return 0;
	}
	uint32_t status = gpu_rd(g, RBBM_INT_0_STATUS);
	if (status == 0) {
		return 0;
	}
	gpu_wr(g, RBBM_INT_CLEAR_CMD, status);          /* ack first, then dispatch */
	if (status & INT_CP_HW_ERROR) {
		uint32_t s = gpu_rd(g, CP_INTERRUPT_STATUS);
		if (s & 1) {
			gpu_wr(g, CP_SQE_STAT_ADDR, 1);
			sgpu_log("cp: opcode error, SQE_STAT %#x", gpu_rd(g, CP_SQE_STAT_DATA));
		}
		if (s & 2) {
			sgpu_log("cp: ucode error");
		}
		if (s & 4) {
			sgpu_log("cp: hw fault %#x", gpu_rd(g, CP_HW_FAULT));
		}
		if (s & 0x10) {
			uint32_t p = gpu_rd(g, CP_PROTECT_STATUS);
			sgpu_log("cp: protected register %s at %#x", (p & (1u << 20)) ? "read" : "write",
			    p & 0x3ffff);
		}
		if (s & 0x20) {
			sgpu_log("cp: AHB error");
		}
		if (s & 0x40) {
			sgpu_log("cp: VSD parity error");
		}
		if (s & 0x80) {
			sgpu_log("cp: illegal instruction");
		}
		g->faults++;
	}
	if (status & ((1u << 1) | (1u << 6) | (1u << 22) | (1u << 24))) {
		sgpu_log("cp: RBBM_INT_0 error bits %#x", status & ((1u << 1) | (1u << 6) |
		    (1u << 22) | (1u << 24)));
	}
	if ((status & INT_RBBM_HANG_DETECT) && !(gpu_rd(g, RBBM_STATUS3) & (1u << 24))) {
		sgpu_log("cp: hardware hang detect");
	}
	return status;
}

/*
 * Recovery without power-collapsing CX (Linux collapses CX and replays):
 * halt the SQE, drain the bus, abandon every pending submission (all
 * timelines jump to their queued value, so waiters return), then rerun
 * hw_init. If that fails the GPU stays dead until reboot and every
 * submit returns -EIO (stage drops below CP).
 */
static void
cp_recover(struct sgpu *g)
{
	g->hangs++;
	dump_state(g, "hang");
	gpu_wr(g, CP_SQE_CNTL, 3);
	gpu_wr(g, GBIF_HALT, 1);
	(void)sgpu_poll(g->gpu, R(GBIF_HALT_ACK), 1, 1, 100000);
	gpu_wr(g, GBIF_HALT, 2);
	(void)sgpu_poll(g->gpu, R(GBIF_HALT_ACK), 2, 2, 100000);
	gpu_wr(g, GBIF_HALT, 0);

	for (uint32_t i = 0; i < SGPU_MAX_CTX; i++) {
		sgpu_mem_wr32(mp(g) + MP_CTX_EOP(i), g->ctx[i].queued);
	}
	sgpu_mem_wr32(mp(g) + MP_FENCE, g->fence_seqno);
	sgpu_wmb();

	int err = sgpu_gmu_oob_set(g, SGPU_OOB_GPU_SET);
	if (!err) {
		err = hw_init(g);
		sgpu_gmu_oob_clear(g, SGPU_OOB_GPU_SET);
	}
	if (err) {
		sgpu_log("cp: recovery failed (%d): GPU disabled until reboot", err);
		g->stage_reached = SGPU_STAGE_GX;
	} else {
		sgpu_log("cp: recovered after hang #%u", g->hangs);
	}
}

bool
sgpu_cp_hang_check(struct sgpu *g)
{
	if (g->stage_reached < SGPU_STAGE_CP) {
		return true;            /* nothing will ever retire */
	}
	sgpu_cp_irq(g);
	sgpu_smmu_check_faults(g);
	sgpu_gmu_irq(g);

	uint32_t fence = sgpu_cp_ring_fence_retired(g);
	uint64_t now = sgpu_now_us();
	if (fence == g->fence_seqno || fence != g->hc_fence) {
		g->hc_fence = fence;
		g->hc_us = now;
		g->hc_strikes = 0;
		return false;
	}
	if (now - g->hc_us < 250000) {  /* Linux hangcheck period */
		return false;
	}
	g->hc_us = now;
	uint32_t ib1 = gpu_rd(g, CP_IB1_BASE), rem = gpu_rd(g, CP_IB1_REM_SIZE);
	if (ib1 != g->hc_ib1 || rem != g->hc_rem) {
		g->hc_ib1 = ib1;        /* CP still walking an IB: progress */
		g->hc_rem = rem;
		g->hc_strikes = 0;
		return false;
	}
	if (++g->hc_strikes < 4) {
		return false;
	}
	g->hc_strikes = 0;
	cp_recover(g);
	return g->stage_reached < SGPU_STAGE_CP;
}
#endif
