/*
 * sweet_gpu_gmu.c - Adreno 650 GMU (graphics management unit) on SM8250.
 *
 * Spec: specs/gmu-hfi-a650.md (register-level, with Linux file:line
 * references; Linux a6xx_gmu.c/a6xx_hfi.c were documentation only).
 *
 * Why the GMU is unavoidable
 * --------------------------
 * On A6xx the CPU does not power the GPU core. The GX rail and the GX GDSC
 * belong to the GMU, a Cortex-M3 inside the CX domain, which votes RPMh
 * (via its own RSC) for GFX/MX/CX levels and DDR bandwidth and switches GX
 * on and off. The host's job is:
 *   1. program the GPU RSC and the PDC once (RPMh plumbing for the GMU),
 *   2. load a650_gmu.bin into the M3's ITCM/DTCM and into the "icache"
 *      region of GMU virtual memory (SMMU context bank, SID 5),
 *   3. hand the firmware the HFI queue table, its log buffer and the chip
 *      id, release it from reset and wait for INIT_RESULT,
 *   4. speak HFI: PERF_TABLE (GPU OPPs + RPMh ARC votes from cmd-db),
 *      BW_TABLE, CORE_FW_START, START, then GX_BW_PERF_VOTE,
 *   5. use the OOB doorbell GPU_SET whenever the CPU must touch GX
 *      registers (hw_init); the GMU acks once GX is powered.
 *
 * A650 = "non-legacy" GMU: HFI v2, new OOB bits, cold boot only, no IFPC
 * (idle level ACTIVE), PDC sequencer owned by AOP (host only enables it).
 *
 * Memory model: every buffer the GMU reads or writes is SGPU_DMA_WC
 * (Normal non-cacheable for the CPU, non-cacheable for the GMU), exactly
 * like Linux's MSM_BO_WC GMU buffers, so HFI needs only barriers.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(SGPU_HOST)
#include "sweet_gpu.h"

/* ---- GMU registers: byte offsets from 0x03d6a000 (dword offset * 4) ---- */
#define GMU_GX_SPTPRAC_CLOCK_CONTROL    0x00200
#define GMU_CM3_ITCM_START              0x03000         /* 16 KiB window */
#define GMU_CM3_DTCM_START              0x07000         /* 16 KiB window */
#define GMU_CORE_FW_VERSION             0x0afe0         /* DTCM byte 0x3fe0 */
#define GMU_ICACHE_CONFIG               0x13000
#define GMU_DCACHE_CONFIG               0x13004
#define GMU_SYS_BUS_CONFIG              0x1303c
#define GMU_CM3_SYSRESET                0x14000
#define GMU_CM3_BOOT_CONFIG             0x14004
#define GMU_CM3_FW_BUSY                 0x14068
#define GMU_CM3_FW_INIT_RESULT          0x14070
#define GMU_CM3_CFG                     0x140b4
#define GMU_POWER_COUNTER_ENABLE        0x14100
#define GMU_PWR_COL_INTER_FRAME_CTRL    0x14300
#define GMU_PWR_COL_KEEPALIVE           0x1430c
#define GMU_SPTPRAC_PWR_CLK_STATUS      0x14340
#define  GX_HM_GDSC_POWER_OFF           (1u << 6)
#define  GX_HM_CLK_OFF                  (1u << 7)
#define GMU_RPMH_CTRL                   0x143a0
#define GMU_RPMH_POWER_STATE            0x143b0
#define GMU_CX_FAL_INTF                 0x143c0
#define GMU_CX_FALNEXT_INTF             0x143c4
#define GMU_PWR_COL_CP_MSG              0x14400
#define GMU_PWR_COL_CP_RESP             0x14404
#define GMU_HFI_CTRL_STATUS             0x14600
#define GMU_HFI_VERSION_INFO            0x14604
#define GMU_HFI_SFR_ADDR                0x14608         /* carries the chip id */
#define GMU_HFI_QTBL_INFO               0x14610
#define GMU_HFI_QTBL_ADDR               0x14614
#define GMU_HFI_CTRL_INIT               0x14618
#define GMU_GMU2HOST_INTR_CLR           0x14644
#define GMU_GMU2HOST_INTR_INFO          0x14648
#define  GMU2HOST_MSGQ                  (1u << 0)
#define  GMU2HOST_CM3_FAULT             (1u << 23)
#define GMU_GMU2HOST_INTR_MASK          0x1464c
#define GMU_HOST2GMU_INTR_SET           0x14650
#define GMU_GENERAL_7                   0x14730
/* gpu_cc registers read only in gmu_dump (sweet_gpu_clk.c owns the clocks) */
#define GPUCC_CX_GMU_CBCR_DUMP          0x1098
#define GPUCC_GMU_CMD_RCGR_DUMP         0x1120
#define GMU_AO_HOST_INTERRUPT_CLR       0x24c10
#define GMU_AO_HOST_INTERRUPT_STATUS    0x24c14
#define GMU_AO_HOST_INTERRUPT_MASK      0x24c18
#define GMU_RSCC_CONTROL_REQ            0x24c1c
#define GMU_RSCC_CONTROL_ACK            0x24c20
#define GMU_CX_BUSY_STATUS              0x24c30
#define GMU_AO_AHB_FENCE_CTRL           0x24c40
#define GMU_AHB_FENCE_RANGE_0           0x24c44
#define GMU_AHB_FENCE_STATUS            0x24c4c
#define GMU_AHB_FENCE_STATUS_CLR        0x24c50

/* ---- RSCC (0x03de0000) ---- */
#define RSCC_STATUS0_DRV0               0x0010
#define RSCC_PDC_SEQ_START_ADDR         0x0020
#define RSCC_PDC_MATCH_VALUE_LO         0x0024
#define RSCC_PDC_MATCH_VALUE_HI         0x0028
#define RSCC_PDC_SLAVE_ID_DRV0          0x002c
#define RSCC_HIDDEN_TCS_CMD_ADDR(n)     (0x0034 + 8 * (n))
#define RSCC_HIDDEN_TCS_CMD_DATA(n)     (0x0038 + 8 * (n))
#define RSCC_OVERRIDE_START_ADDR        0x0400
#define RSCC_SEQ_BUSY_DRV0              0x0404
#define RSCC_SEQ_MEM_0_DRV0             0x0600
static const uint32_t rscc_tcs_status[4] = { 0x0d18, 0x0fb8, 0x1258, 0x14f8 };

/* ---- PDC (0x0b290000) ---- */
#define PDC_GPU_ENABLE_PDC              0x4500
#define PDC_GPU_SEQ_START_ADDR          0x4520

/* ---- GMU virtual memory (A650, spec 2.2) ---- */
#define GMU_ICACHE_IOVA                 0x00004000u
#define GMU_ICACHE_SIZE                 0x00ffc000u     /* up to 16 MiB */
#define GMU_ICACHE_CHUNK                0x00100000u
#define GMU_ICACHE_CHUNKS               16
#define GMU_DUMMY_IOVA                  0x60000000u
#define GMU_LOG_IOVA                    0x60004000u
#define GMU_HFI_IOVA                    0x60008000u
#define GMU_BUF_SIZE                    0x4000u
#define GMU_ITCM_BASE                   0x00000000u
#define GMU_DTCM_BASE                   0x10004000u
#define GMU_TCM_SIZE                    0x4000u

/* ---- HFI ---- */
#define HFI_QUEUE_DWORDS                1024
#define HFI_H2F_PERF_TABLE              4
#define HFI_H2F_BW_TABLE                3
#define HFI_H2F_CORE_FW_START           14
#define HFI_H2F_START                   10
#define HFI_H2F_GX_BW_PERF_VOTE         30
#define HFI_H2F_PREPARE_SLUMBER         33
#define HFI_F2H_ERROR                   100
#define HFI_MAX_LEVELS_GX               16
#define HFI_MAX_LEVELS_CX               4

/* queue table layout inside the hfi buffer (dword indices) */
#define QT_VERSION      0
#define QT_SIZE         1
#define QT_QHDR0_OFF    2
#define QT_QHDR_SIZE    3
#define QT_NUM_QUEUES   4
#define QT_ACTIVE       5
#define QH(q)           (6 + 12 * (q))          /* queue header q */
#define QH_STATUS       0
#define QH_IOVA         1
#define QH_TYPE         2
#define QH_SIZE         3
#define QH_MSG_SIZE     4
#define QH_DROPPED      5
#define QH_RX_WM        6
#define QH_TX_WM        7
#define QH_RX_REQ       8
#define QH_TX_REQ       9
#define QH_READ         10
#define QH_WRITE        11
#define Q_CMD           0
#define Q_RSP           1

struct sgpu_gmu {
	struct sgpu_dma icache[GMU_ICACHE_CHUNKS];
	uint32_t        n_icache;
	struct sgpu_dma dummy, log, hfi;
	bool            mem_ready, rpmh_ready, running;
	uint32_t        seqnum;
	uint32_t        fw_version;

	uint32_t        nr_gpu_freqs, nr_gmu_freqs;
	uint32_t        gpu_freq_khz[HFI_MAX_LEVELS_GX];
	uint32_t        gx_votes[HFI_MAX_LEVELS_GX];
	uint32_t        gmu_freq_khz[HFI_MAX_LEVELS_CX];
	uint32_t        cx_votes[HFI_MAX_LEVELS_CX];
	uint32_t        perf_index;
	bool            dcvs_pinned;    /* sweetgpu_freq=N: no DCVS */
	uint64_t        dcvs_busy_us;   /* last time the GPU had work */
};

static struct sgpu_gmu gmu_storage;

static inline uint32_t gmu_rd(struct sgpu *g, uint32_t o) { return sgpu_rd(g->gmu, o); }
static inline void gmu_wr(struct sgpu *g, uint32_t o, uint32_t v) { sgpu_wr(g->gmu, o, v); }

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ------------------------------------------------------------------ */
/* memory                                                             */
/* ------------------------------------------------------------------ */

static int
gmu_buf(struct sgpu *g, struct sgpu_dma *d, uint32_t iova, uint32_t size, uint32_t prot,
    const char *name)
{
	int err = sgpu_dma_alloc(size, 0x4000, SGPU_DMA_WC, d);
	if (err) {
		return sgpu_fail(g, -12, "gmu: cannot allocate %s (%u bytes)", name, size);
	}
	err = sgpu_smmu_map(g, SGPU_DOM_GMU, iova, d->pa, size, prot);
	if (err) {
		return sgpu_fail(g, err, "gmu: cannot map %s at %#x", name, iova);
	}
	return 0;
}

static int
gmu_memory_init(struct sgpu *g, struct sgpu_gmu *m)
{
	const uint32_t rwx = SGPU_PROT_READ | SGPU_PROT_WRITE | SGPU_PROT_EXEC;
	int err;

	if (m->mem_ready) {
		return 0;
	}
	/*
	 * icache: 16 MiB - 16 KiB at GMU VA 0x4000, privileged (the M3
	 * fetches code from it). Linux backs it with scattered pages; we use
	 * 1 MiB contiguous chunks so no huge contiguous allocation is needed.
	 */
	for (uint32_t off = 0; off < GMU_ICACHE_SIZE; off += GMU_ICACHE_CHUNK) {
		uint32_t len = GMU_ICACHE_SIZE - off < GMU_ICACHE_CHUNK ?
		    GMU_ICACHE_SIZE - off : GMU_ICACHE_CHUNK;
		err = gmu_buf(g, &m->icache[m->n_icache], GMU_ICACHE_IOVA + off, len,
		    rwx | SGPU_PROT_PRIV, "icache");
		if (err) {
			return err;
		}
		m->n_icache++;
	}
	/* firmware "prealloc" block (type 7) at 0x60000000, 4 KiB; our granule is 16 KiB */
	if ((err = gmu_buf(g, &m->dummy, GMU_DUMMY_IOVA, GMU_BUF_SIZE, rwx | SGPU_PROT_PRIV, "dummy")) ||
	    (err = gmu_buf(g, &m->log, GMU_LOG_IOVA, GMU_BUF_SIZE, rwx, "log")) ||
	    (err = gmu_buf(g, &m->hfi, GMU_HFI_IOVA, GMU_BUF_SIZE, rwx, "hfi"))) {
		return err;
	}
	sgpu_smmu_tlb_sync(g, SGPU_DOM_GMU);
	m->mem_ready = true;
	sgpu_log("gmu: memory: icache %#x+%#x (%u chunks), dummy %#x, log %#x, hfi %#x",
	    GMU_ICACHE_IOVA, GMU_ICACHE_SIZE, m->n_icache, GMU_DUMMY_IOVA, GMU_LOG_IOVA,
	    GMU_HFI_IOVA);
	return 0;
}

/* CPU pointer for a GMU VA inside icache/dummy, with the bytes left in that buffer */
static volatile uint8_t *
gmu_va_to_cpu(struct sgpu_gmu *m, uint32_t addr, uint32_t *room)
{
	if (addr >= GMU_ICACHE_IOVA && addr < GMU_ICACHE_IOVA + GMU_ICACHE_SIZE) {
		uint32_t off = addr - GMU_ICACHE_IOVA;
		uint32_t i = off / GMU_ICACHE_CHUNK;
		if (i < m->n_icache) {
			*room = (uint32_t)m->icache[i].size - off % GMU_ICACHE_CHUNK;
			return (volatile uint8_t *)m->icache[i].va + off % GMU_ICACHE_CHUNK;
		}
	}
	if (addr >= GMU_DUMMY_IOVA && addr < GMU_DUMMY_IOVA + GMU_BUF_SIZE) {
		*room = GMU_DUMMY_IOVA + GMU_BUF_SIZE - addr;
		return (volatile uint8_t *)m->dummy.va + (addr - GMU_DUMMY_IOVA);
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* firmware                                                           */
/* ------------------------------------------------------------------ */

static void
tcm_write(struct sgpu *g, uint32_t win, uint32_t off, const uint8_t *src, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 4) {
		uint8_t b[4] = { 0, 0, 0, 0 };
		for (uint32_t k = 0; k < 4 && i + k < n; k++) {
			b[k] = src[i + k];
		}
		gmu_wr(g, win + off + i, le32(b));
	}
}

/*
 * a650_gmu.bin: { u32 addr, size(bytes), type, value; u8 data[size] }...
 * size 0 = descriptor (skipped; type 7 asks for the dummy prealloc).
 * Unlike Linux we bound-check every block against the file and the target.
 */
static int
gmu_fw_load(struct sgpu *g, struct sgpu_gmu *m)
{
	const uint8_t *fw = g->gmu_fw;
	uint32_t len = g->gmu_fw_len, off = 0, blocks = 0, bytes = 0;

	while (off + 16 <= len) {
		uint32_t addr = le32(fw + off), size = le32(fw + off + 4);
		const uint8_t *data = fw + off + 16;
		if (size > len - off - 16) {
			return sgpu_fail(g, -22, "gmu fw: block at %#x (addr %#x size %#x) "
			           "runs past the file", off, addr, size);
		}
		off += 16 + size;
		if (size == 0) {
			continue;
		}
		if (addr < GMU_ITCM_BASE + GMU_TCM_SIZE) {
			if (addr + size > GMU_TCM_SIZE) {
				return sgpu_fail(g, -22, "gmu fw: ITCM block %#x+%#x too big", addr, size);
			}
			tcm_write(g, GMU_CM3_ITCM_START, addr - GMU_ITCM_BASE, data, size);
		} else if (addr >= GMU_DTCM_BASE && addr < GMU_DTCM_BASE + GMU_TCM_SIZE) {
			if (addr - GMU_DTCM_BASE + size > GMU_TCM_SIZE) {
				return sgpu_fail(g, -22, "gmu fw: DTCM block %#x+%#x too big", addr, size);
			}
			tcm_write(g, GMU_CM3_DTCM_START, addr - GMU_DTCM_BASE, data, size);
		} else {
			uint32_t done = 0;
			while (done < size) {
				uint32_t room;
				volatile uint8_t *dst = gmu_va_to_cpu(m, addr + done, &room);
				if (dst == NULL) {
					sgpu_log("gmu fw: block %#x+%#x outside every GMU buffer (skipped)",
					    addr, size);
					break;
				}
				uint32_t n = size - done < room ? size - done : room;
				for (uint32_t i = 0; i < n; i++) {
					dst[i] = data[done + i];
				}
				done += n;
			}
		}
		blocks++;
		bytes += size;
	}
	sgpu_wmb();
	m->fw_version = gmu_rd(g, GMU_CORE_FW_VERSION);
	sgpu_log("gmu fw: %u blocks, %u bytes, version %u.%u.%u (%#x)", blocks, bytes,
	    m->fw_version >> 28, (m->fw_version >> 16) & 0xfff, m->fw_version & 0xffff,
	    m->fw_version);
	return 0;
}

/* ------------------------------------------------------------------ */
/* RSC / PDC (spec 1.2), once per boot, with CX + GMU clocks on       */
/* ------------------------------------------------------------------ */

static void
rpmh_stop(struct sgpu *g)
{
	gmu_wr(g, GMU_RSCC_CONTROL_REQ, 1);
	if (sgpu_poll(g->rscc, RSCC_STATUS0_DRV0, 1u << 16, 1u << 16, 10000)) {
		sgpu_log("gmu: unable to power off the GPU RSC (STATUS0 %#x)",
		    sgpu_rd(g->rscc, RSCC_STATUS0_DRV0));
	}
	gmu_wr(g, GMU_RSCC_CONTROL_REQ, 0);
}

static void
rpmh_init(struct sgpu *g, struct sgpu_gmu *m)
{
	static const uint32_t seq[5] = {
		0xeaaae5a0, 0xe1a1ebab, 0xa2e0a581, 0xecac82e2, 0x0020edad,     /* A650 family */
	};

	if (m->rpmh_ready) {
		return;
	}
	sgpu_wr(g->rscc, RSCC_STATUS0_DRV0, 1u << 24);  /* disable SDE clock gating */
	sgpu_wr(g->rscc, RSCC_PDC_SLAVE_ID_DRV0, 1);
	sgpu_wr(g->rscc, RSCC_HIDDEN_TCS_CMD_DATA(0), 0);
	sgpu_wr(g->rscc, RSCC_HIDDEN_TCS_CMD_ADDR(0), 0);
	sgpu_wr(g->rscc, RSCC_HIDDEN_TCS_CMD_DATA(1), 0);
	sgpu_wr(g->rscc, RSCC_HIDDEN_TCS_CMD_ADDR(1), 0);
	sgpu_wr(g->rscc, RSCC_HIDDEN_TCS_CMD_DATA(2), 0x80000000);
	sgpu_wr(g->rscc, RSCC_HIDDEN_TCS_CMD_ADDR(2), 0);
	sgpu_wr(g->rscc, RSCC_OVERRIDE_START_ADDR, 0);
	sgpu_wr(g->rscc, RSCC_PDC_SEQ_START_ADDR, PDC_GPU_SEQ_START_ADDR);
	sgpu_wr(g->rscc, RSCC_PDC_MATCH_VALUE_LO, 0x4510);
	sgpu_wr(g->rscc, RSCC_PDC_MATCH_VALUE_HI, 0x4514);
	for (int i = 0; i < 5; i++) {
		sgpu_wr(g->rscc, RSCC_SEQ_MEM_0_DRV0 + 4 * i, seq[i]);
	}
	/* PDC sequencer + TCS live in AOP on A650: only point and enable it */
	sgpu_wr(g->pdc, PDC_GPU_SEQ_START_ADDR, 0);
	sgpu_wr(g->pdc, PDC_GPU_ENABLE_PDC, 0x80000001);
	sgpu_wmb();
	rpmh_stop(g);
	m->rpmh_ready = true;
	sgpu_log("gmu: RSC/PDC programmed");
}

static int
rpmh_start(struct sgpu *g)
{
	gmu_wr(g, GMU_RSCC_CONTROL_REQ, 1u << 1);
	if (sgpu_poll(g->gmu, GMU_RSCC_CONTROL_ACK, 1u << 1, 1u << 1, 10000)) {
		return sgpu_fail(g, -60, "gmu: unable to power on the GPU RSC (ACK %#x)",
		           gmu_rd(g, GMU_RSCC_CONTROL_ACK));
	}
	if (sgpu_poll(g->rscc, RSCC_SEQ_BUSY_DRV0, 0xffffffffu, 0, 10000)) {
		return sgpu_fail(g, -60, "gmu: GPU RSC sequence stuck (%#x)",
		           sgpu_rd(g->rscc, RSCC_SEQ_BUSY_DRV0));
	}
	gmu_wr(g, GMU_RSCC_CONTROL_REQ, 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* frequency tables and ARC votes (spec 3.5)                          */
/* ------------------------------------------------------------------ */

struct opp { uint32_t khz; uint16_t level; uint8_t bins; };
/* bins: bit n = enabled for speed-bin fuse value n (sm8250 opp-supported-hw
 * after the fuse->index map {0:0, 1:1, 2:3, 3:2}) */
static const struct opp a650_opps[] = {
	{ 305000,  64, 0xf },
	{ 400000, 128, 0xf },
	{ 441600, 144, 0xf },
	{ 490000, 192, 0xf },
	{ 525000, 224, 0xf },
	{ 587000, 256, 0x7 },
	{ 670000, 320, 0x6 },
};

static uint32_t
read_speed_bin(void)
{
	const void *p;
	uint32_t len;

	if (sgpu_dt_prop("speed-bin", &p, &len) && len == 4) {
		return *(const uint32_t *)p;
	}
	/* qfprom 0x784000 + 0x19b, bits [7:5] */
	volatile uint8_t *q = sgpu_ioremap(0x00784000, 0x1000);
	if (q == NULL) {
		return 0;
	}
	uint32_t v = (sgpu_rd(q, 0x198) >> 24) & 0xff;
	return (v >> 5) & 7;
}

static int
arc_vote(const struct sgpu_arc *pri, const struct sgpu_arc *sec, uint32_t level,
    uint32_t *vote)
{
	uint32_t p, s;

	for (p = 0; p < pri->count && pri->level[p] < level; p++) {
	}
	if (p == pri->count) {
		return -2;
	}
	for (s = 0; s < sec->count && sec->level[s] < level; s++) {
	}
	if (s == sec->count) {
		for (s = sec->count; s > 0 && sec->level[s - 1] == 0; s--) {
		}
		s = s ? s - 1 : 0;
	}
	*vote = ((uint32_t)pri->level[p] << 16) | ((s & 0xff) << 8) | (p & 0xff);
	return 0;
}

static int
gmu_pwrlevels(struct sgpu *g, struct sgpu_gmu *m)
{
	struct sgpu_arc gfx, cx, mx;
	uint32_t bin = read_speed_bin();

	g->speed_bin = bin;
	if (sgpu_cmddb_arc("gfx.lvl", &gfx) || sgpu_cmddb_arc("cx.lvl", &cx) ||
	    (sgpu_cmddb_arc("gmxc.lvl", &mx) && sgpu_cmddb_arc("mx.lvl", &mx))) {
		/* Linux ignores this; all-zero votes would vote the rails off */
		return sgpu_fail(g, -2, "gmu: cmd-db has no gfx/cx/mx ARC levels");
	}

	m->nr_gpu_freqs = 1;
	m->gpu_freq_khz[0] = 0;
	if (arc_vote(&gfx, &mx, 0, &m->gx_votes[0])) {
		return sgpu_fail(g, -2, "gmu: gfx.lvl has no off level");
	}
	for (uint32_t i = 0; i < sizeof(a650_opps) / sizeof(a650_opps[0]); i++) {
		const struct opp *o = &a650_opps[i];
		if (!(o->bins & (1u << (bin < 4 ? bin : 0)))) {
			continue;
		}
		uint32_t n = m->nr_gpu_freqs;
		if (arc_vote(&gfx, &mx, o->level, &m->gx_votes[n])) {
			return sgpu_fail(g, -2, "gmu: RPMh level %u not in gfx.lvl", o->level);
		}
		m->gpu_freq_khz[n] = o->khz;
		m->nr_gpu_freqs++;
	}
	m->nr_gmu_freqs = 2;
	m->gmu_freq_khz[0] = 0;
	m->gmu_freq_khz[1] = 200000;
	if (arc_vote(&cx, &mx, 0, &m->cx_votes[0]) || arc_vote(&cx, &mx, 48, &m->cx_votes[1])) {
		return sgpu_fail(g, -2, "gmu: RPMh level 48 not in cx.lvl");
	}
	sgpu_log("gmu: speed bin %u, %u GPU levels up to %u MHz", bin, m->nr_gpu_freqs - 1,
	    m->gpu_freq_khz[m->nr_gpu_freqs - 1] / 1000);
	for (uint32_t i = 0; i < m->nr_gpu_freqs; i++) {
		sgpu_log("gmu:   level %u: %u kHz vote %#010x", i, m->gpu_freq_khz[i], m->gx_votes[i]);
	}
	sgpu_log("gmu:   GMU 200 MHz vote %#010x", m->cx_votes[1]);
	return 0;
}

/* ------------------------------------------------------------------ */
/* HFI                                                                */
/* ------------------------------------------------------------------ */

static volatile uint32_t *
hfi_tbl(struct sgpu_gmu *m)
{
	return (volatile uint32_t *)m->hfi.va;
}

static volatile uint32_t *
hfi_ring(struct sgpu_gmu *m, int q)
{
	return (volatile uint32_t *)(void *)((uint8_t *)m->hfi.va + 0x1000 * (q + 1));
}

static void
hfi_init(struct sgpu_gmu *m)
{
	volatile uint32_t *t = hfi_tbl(m);

	sgpu_mem_wr32(&t[QT_VERSION], 0);
	sgpu_mem_wr32(&t[QT_SIZE], 24 + 2 * 48);       /* bytes, as Linux does */
	sgpu_mem_wr32(&t[QT_QHDR0_OFF], 6);
	sgpu_mem_wr32(&t[QT_QHDR_SIZE], 12);
	sgpu_mem_wr32(&t[QT_NUM_QUEUES], 2);
	sgpu_mem_wr32(&t[QT_ACTIVE], 2);
	for (int q = 0; q < 2; q++) {
		volatile uint32_t *h = &t[QH(q)];
		sgpu_mem_wr32(&h[QH_STATUS], 1);
		sgpu_mem_wr32(&h[QH_IOVA], GMU_HFI_IOVA + 0x1000 * (uint32_t)(q + 1));
		sgpu_mem_wr32(&h[QH_TYPE], (10u << 8) | (uint32_t)q);
		sgpu_mem_wr32(&h[QH_SIZE], HFI_QUEUE_DWORDS);
		sgpu_mem_wr32(&h[QH_MSG_SIZE], 0);
		sgpu_mem_wr32(&h[QH_DROPPED], 0);
		sgpu_mem_wr32(&h[QH_RX_WM], 1);
		sgpu_mem_wr32(&h[QH_TX_WM], 1);
		sgpu_mem_wr32(&h[QH_RX_REQ], 1);
		sgpu_mem_wr32(&h[QH_TX_REQ], 0);
		sgpu_mem_wr32(&h[QH_READ], 0);
		sgpu_mem_wr32(&h[QH_WRITE], 0);
	}
	sgpu_wmb();
}

static void
hfi_reset_queues(struct sgpu_gmu *m)
{
	volatile uint32_t *t = hfi_tbl(m);
	for (int q = 0; q < 2; q++) {
		sgpu_mem_wr32(&t[QH(q) + QH_READ], 0);
		sgpu_mem_wr32(&t[QH(q) + QH_WRITE], 0);
	}
	sgpu_wmb();
}

static int
hfi_queue_write(struct sgpu *g, struct sgpu_gmu *m, const uint32_t *msg, uint32_t n)
{
	volatile uint32_t *h = &hfi_tbl(m)[QH(Q_CMD)];
	volatile uint32_t *ring = hfi_ring(m, Q_CMD);
	uint32_t rd = sgpu_mem_rd32(&h[QH_READ]) % HFI_QUEUE_DWORDS;
	uint32_t wr = sgpu_mem_rd32(&h[QH_WRITE]) % HFI_QUEUE_DWORDS;
	uint32_t space = (rd - wr - 1) & (HFI_QUEUE_DWORDS - 1);

	if (space < ((n + 3) & ~3u)) {
		sgpu_mem_wr32(&h[QH_DROPPED], sgpu_mem_rd32(&h[QH_DROPPED]) + 1);
		return sgpu_fail(g, -28, "hfi: command queue full (rd %u wr %u)", rd, wr);
	}
	for (uint32_t i = 0; i < n; i++) {
		sgpu_mem_wr32(&ring[wr], msg[i]);
		wr = (wr + 1) % HFI_QUEUE_DWORDS;
	}
	while (wr & 3) {                        /* HFI v2: pad to 4 dwords */
		sgpu_mem_wr32(&ring[wr], 0xfafafafa);
		wr = (wr + 1) % HFI_QUEUE_DWORDS;
	}
	sgpu_wmb();
	sgpu_mem_wr32(&h[QH_WRITE], wr);
	sgpu_wmb();
	gmu_wr(g, GMU_HOST2GMU_INTR_SET, 1);
	return 0;
}

/* one message from the response queue; 0 = got one, 1 = empty */
static int
hfi_queue_read(struct sgpu *g, struct sgpu_gmu *m, uint32_t *msg, uint32_t max)
{
	volatile uint32_t *h = &hfi_tbl(m)[QH(Q_RSP)];
	volatile uint32_t *ring = hfi_ring(m, Q_RSP);
	uint32_t rd = sgpu_mem_rd32(&h[QH_READ]) % HFI_QUEUE_DWORDS;
	uint32_t wr = sgpu_mem_rd32(&h[QH_WRITE]) % HFI_QUEUE_DWORDS;

	if (rd == wr) {
		sgpu_mem_wr32(&h[QH_RX_REQ], 1);
		return 1;
	}
	uint32_t hdr = sgpu_mem_rd32(&ring[rd]);
	uint32_t n = (hdr >> 8) & 0xff;
	if (n == 0 || n > max) {
		sgpu_log("hfi: bad response header %#x at %u, flushing the queue", hdr, rd);
		sgpu_mem_wr32(&h[QH_READ], wr);
		(void)g;
		return 1;
	}
	for (uint32_t i = 0; i < n; i++) {
		msg[i] = sgpu_mem_rd32(&ring[rd]);
		rd = (rd + 1) % HFI_QUEUE_DWORDS;
	}
	rd = ((rd + 3) & ~3u) % HFI_QUEUE_DWORDS;
	sgpu_mem_wr32(&h[QH_READ], rd);
	sgpu_wmb();
	return 0;
}

static int
hfi_wait_ack(struct sgpu *g, struct sgpu_gmu *m, uint32_t id, uint32_t seq,
    uint32_t *payload, uint32_t npayload)
{
	uint32_t rsp[19];
	int empty_retries = 0;

	for (;;) {
		if (sgpu_poll(g->gmu, GMU_GMU2HOST_INTR_INFO, GMU2HOST_MSGQ, GMU2HOST_MSGQ, 1000000)) {
			return sgpu_fail(g, -60, "hfi: timeout waiting for ack of msg %u seq %u "
			           "(INFO %#x, fw init %#x)", id, seq,
			           gmu_rd(g, GMU_GMU2HOST_INTR_INFO), gmu_rd(g, GMU_CM3_FW_INIT_RESULT));
		}
		gmu_wr(g, GMU_GMU2HOST_INTR_CLR, GMU2HOST_MSGQ);
		for (;;) {
			int r = hfi_queue_read(g, m, rsp, 19);
			if (r == 1) {
				if (++empty_retries > 1) {
					return sgpu_fail(g, -5, "hfi: response queue empty for msg %u", id);
				}
				break;          /* wait for the next doorbell */
			}
			if ((rsp[0] & 0xff) == HFI_F2H_ERROR) {
				sgpu_log("hfi: firmware error %u (%#x %#x)", rsp[1], rsp[2], rsp[3]);
				continue;
			}
			if (((rsp[1] >> 20) & 0xfff) != seq) {
				sgpu_log("hfi: unexpected ack seq %u (want %u) for msg %u",
				    (rsp[1] >> 20) & 0xfff, seq, rsp[1] & 0xff);
				continue;
			}
			if (rsp[2] != 0) {
				return sgpu_fail(g, -22, "hfi: msg %u rejected, error %u", id, rsp[2]);
			}
			for (uint32_t i = 0; i < npayload && i < 16; i++) {
				payload[i] = rsp[3 + i];
			}
			return 0;
		}
	}
}

static int
hfi_send(struct sgpu *g, struct sgpu_gmu *m, uint32_t id, uint32_t *msg, uint32_t dwords)
{
	uint32_t seq = (++m->seqnum) % 0xfff;
	int err;

	msg[0] = (seq << 20) | (0u << 16) | (dwords << 8) | id;
	if ((err = hfi_queue_write(g, m, msg, dwords)) != 0) {
		return err;
	}
	return hfi_wait_ack(g, m, id, seq, NULL, 0);
}

static int
hfi_send_perf_table(struct sgpu *g, struct sgpu_gmu *m)
{
	uint32_t msg[59] = { 0 };

	msg[1] = m->nr_gpu_freqs;
	msg[2] = m->nr_gmu_freqs;
	for (uint32_t i = 0; i < m->nr_gpu_freqs; i++) {
		msg[3 + 3 * i] = m->gx_votes[i];
		msg[4 + 3 * i] = 0xffffffff;    /* no ACD */
		msg[5 + 3 * i] = m->gpu_freq_khz[i];
	}
	for (uint32_t j = 0; j < m->nr_gmu_freqs; j++) {
		msg[51 + 2 * j] = m->cx_votes[j];
		msg[52 + 2 * j] = m->gmu_freq_khz[j];
	}
	return hfi_send(g, m, HFI_H2F_PERF_TABLE, msg, 59);
}

static int
hfi_send_bw_table(struct sgpu *g, struct sgpu_gmu *m)
{
	uint32_t msg[160] = { 0 };

	/* Linux a650_build_bw_table: one DDR level ("off" BCM votes), CNOC on/off */
	msg[1] = 1;                     /* bw_level_num */
	msg[2] = 1;                     /* cnoc_cmds_num */
	msg[3] = 3;                     /* ddr_cmds_num */
	msg[4] = 0x01;                  /* cnoc_wait_bitmask */
	msg[5] = 0x01;                  /* ddr_wait_bitmask */
	msg[6] = 0x500a4;               /* cnoc_cmds_addrs[0] */
	msg[12 + 0 * 6 + 0] = 0x40000000;       /* cnoc_cmds_data[0][0] */
	msg[12 + 1 * 6 + 0] = 0x60000001;       /* cnoc_cmds_data[1][0] */
	msg[24] = 0x50000;              /* ddr_cmds_addrs */
	msg[25] = 0x50004;
	msg[26] = 0x5007c;
	msg[32 + 0] = 0x40000000;       /* ddr_cmds_data[0][0..2] */
	msg[32 + 1] = 0x40000000;
	msg[32 + 2] = 0x40000000;
	return hfi_send(g, m, HFI_H2F_BW_TABLE, msg, 160);
}

static int
hfi_perf_vote(struct sgpu *g, struct sgpu_gmu *m, uint32_t index)
{
	uint32_t msg[4] = { 0, 1 /* blocking ack */, index, 0 /* bw index */ };
	return hfi_send(g, m, HFI_H2F_GX_BW_PERF_VOTE, msg, 4);
}

static int
hfi_start(struct sgpu *g, struct sgpu_gmu *m)
{
	uint32_t msg[2] = { 0, 0 };
	int err;

	if ((err = hfi_send_perf_table(g, m)) != 0 ||
	    (err = hfi_send_bw_table(g, m)) != 0) {
		return err;
	}
	if ((err = hfi_send(g, m, HFI_H2F_CORE_FW_START, msg, 2)) != 0) {
		return err;
	}
	return hfi_send(g, m, HFI_H2F_START, msg, 1);
}

/* ------------------------------------------------------------------ */
/* firmware start (spec 1.4)                                          */
/* ------------------------------------------------------------------ */

static uint32_t
gmu_chipid(uint32_t chip)
{
	return (chip & 0xffff0000u) | ((chip << 4) & 0xf000u) | ((chip << 8) & 0x0f00u);
}

/* What the host can see of a GMU that does not answer. */
static void
gmu_dump(struct sgpu *g)
{
	sgpu_log("gmu: SYSRESET %#x BOOT_CONFIG %#x CFG %#x GENERAL_7 %#x FW_BUSY %#x",
	    gmu_rd(g, GMU_CM3_SYSRESET), gmu_rd(g, GMU_CM3_BOOT_CONFIG), gmu_rd(g, GMU_CM3_CFG),
	    gmu_rd(g, GMU_GENERAL_7), gmu_rd(g, GMU_CM3_FW_BUSY));
	sgpu_log("gmu: ITCM %#x %#x %#x %#x, DTCM %#x %#x, HFI_QTBL %#x, CP_MSG %#x",
	    gmu_rd(g, GMU_CM3_ITCM_START), gmu_rd(g, GMU_CM3_ITCM_START + 4),
	    gmu_rd(g, GMU_CM3_ITCM_START + 8), gmu_rd(g, GMU_CM3_ITCM_START + 12),
	    gmu_rd(g, GMU_CM3_DTCM_START), gmu_rd(g, GMU_CM3_DTCM_START + 4),
	    gmu_rd(g, GMU_HFI_QTBL_ADDR), gmu_rd(g, GMU_PWR_COL_CP_MSG));
	sgpu_log("gmu: AO int status %#x, RSCC ack %#x, cx-gmu cbcr %#x, gmu rcg %#x/%#x",
	    gmu_rd(g, GMU_AO_HOST_INTERRUPT_STATUS), gmu_rd(g, GMU_RSCC_CONTROL_ACK),
	    sgpu_rd(g->gpucc, GPUCC_CX_GMU_CBCR_DUMP), sgpu_rd(g->gpucc, GPUCC_GMU_CMD_RCGR_DUMP),
	    sgpu_rd(g->gpucc, GPUCC_GMU_CMD_RCGR_DUMP + 4));
	if (!sgpu_smmu_check_faults(g)) {
		sgpu_log("gmu: no SMMU faults recorded");
	}
	sgpu_smmu_debug(g, SGPU_DOM_GMU, GMU_LOG_IOVA);
	sgpu_smmu_debug(g, SGPU_DOM_GMU, GMU_ICACHE_IOVA);
}

static int
gmu_fw_start(struct sgpu *g, struct sgpu_gmu *m)
{
	int err;

	gmu_wr(g, GMU_CX_FALNEXT_INTF, 1);      /* vote veto for FAL10 */
	gmu_wr(g, GMU_CX_FAL_INTF, 1);
	gmu_wr(g, GMU_GENERAL_7, 1);            /* TCM retention */
	if ((err = rpmh_start(g)) != 0) {
		return err;
	}
	/* hold the M3 in reset while its TCMs are rewritten (CX may not have
	 * collapsed since a previous attempt) */
	gmu_wr(g, GMU_CM3_SYSRESET, 1);
	if ((err = gmu_fw_load(g, m)) != 0) {
		return err;
	}
	gmu_wr(g, GMU_CM3_FW_INIT_RESULT, 0);
	gmu_wr(g, GMU_CM3_BOOT_CONFIG, 2);
	gmu_wr(g, GMU_HFI_QTBL_ADDR, GMU_HFI_IOVA);
	gmu_wr(g, GMU_HFI_QTBL_INFO, 1);
	gmu_wr(g, GMU_AHB_FENCE_RANGE_0, (1u << 31) | (0xau << 18) | 0xa0);     /* 0x802800a0 */
	gmu_wr(g, GMU_CM3_CFG, 0x4052);
	gmu_wr(g, GMU_HFI_SFR_ADDR, gmu_chipid(g->chip_id));
	gmu_wr(g, GMU_PWR_COL_CP_MSG, GMU_LOG_IOVA | (GMU_BUF_SIZE / 0x1000 - 1));

	/* power config */
	gmu_wr(g, GMU_SYS_BUS_CONFIG, 1);
	gmu_wr(g, GMU_ICACHE_CONFIG, 1);
	gmu_wr(g, GMU_DCACHE_CONFIG, 1);
	gmu_wr(g, GMU_PWR_COL_INTER_FRAME_CTRL, 0x09c40400);
	/* RPMh interface + LLC/DDR/MX/CX/GFX vote enables */
	sgpu_rmw(g->gmu, GMU_RPMH_CTRL, 0, 0x00000f11);

	/* start the M3 */
	uint32_t mask, want;
	if (m->fw_version <= 0x20010004) {
		mask = 0xffffffffu;
		want = 0xbabeface;
	} else {
		mask = 0x1ff;
		want = 0x100;
	}
	gmu_wr(g, GMU_CM3_SYSRESET, 1);
	gmu_wr(g, GMU_PWR_COL_CP_RESP, 0);      /* log write index */
	sgpu_wmb();
	gmu_wr(g, GMU_CM3_SYSRESET, 0);
	if (sgpu_poll(g->gmu, GMU_CM3_FW_INIT_RESULT, mask, want, 10000)) {
		gmu_dump(g);
		return sgpu_fail(g, -60, "gmu: firmware init timed out (INIT_RESULT %#x, "
		           "FW_BUSY %#x, AO status %#x)", gmu_rd(g, GMU_CM3_FW_INIT_RESULT),
		           gmu_rd(g, GMU_CM3_FW_BUSY), gmu_rd(g, GMU_AO_HOST_INTERRUPT_STATUS));
	}
	/* HFI queues */
	gmu_wr(g, GMU_HFI_CTRL_INIT, 1);
	if (sgpu_poll(g->gmu, GMU_HFI_CTRL_STATUS, 1, 1, 10000)) {
		return sgpu_fail(g, -60, "gmu: unable to start the HFI queues (%#x)",
		           gmu_rd(g, GMU_HFI_CTRL_STATUS));
	}
	sgpu_wmb();
	sgpu_log("gmu: M3 running, INIT_RESULT %#x, HFI version %#x",
	    gmu_rd(g, GMU_CM3_FW_INIT_RESULT), gmu_rd(g, GMU_HFI_VERSION_INFO));
	return 0;
}

/* ------------------------------------------------------------------ */
/* public                                                             */
/* ------------------------------------------------------------------ */

int
sgpu_gmu_init(struct sgpu *g)
{
	struct sgpu_gmu *m = &gmu_storage;
	uint32_t want;
	int err;

	if (g->gmu_state == NULL) {         /* first init of this device */
		for (size_t i = 0; i < sizeof(*m); i++) {
			((uint8_t *)m)[i] = 0;
		}
		g->gmu_state = m;
	}
	if ((err = gmu_memory_init(g, m)) != 0) {
		return err;
	}
	if (m->nr_gpu_freqs == 0 && (err = gmu_pwrlevels(g, m)) != 0) {
		return err;
	}
	hfi_init(m);
	rpmh_init(g, m);

	/* AO interrupts: unmask WDOG_BITE, FENCE_ERR, HOST_AHB_BUS_ERROR */
	gmu_wr(g, GMU_AO_HOST_INTERRUPT_CLR, 0xffffffffu);
	gmu_wr(g, GMU_AO_HOST_INTERRUPT_MASK, ~((1u << 0) | (1u << 3) | (1u << 5)));

	if ((err = gmu_fw_start(g, m)) != 0 || (err = hfi_start(g, m)) != 0) {
		sgpu_gmu_irq(g);
		rpmh_stop(g);
		return err;
	}
	gmu_wr(g, GMU_GMU2HOST_INTR_CLR, 0xffffffffu);
	gmu_wr(g, GMU_GMU2HOST_INTR_MASK, ~GMU2HOST_CM3_FAULT);

	/*
	 * Initial GX level: the lowest (305 MHz, RPMh LOW_SVS), and DCVS
	 * (sgpu_dcvs_busy) raises it with the first submission.
	 * sweetgpu_freq=N pins level N instead (1 = lowest, 0 = highest).
	 */
	want = 1;
	m->dcvs_pinned = sgpu_bootarg_u32("sweetgpu_freq", &want);
	if (want == 0 || want >= m->nr_gpu_freqs) {
		want = m->nr_gpu_freqs - 1;
	}
	m->perf_index = want;
	m->dcvs_busy_us = sgpu_now_us();
	if ((err = hfi_perf_vote(g, m, m->perf_index)) != 0) {
		return err;
	}
	m->running = true;
	sgpu_log("gmu: HFI started, GX level %u (%u MHz, %s), RPMh power state %#x",
	    m->perf_index, m->gpu_freq_khz[m->perf_index] / 1000,
	    m->dcvs_pinned ? "pinned by sweetgpu_freq" : "DCVS", gmu_rd(g, GMU_RPMH_POWER_STATE));
	return 0;
}

static void
oob_bits(enum sgpu_oob oob, uint32_t *req, uint32_t *ack, uint32_t *clr)
{
	if (oob == SGPU_OOB_GPU_SET) {
		*req = 30; *ack = 31; *clr = 31;
	} else {
		*req = 28; *ack = 30; *clr = 29;
	}
}

int
sgpu_gmu_oob_set(struct sgpu *g, enum sgpu_oob oob)
{
	uint32_t req, ack, clr;
	int err;

	oob_bits(oob, &req, &ack, &clr);
	gmu_wr(g, GMU_HOST2GMU_INTR_SET, 1u << req);
	err = sgpu_poll(g->gmu, GMU_GMU2HOST_INTR_INFO, 1u << ack, 1u << ack, 10000);
	gmu_wr(g, GMU_GMU2HOST_INTR_CLR, 1u << ack);
	if (err) {
		sgpu_log("gmu: OOB %s timed out (INFO %#x)",
		    oob == SGPU_OOB_GPU_SET ? "GPU_SET" : "PERFCOUNTER",
		    gmu_rd(g, GMU_GMU2HOST_INTR_INFO));
	}
	return err;
}

void
sgpu_gmu_oob_clear(struct sgpu *g, enum sgpu_oob oob)
{
	uint32_t req, ack, clr;

	oob_bits(oob, &req, &ack, &clr);
	gmu_wr(g, GMU_HOST2GMU_INTR_SET, 1u << clr);
}

int
sgpu_gmu_gx_on(struct sgpu *g)
{
	/*
	 * The OOB GPU_SET request is held from here until the end of CP
	 * hw_init (sgpu_cp_init clears it), mirroring Linux, where hw_init
	 * starts and ends with this handshake. With IFPC disabled the GMU
	 * keeps GX up afterwards.
	 */
	if (sgpu_gmu_oob_set(g, SGPU_OOB_GPU_SET)) {
		return sgpu_fail(g, -60, "gmu: GX power (OOB GPU_SET) not acknowledged");
	}
	uint32_t st = gmu_rd(g, GMU_SPTPRAC_PWR_CLK_STATUS);
	if (st & (GX_HM_GDSC_POWER_OFF | GX_HM_CLK_OFF)) {
		return sgpu_fail(g, -5, "gmu: GX still off after OOB ack (SPTPRAC status %#x, "
		           "GX GDSCR %#x)", st, sgpu_rd(g->gpucc, 0x100c));
	}
	/* first GX register reads: RBBM_STATUS and the chip's version */
	sgpu_log("gmu: GX on (SPTPRAC %#x, GX GDSCR %#x), RBBM_STATUS %#x",
	    st, sgpu_rd(g->gpucc, 0x100c), sgpu_rd(g->gpu, 0x210 * 4));
	return 0;
}

void
sgpu_gmu_gx_off(struct sgpu *g)
{
	sgpu_gmu_oob_clear(g, SGPU_OOB_GPU_SET);
}

int
sgpu_gmu_set_freq(struct sgpu *g, uint32_t index)
{
	struct sgpu_gmu *m = g->gmu_state;

	if (m == NULL || !m->running || index == 0 || index >= m->nr_gpu_freqs) {
		return -22;
	}
	if (index == m->perf_index) {
		return 0;
	}
	int err = hfi_perf_vote(g, m, index);
	if (!err) {
		m->perf_index = index;
	}
	return err;
}

uint32_t
sgpu_gmu_max_freq_hz(struct sgpu *g)
{
	struct sgpu_gmu *m = g->gmu_state;
	if (m == NULL || m->nr_gpu_freqs == 0) {
		return 0;
	}
	return m->gpu_freq_khz[m->dcvs_pinned ? m->perf_index : m->nr_gpu_freqs - 1] * 1000u;
}

/*
 * DCVS, race to idle: the highest level as soon as work is submitted, the
 * lowest once the ring has been idle for SGPU_DCVS_IDLE_US. Called with
 * the driver lock held, from the submit path and from a periodic tick.
 */
#define SGPU_DCVS_IDLE_US       100000

void
sgpu_dcvs_busy(struct sgpu *g)
{
	struct sgpu_gmu *m = g->gmu_state;

	if (m == NULL || !m->running || m->dcvs_pinned) {
		return;
	}
	m->dcvs_busy_us = sgpu_now_us();
	if (m->perf_index != m->nr_gpu_freqs - 1) {
		(void)sgpu_gmu_set_freq(g, m->nr_gpu_freqs - 1);
	}
}

void
sgpu_dcvs_tick(struct sgpu *g)
{
	struct sgpu_gmu *m = g->gmu_state;

	if (m == NULL || !m->running || m->dcvs_pinned || g->stage_reached < SGPU_STAGE_CP) {
		return;
	}
	uint64_t now = sgpu_now_us();
	if (sgpu_cp_ring_fence_retired(g) != g->fence_seqno) {
		m->dcvs_busy_us = now;
		return;
	}
	if (m->perf_index != 1 && now - m->dcvs_busy_us >= SGPU_DCVS_IDLE_US) {
		(void)sgpu_gmu_set_freq(g, 1);
	}
}

uint32_t
sgpu_gmu_state(struct sgpu *g)
{
	struct sgpu_gmu *m = g->gmu_state;
	if (m == NULL || !m->running) {
		return 0xffffffffu;
	}
	return gmu_rd(g, GMU_RPMH_POWER_STATE) | (m->perf_index << 16);
}

uint32_t
sgpu_gmu_irq(struct sgpu *g)
{
	uint32_t ao, info;

	if (g->gmu == NULL || g->stage_reached < SGPU_STAGE_CLOCKS) {
		return 0;
	}
	ao = gmu_rd(g, GMU_AO_HOST_INTERRUPT_STATUS);
	if (ao) {
		gmu_wr(g, GMU_AO_HOST_INTERRUPT_CLR, ao);
		if (ao & 1) {
			sgpu_log("gmu: watchdog bite -- GMU hung");
		}
		if (ao & (1u << 3)) {
			sgpu_log("gmu: AHB fence error, status %#x", gmu_rd(g, GMU_AHB_FENCE_STATUS));
		}
		if (ao & (1u << 5)) {
			sgpu_log("gmu: host AHB bus error");
		}
	}
	info = gmu_rd(g, GMU_GMU2HOST_INTR_INFO);
	if (info & GMU2HOST_CM3_FAULT) {
		gmu_wr(g, GMU_GMU2HOST_INTR_CLR, GMU2HOST_CM3_FAULT);
		sgpu_log("gmu: CM3 fault");
		ao |= 1u << 31;
	}
	return ao;
}

void
sgpu_gmu_shutdown(struct sgpu *g)
{
	struct sgpu_gmu *m = g->gmu_state;

	if (m == NULL || !m->running) {
		return;
	}
	/* graceful: PREPARE_SLUMBER, else the force-off path (spec 6) */
	uint32_t msg[3] = { 0, 0, 0 };
	gmu_wr(g, GMU_POWER_COUNTER_ENABLE, 0);
	if (hfi_send(g, m, HFI_H2F_PREPARE_SLUMBER, msg, 3) == 0) {
		gmu_wr(g, GMU_AO_AHB_FENCE_CTRL, 0);
		(void)sgpu_poll(g->gmu, GMU_CX_BUSY_STATUS, 1u << 23, 0, 10000);
	} else {
		gmu_wr(g, GMU_PWR_COL_KEEPALIVE, 0);
		for (int i = 0; i < 4; i++) {
			(void)sgpu_poll(g->rscc, rscc_tcs_status[i], 1, 1, i == 3 ? 1000 : 10000);
		}
		gmu_wr(g, GMU_AHB_FENCE_STATUS_CLR, 7);
		gmu_wr(g, GMU_AO_AHB_FENCE_CTRL, 0);
		sgpu_wmb();
		gmu_wr(g, GMU_CM3_SYSRESET, 1);
	}
	hfi_reset_queues(m);
	gmu_wr(g, GMU_AO_HOST_INTERRUPT_MASK, 0xffffffffu);
	gmu_wr(g, GMU_GMU2HOST_INTR_MASK, 0xffffffffu);
	rpmh_stop(g);
	m->running = false;
}
#endif
