/*
 * sweet_gpu_smmu.c - Adreno SMMU (ARM MMU-500, "qcom,sm8250-smmu-500",
 * 0x03da0000) for the GPU (SID 0, mask 0x401) and the GMU (SID 5, mask 0x400).
 *
 * Spec: specs/smmu-a650.md. Linux arm-smmu/io-pgtable-arm were used as
 * documentation only; this is an independent implementation.
 *
 * What it does and why
 * --------------------
 *  - The GPU and the GMU fetch everything (ringbuffer, shaders, GMU code,
 *    HFI queues) through this SMMU. Without a translating context bank for
 *    their stream IDs, the first GPU/GMU memory access faults (or, if
 *    firmware left the stream in bypass, goes to a raw physical address).
 *  - GPU -> context bank 0. Mandatory: CP_SMMU_TABLE_UPDATE hard-codes
 *    CONTEXTBANK 0 and TZ grants the CP access only to CB0 (the stock DT's
 *    qcom,gpu-offset 0xa8000 is exactly CB0 = 0x3da8000).
 *  - GMU -> the lowest free context bank >= 1 (CB1 on Linux).
 *  - Stage 1 only (CBAR type "S1 translate, S2 bypass"), AArch64 LPAE,
 *    4 KiB granule, 39-bit input (3 levels: 1G/2M/4K), one TTBR0 table per
 *    domain (spec section 6 "option A": no split TTBR, no per-process
 *    tables, no SCM aperture call needed yet).
 *
 * Hypervisor safety (SM8250 production devices run QHEE, which traps and
 * emulates the SMMU global register space):
 *  - no global reset: we never rewrite SMRs/S2CRs/CBs we did not pick,
 *    never issue TLBIALLNSNH/TLBIALLH, never touch sACR;
 *  - every store uses a real register as source (sweet_gpu.h sgpu_wr):
 *    the GPU's S2CR value is exactly 0, which a "str wzr" would break;
 *  - SMR and S2CR writes are read back: QHEE filters some of them.
 *
 * Page tables live in write-combined (Normal non-cacheable) memory and the
 * TCR declares a non-cacheable walk, so no cache maintenance is needed for
 * PTE updates and the setup is correct whether or not the SMMU's walker
 * port is really IO-coherent (upstream DT says dma-coherent, the stock DT
 * does not). A DSB orders PTE writes before any TLB operation or before
 * handing an address to the GPU.
 *
 * Tables are never freed while the driver runs (an unmapped range keeps
 * its empty L2/L3 tables): the VA allocator reuses low addresses first,
 * so the table count is bounded by peak GPU memory (1 L3 table per 2 MiB).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(SGPU_HOST)
#include "sweet_gpu.h"

/* ---- GR0 ---- */
#define SMMU_sCR0               0x000
#define  sCR0_CLIENTPD          (1u << 0)
#define  sCR0_GFRE              (1u << 1)
#define  sCR0_GFIE              (1u << 2)
#define  sCR0_EXIDENABLE        (1u << 3)
#define  sCR0_USFCFG            (1u << 10)
#define  sCR0_VMID16EN          (1u << 31)
#define SMMU_ID0                0x020
#define  ID0_S1TS               (1u << 30)
#define  ID0_SMS                (1u << 27)
#define  ID0_CTTW               (1u << 14)
#define  ID0_NUMSIDB(v)         (((v) >> 9) & 0xf)
#define  ID0_EXIDS              (1u << 8)
#define  ID0_NUMSMRG(v)         ((v) & 0xff)
#define SMMU_ID1                0x024
#define  ID1_PAGESIZE           (1u << 31)
#define  ID1_NUMPAGENDXB(v)     (((v) >> 28) & 7)
#define  ID1_NUMCB(v)           ((v) & 0xff)
#define SMMU_ID2                0x028
#define  ID2_PTFS_4K            (1u << 12)
#define  ID2_UBS(v)             (((v) >> 8) & 0xf)
#define  ID2_OAS(v)             (((v) >> 4) & 0xf)
#define  ID2_IAS(v)             ((v) & 0xf)
#define SMMU_ID7                0x03c
#define SMMU_sGFSR              0x048
#define SMMU_sGFSYNR0           0x050
#define SMMU_sGFSYNR1           0x054
#define SMMU_sGFSYNR2           0x058
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define  SMR_VALID              (1u << 31)
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define  S2CR_TYPE(v)           (((v) >> 16) & 3)
#define  S2CR_TYPE_TRANS        0
#define  S2CR_TYPE_BYPASS       1
#define  S2CR_TYPE_FAULT        2
#define  S2CR_EXIDVALID         (1u << 10)
#define  S2CR_CBNDX(v)          ((v) & 0xff)

/* ---- GR1 ---- */
#define SMMU_CBAR(n)            (0x000 + 4 * (n))
#define SMMU_CBFRSYNRA(n)       (0x400 + 4 * (n))
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define  CBA2R_VA64             (1u << 0)

/* ---- context bank ---- */
#define CB_SCTLR                0x000
#define  SCTLR_M                (1u << 0)
#define  SCTLR_TRE              (1u << 1)
#define  SCTLR_AFE              (1u << 2)
#define  SCTLR_CFRE             (1u << 5)
#define  SCTLR_CFIE             (1u << 6)
#define  SCTLR_HUPCF            (1u << 8)
#define  SCTLR_ASIDPNE          (1u << 12)
#define CB_ACTLR                0x004
#define CB_RESUME               0x008
#define CB_TCR2                 0x010
#define CB_TTBR0                0x020
#define CB_TTBR1                0x028
#define CB_TCR                  0x030
#define CB_MAIR0                0x038
#define CB_MAIR1                0x03c
#define CB_FSR                  0x058
#define  FSR_SS                 (1u << 30)
#define  FSR_FAULT              0xc00001feu
#define CB_FAR                  0x060
#define CB_FSYNR0               0x068
#define CB_FSYNR1               0x06c
#define CB_TLBIASID             0x610
#define CB_TLBSYNC              0x7f0
#define CB_TLBSTATUS            0x7f4

/* values (spec 3.2-3.6) */
#define IAS_BITS                39
#define CBAR_S1_BYPASS_S2       0x0001f300u     /* TYPE=1, BPSHCFG=NSH, MEMATTR=WB */
#define ACTLR_PREFETCH          0x00000303u     /* CMTLB | CPRE | deep prefetch (upstream) */
#define MAIR0_VALUE             0xf404ff44u     /* 0:NC 1:WB-RWA 2:Device-nGnRE 3:iNC/oWB */
#define SCTLR_VALUE             (SCTLR_M | SCTLR_TRE | SCTLR_AFE | SCTLR_CFRE | \
	                         SCTLR_CFIE | SCTLR_HUPCF | SCTLR_ASIDPNE)     /* 0x1167 */
/* TTBR0 only, EPD1, 4K, T0SZ = 25; walk Normal non-cacheable, outer-shareable */
#define TCR_TTBR0_NC            (0x00800000u | (2u << 12) | (0u << 10) | (0u << 8) | (64 - IAS_BITS))
#define TCR2_BASE               0x00038010u     /* SEP=7 (upstream), AS=16-bit ASID */

/* LPAE stage-1 descriptors */
#define PTE_VALID_TABLE         3ull            /* table (L1/L2) or page (L3) */
#define PTE_ATTRIDX(n)          ((uint64_t)(n) << 2)
#define PTE_AP_UNPRIV           (1ull << 6)
#define PTE_AP_RDONLY           (1ull << 7)
#define PTE_SH_OS               (2ull << 8)
#define PTE_SH_IS               (3ull << 8)
#define PTE_AF                  (1ull << 10)
#define PTE_NG                  (1ull << 11)
#define PTE_ADDR_MASK           0x0000fffffffff000ull

#define GRANULE                 0x1000ull
#define CHUNK_SIZE              0x10000u        /* 64 KiB of tables per DMA allocation */
#define UNITS_PER_CHUNK         (CHUNK_SIZE / GRANULE)
#define MAX_CHUNKS              2048            /* 128 MiB of tables = 64 GiB mapped */

struct smmu_dom {
	bool      active;
	uint32_t  sid, mask;
	int       cb;
	int       sme;
	bool      sme_reused;           /* covered by a pre-existing SMR */
	uint32_t  asid;
	uint64_t  root_pa;
	uint64_t *root;
	uint32_t  smr_val, s2cr_val;
	uint32_t  old_s2cr;             /* for the log */
};

struct sgpu_smmu {
	uint32_t pgshift, numpage, numcb, numsmr, oas_bits, ubs_bits, sid_bits;
	uint32_t id0, id1, id2, id7, scr0;
	bool     exids, vmid16;
	struct smmu_dom dom[SGPU_DOM_COUNT];
	struct sgpu_dma chunk[MAX_CHUNKS];
	uint32_t nchunks, next_unit;    /* bump allocator inside chunk[nchunks-1] */
	uint64_t tables;
};

static struct sgpu_smmu smmu_storage;

static inline volatile uint8_t *gr0(struct sgpu *g) { return g->smmu; }
static inline volatile uint8_t *gr1(struct sgpu *g) { return g->smmu + (1u << g->smmu_state->pgshift); }
static inline volatile uint8_t *
cbase(struct sgpu *g, int cb)
{
	struct sgpu_smmu *s = g->smmu_state;
	return g->smmu + ((s->numpage + (uint32_t)cb) << s->pgshift);
}

static uint32_t
id_size_to_bits(uint32_t f)
{
	static const uint32_t bits[] = { 32, 36, 40, 42, 44 };
	return f < 5 ? bits[f] : 48;
}

static uint32_t
oas_to_ips(uint32_t bits)
{
	switch (bits) {
	case 32: return 0;
	case 36: return 1;
	case 40: return 2;
	case 42: return 3;
	case 44: return 4;
	default: return 5;
	}
}

/* ------------------------------------------------------------------ */
/* page-table memory                                                  */
/* ------------------------------------------------------------------ */

static uint64_t *
table_alloc(struct sgpu_smmu *s, uint64_t *pa)
{
	if (s->nchunks == 0 || s->next_unit == UNITS_PER_CHUNK) {
		if (s->nchunks == MAX_CHUNKS) {
			return NULL;
		}
		if (sgpu_dma_alloc(CHUNK_SIZE, CHUNK_SIZE, SGPU_DMA_WC, &s->chunk[s->nchunks])) {
			return NULL;
		}
		if (s->chunk[s->nchunks].pa + CHUNK_SIZE > (1ull << s->oas_bits)) {
			sgpu_log("smmu: table memory %#llx above the %u-bit output size",
			    (unsigned long long)s->chunk[s->nchunks].pa, s->oas_bits);
			sgpu_dma_free(&s->chunk[s->nchunks]);
			return NULL;
		}
		s->nchunks++;
		s->next_unit = 0;
	}
	struct sgpu_dma *c = &s->chunk[s->nchunks - 1];
	uint32_t u = s->next_unit++;
	*pa = c->pa + u * GRANULE;
	s->tables++;
	return (uint64_t *)(void *)((uint8_t *)c->va + u * GRANULE);     /* zeroed by dma_alloc */
}

/* next-level table PA -> its CPU address (linear in the number of chunks,
 * done once per 2 MiB of a map/unmap) */
static uint64_t *
table_va(struct sgpu_smmu *s, uint64_t pa)
{
	for (uint32_t i = 0; i < s->nchunks; i++) {
		if (pa >= s->chunk[i].pa && pa < s->chunk[i].pa + CHUNK_SIZE) {
			return (uint64_t *)(void *)((uint8_t *)s->chunk[i].va + (pa - s->chunk[i].pa));
		}
	}
	return NULL;
}

/* L3 table covering iova, optionally creating L2/L3 tables on the way. */
static uint64_t *
walk_l3(struct sgpu_smmu *s, struct smmu_dom *d, uint64_t iova, bool create)
{
	uint64_t *t = d->root;
	for (int level = 1; level <= 2; level++) {
		uint32_t idx = (uint32_t)(iova >> (level == 1 ? 30 : 21)) & 0x1ff;
		uint64_t e = t[idx];
		if ((e & 3) == 0) {
			if (!create) {
				return NULL;
			}
			uint64_t pa;
			uint64_t *n = table_alloc(s, &pa);
			if (n == NULL) {
				return NULL;
			}
			sgpu_wmb();             /* zeroed table visible before it is linked */
			t[idx] = pa | PTE_VALID_TABLE;
			t = n;
			continue;
		}
		if ((e & 3) != 3) {
			return NULL;            /* block mapping: never created by this driver */
		}
		t = table_va(s, e & PTE_ADDR_MASK);
		if (t == NULL) {
			return NULL;
		}
	}
	return t;
}

static uint64_t
make_pte(uint64_t pa, uint32_t prot)
{
	uint64_t pte = (pa & PTE_ADDR_MASK) | PTE_VALID_TABLE | PTE_AF | PTE_NG;

	if (prot & SGPU_PROT_CACHED) {
		pte |= PTE_ATTRIDX(1) | PTE_SH_IS;
	} else {
		pte |= PTE_ATTRIDX(0) | PTE_SH_OS;
	}
	if (!(prot & SGPU_PROT_PRIV)) {
		pte |= PTE_AP_UNPRIV;
	}
	if (!(prot & SGPU_PROT_WRITE)) {
		pte |= PTE_AP_RDONLY;
	}
	/* XN is never set: the GPU/GMU fetch shaders/code from any buffer
	 * (Linux msm never passes IOMMU_NOEXEC either). */
	return pte;
}

/* ------------------------------------------------------------------ */
/* TLB                                                                */
/* ------------------------------------------------------------------ */

static int
cb_tlb_sync(struct sgpu *g, int cb)
{
	volatile uint8_t *c = cbase(g, cb);
	uint64_t start = sgpu_now_us();

	sgpu_wr(c, CB_TLBSYNC, 0xffffffffu);
	while (sgpu_rd(c, CB_TLBSTATUS) & 1) {
		if (sgpu_now_us() - start > 1000000) {
			sgpu_log("smmu: CB%d TLB sync timed out -- SMMU may be deadlocked", cb);
			return -60;
		}
		sgpu_udelay(1);
	}
	return 0;
}

static void
dom_tlb_inval(struct sgpu *g, struct smmu_dom *d)
{
	sgpu_wmb();                     /* PTE updates before the invalidate */
	sgpu_wr(cbase(g, d->cb), CB_TLBIASID, d->asid);
	cb_tlb_sync(g, d->cb);
}

void
sgpu_smmu_tlb_sync(struct sgpu *g, enum sgpu_smmu_domain dom)
{
	struct smmu_dom *d = &g->smmu_state->dom[dom];
	if (d->active) {
		dom_tlb_inval(g, d);
	}
}

/* ------------------------------------------------------------------ */
/* map / unmap                                                        */
/* ------------------------------------------------------------------ */

int
sgpu_smmu_map(struct sgpu *g, enum sgpu_smmu_domain dom, uint64_t iova,
    uint64_t pa, uint64_t size, uint32_t prot)
{
	struct sgpu_smmu *s = g->smmu_state;
	struct smmu_dom *d;
	uint64_t done = 0;

	if (s == NULL || dom >= SGPU_DOM_COUNT || !(d = &s->dom[dom])->root) {
		return -19;     /* ENODEV */
	}
	if (((iova | pa | size) & (GRANULE - 1)) || size == 0 ||
	    iova + size > (1ull << IAS_BITS) || iova + size < iova ||
	    pa + size > (1ull << s->oas_bits)) {
		sgpu_log("smmu: bad map dom %d iova %#llx pa %#llx size %#llx", dom,
		    (unsigned long long)iova, (unsigned long long)pa, (unsigned long long)size);
		return -22;
	}
	while (done < size) {
		uint64_t va = iova + done;
		uint64_t *l3 = walk_l3(s, d, va, true);
		if (l3 == NULL) {
			goto fail;
		}
		/* fill this L3 table as far as the range goes */
		for (uint32_t i = (uint32_t)(va >> 12) & 0x1ff; i < 512 && done < size; i++) {
			if (l3[i] & 1) {
				sgpu_log("smmu: dom %d iova %#llx already mapped", dom,
				    (unsigned long long)(iova + done));
				goto fail;
			}
			l3[i] = make_pte(pa + done, prot);
			done += GRANULE;
		}
	}
	sgpu_wmb();     /* map needs no TLB maintenance, only ordering */
	return 0;
fail:
	if (done) {
		sgpu_smmu_unmap(g, dom, iova, done);
	}
	return -12;
}

int
sgpu_smmu_unmap(struct sgpu *g, enum sgpu_smmu_domain dom, uint64_t iova, uint64_t size)
{
	struct sgpu_smmu *s = g->smmu_state;
	struct smmu_dom *d;
	uint64_t done = 0;

	if (s == NULL || dom >= SGPU_DOM_COUNT || !(d = &s->dom[dom])->root) {
		return -19;
	}
	size = (size + GRANULE - 1) & ~(GRANULE - 1);
	while (done < size) {
		uint64_t va = iova + done;
		uint64_t *l3 = walk_l3(s, d, va, false);
		if (l3 == NULL) {
			/* nothing mapped in this 2 MiB: skip to the next L3 boundary */
			uint64_t next = (va + 0x200000ull) & ~0x1fffffull;
			done = next - iova;
			continue;
		}
		for (uint32_t i = (uint32_t)(va >> 12) & 0x1ff; i < 512 && done < size; i++) {
			l3[i] = 0;
			done += GRANULE;
		}
	}
	if (d->active) {
		dom_tlb_inval(g, d);
	}
	return 0;
}

uint64_t
sgpu_smmu_ttbr0(struct sgpu *g, enum sgpu_smmu_domain dom)
{
	struct smmu_dom *d = &g->smmu_state->dom[dom];
	return d->root_pa | ((uint64_t)d->asid << 48);
}

/* ------------------------------------------------------------------ */
/* discovery and stream/context-bank selection                        */
/* ------------------------------------------------------------------ */

static bool
sme_valid(struct sgpu_smmu *s, uint32_t smr, uint32_t s2cr)
{
	return s->exids ? (s2cr & S2CR_EXIDVALID) != 0 : (smr & SMR_VALID) != 0;
}

/*
 * Linux arm_smmu_find_sme semantics: an existing valid SMR that fully
 * covers (id, mask) is reused; any partial overlap is an error; otherwise
 * the first invalid index. Indices already claimed by the other domain
 * are skipped.
 */
static int
find_sme(struct sgpu *g, uint32_t id, uint32_t mask, int taken, bool *reused)
{
	struct sgpu_smmu *s = g->smmu_state;
	int free_idx = -1;

	*reused = false;
	for (uint32_t i = 0; i < s->numsmr; i++) {
		if ((int)i == taken) {
			continue;
		}
		uint32_t smr = sgpu_rd(gr0(g), SMMU_SMR(i));
		uint32_t s2cr = sgpu_rd(gr0(g), SMMU_S2CR(i));
		if (!sme_valid(s, smr, s2cr)) {
			if (free_idx < 0) {
				free_idx = (int)i;
			}
			continue;
		}
		uint32_t eid = smr & 0xffff;
		uint32_t emask = (smr >> 16) & (s->exids ? 0xffff : 0x7fff);
		if ((mask & emask) == mask && !((id ^ eid) & ~emask)) {
			*reused = true;
			return (int)i;
		}
		if (!((id ^ eid) & ~(emask | mask))) {
			sgpu_log("smmu: SID %#x/%#x overlaps foreign SMR%u %#x (S2CR %#x)",
			    id, mask, i, smr, s2cr);
			return -17;
		}
	}
	return free_idx >= 0 ? free_idx : -28;
}

/* Is context bank cb referenced by a valid SME other than `ignore`? */
static bool
cb_referenced(struct sgpu *g, int cb, int ignore)
{
	struct sgpu_smmu *s = g->smmu_state;
	for (uint32_t i = 0; i < s->numsmr; i++) {
		if ((int)i == ignore) {
			continue;
		}
		uint32_t smr = sgpu_rd(gr0(g), SMMU_SMR(i));
		uint32_t s2cr = sgpu_rd(gr0(g), SMMU_S2CR(i));
		if (sme_valid(s, smr, s2cr) && S2CR_TYPE(s2cr) == S2CR_TYPE_TRANS &&
		    S2CR_CBNDX(s2cr) == (uint32_t)cb) {
			return true;
		}
	}
	return false;
}

static void
snapshot(struct sgpu *g)
{
	struct sgpu_smmu *s = g->smmu_state;

	for (uint32_t i = 0; i < s->numsmr; i++) {
		uint32_t smr = sgpu_rd(gr0(g), SMMU_SMR(i));
		uint32_t s2cr = sgpu_rd(gr0(g), SMMU_S2CR(i));
		if (sme_valid(s, smr, s2cr)) {
			sgpu_log("smmu: firmware SME%u: SMR %#010x S2CR %#010x (type %u cb %u)",
			    i, smr, s2cr, S2CR_TYPE(s2cr), S2CR_CBNDX(s2cr));
		}
	}
	for (uint32_t cb = 0; cb < s->numcb; cb++) {
		uint32_t sctlr = sgpu_rd(cbase(g, (int)cb), CB_SCTLR);
		if (sctlr & SCTLR_M) {
			sgpu_log("smmu: firmware CB%u live: SCTLR %#x CBAR %#x TCR %#x", cb, sctlr,
			    sgpu_rd(gr1(g), SMMU_CBAR(cb)), sgpu_rd(cbase(g, (int)cb), CB_TCR));
		}
	}
}

static int
probe_ids(struct sgpu *g)
{
	struct sgpu_smmu *s = g->smmu_state;

	s->id0 = sgpu_rd(gr0(g), SMMU_ID0);
	s->id1 = sgpu_rd(gr0(g), SMMU_ID1);
	s->id2 = sgpu_rd(gr0(g), SMMU_ID2);
	s->id7 = sgpu_rd(gr0(g), SMMU_ID7);
	s->scr0 = sgpu_rd(gr0(g), SMMU_sCR0);
	sgpu_log("smmu: ID0 %#x ID1 %#x ID2 %#x ID7 %#x (MMU-500 r%up%u) sCR0 %#x",
	    s->id0, s->id1, s->id2, s->id7, (s->id7 >> 4) & 0xf, s->id7 & 0xf, s->scr0);
	if (s->id0 == 0xffffffffu || s->id1 == 0xffffffffu) {
		return sgpu_fail(g, -5, "smmu: registers read all-ones (CX GDSC/AHB clock off?)");
	}
	s->pgshift = (s->id1 & ID1_PAGESIZE) ? 16 : 12;
	s->numpage = 1u << (ID1_NUMPAGENDXB(s->id1) + 1);
	s->numcb = ID1_NUMCB(s->id1);
	s->numsmr = (s->id0 & ID0_SMS) ? ID0_NUMSMRG(s->id0) : 0;
	if (s->numsmr > 128) {
		s->numsmr = 128;
	}
	s->exids = (s->scr0 & sCR0_EXIDENABLE) != 0;    /* live config, not ID0 */
	s->vmid16 = (s->scr0 & sCR0_VMID16EN) != 0;
	s->sid_bits = s->exids ? 16 : ID0_NUMSIDB(s->id0);
	s->oas_bits = id_size_to_bits(ID2_IAS(s->id2));     /* S1 output = IPA size */
	s->ubs_bits = id_size_to_bits(ID2_UBS(s->id2));
	sgpu_log("smmu: page %u KiB, %u pages/half, %u CBs, %u SMRs, %u-bit SIDs, "
	    "S1 in %u / out %u bits, %s walk advertised, EXIDS %d VMID16 %d",
	    1u << (s->pgshift - 10), s->numpage, s->numcb, s->numsmr, s->sid_bits,
	    s->ubs_bits, s->oas_bits, (s->id0 & ID0_CTTW) ? "coherent" : "non-coherent",
	    s->exids, s->vmid16);

	if ((2u * s->numpage) << s->pgshift > 0x10000) {
		return sgpu_fail(g, -22, "smmu: %u-page layout does not fit the 64 KiB window",
		           s->numpage);
	}
	if (!(s->id0 & ID0_S1TS) || !(s->id2 & ID2_PTFS_4K)) {
		return sgpu_fail(g, -22, "smmu: no stage-1 / 4K granule support");
	}
	if (s->numsmr == 0 || s->numcb < 2) {
		return sgpu_fail(g, -22, "smmu: %u SMRs, %u CBs", s->numsmr, s->numcb);
	}
	if (s->ubs_bits < IAS_BITS || s->sid_bits < 11) {
		return sgpu_fail(g, -22, "smmu: input %u bits / SID %u bits too small",
		           s->ubs_bits, s->sid_bits);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* programming                                                        */
/* ------------------------------------------------------------------ */

static int
program_cb(struct sgpu *g, struct smmu_dom *d)
{
	struct sgpu_smmu *s = g->smmu_state;
	volatile uint8_t *c = cbase(g, d->cb);
	uint32_t vmid = (uint32_t)d->cb;

	sgpu_wr(c, CB_SCTLR, 0);
	sgpu_wr(c, CB_FSR, FSR_FAULT);
	sgpu_wr(c, CB_ACTLR, ACTLR_PREFETCH);
	sgpu_wr(gr1(g), SMMU_CBA2R(d->cb), CBA2R_VA64 | (s->vmid16 ? vmid << 16 : 0));
	sgpu_wr(gr1(g), SMMU_CBAR(d->cb), CBAR_S1_BYPASS_S2 | (s->vmid16 ? 0 : (vmid & 0xff)));
	sgpu_wr(c, CB_TCR2, TCR2_BASE | oas_to_ips(s->oas_bits));
	sgpu_wr(c, CB_TCR, TCR_TTBR0_NC);
	sgpu_wr64(c, CB_TTBR0, d->root_pa | ((uint64_t)d->asid << 48));
	sgpu_wr64(c, CB_TTBR1, (uint64_t)d->asid << 48);
	sgpu_wr(c, CB_MAIR0, MAIR0_VALUE);
	sgpu_wr(c, CB_MAIR1, 0);
	sgpu_wr(c, CB_TLBIASID, d->asid);
	if (cb_tlb_sync(g, d->cb)) {
		return sgpu_fail(g, -60, "smmu: CB%d TLB sync timeout", d->cb);
	}
	sgpu_wr(c, CB_SCTLR, SCTLR_VALUE);

	uint32_t actlr = sgpu_rd(c, CB_ACTLR), sctlr = sgpu_rd(c, CB_SCTLR);
	uint64_t ttbr0 = sgpu_rd64(c, CB_TTBR0);
	/*
	 * The Qualcomm hypervisor keeps ASIDPNE clear in HLOS context banks
	 * (one ASID namespace, as the stock DT's qcom,no-dynamic-asid says).
	 * Each domain has its own ASID, so a shared namespace is harmless.
	 */
	if ((sctlr | SCTLR_ASIDPNE) != SCTLR_VALUE ||
	    ttbr0 != (d->root_pa | ((uint64_t)d->asid << 48))) {
		return sgpu_fail(g, -5, "smmu: CB%d did not take SCTLR %#x/TTBR0 %#llx",
		           d->cb, sctlr, (unsigned long long)ttbr0);
	}
	if (!(sctlr & SCTLR_ASIDPNE)) {
		sgpu_log("smmu: CB%d SCTLR reads %#x: ASIDPNE kept clear by the hypervisor",
		    d->cb, sctlr);
	}
	if (actlr != ACTLR_PREFETCH) {
		sgpu_log("smmu: CB%d ACTLR reads %#x (sACR.CACHE_LOCK set by secure world; "
		    "prefetch hints only)", d->cb, actlr);
	}
	return 0;
}

static int
program_sme(struct sgpu *g, struct smmu_dom *d)
{
	/* S2CR first, then SMR (a valid SMR must never point at a stale S2CR) */
	sgpu_wr(gr0(g), SMMU_S2CR(d->sme), d->s2cr_val);
	sgpu_wr(gr0(g), SMMU_SMR(d->sme), d->smr_val);
	uint32_t s2cr = sgpu_rd(gr0(g), SMMU_S2CR(d->sme));
	uint32_t smr = sgpu_rd(gr0(g), SMMU_SMR(d->sme));
	if (s2cr != d->s2cr_val || smr != d->smr_val) {
		return sgpu_fail(g, -1, "smmu: SME%d write filtered (hypervisor?): "
		           "S2CR %#x want %#x, SMR %#x want %#x", d->sme, s2cr, d->s2cr_val,
		           smr, d->smr_val);
	}
	return 0;
}

static int
setup_dom(struct sgpu *g, enum sgpu_smmu_domain dom, uint32_t sid, uint32_t mask)
{
	struct sgpu_smmu *s = g->smmu_state;
	struct smmu_dom *d = &s->dom[dom];
	struct smmu_dom *other = &s->dom[dom == SGPU_DOM_GPU ? SGPU_DOM_GMU : SGPU_DOM_GPU];
	uint32_t force = 0;

	(void)sgpu_bootarg_u32("sweetgpu_smmu_force", &force);
	d->sid = sid;
	d->mask = mask;
	if ((sid | mask) >> s->sid_bits) {
		return sgpu_fail(g, -22, "smmu: SID %#x mask %#x exceed %u SID bits",
		           sid, mask, s->sid_bits);
	}
	int sme = find_sme(g, sid, mask, other->active ? other->sme : -1, &d->sme_reused);
	if (sme < 0) {
		return sgpu_fail(g, sme, "smmu: no stream match entry for SID %#x/%#x", sid, mask);
	}
	d->sme = sme;
	d->old_s2cr = sgpu_rd(gr0(g), SMMU_S2CR(sme));

	/* context bank */
	if (dom == SGPU_DOM_GPU) {
		d->cb = 0;
		bool live = (sgpu_rd(cbase(g, 0), CB_SCTLR) & SCTLR_M) != 0;
		bool used = cb_referenced(g, 0, d->sme_reused ? sme : -1);
		if ((live || used) && !force) {
			return sgpu_fail(g, -16, "smmu: CB0 is in use by firmware (live %d, "
			           "referenced %d); boot with sweetgpu_smmu_force=1 to take it",
			           live, used);
		}
	} else {
		d->cb = -1;
		for (uint32_t cb = 1; cb < s->numcb; cb++) {
			if (other->active && (int)cb == other->cb) {
				continue;
			}
			if (sgpu_rd(cbase(g, (int)cb), CB_SCTLR) & SCTLR_M) {
				continue;
			}
			if (cb_referenced(g, (int)cb, d->sme_reused ? sme : -1)) {
				continue;
			}
			d->cb = (int)cb;
			break;
		}
		if (d->cb < 0) {
			return sgpu_fail(g, -28, "smmu: no free context bank for the GMU");
		}
	}
	if (d->sme_reused && S2CR_TYPE(d->old_s2cr) == S2CR_TYPE_TRANS &&
	    (int)S2CR_CBNDX(d->old_s2cr) != d->cb &&
	    (sgpu_rd(cbase(g, (int)S2CR_CBNDX(d->old_s2cr)), CB_SCTLR) & SCTLR_M) && !force) {
		return sgpu_fail(g, -16, "smmu: SID %#x already translated by live CB%u",
		           sid, S2CR_CBNDX(d->old_s2cr));
	}
	d->asid = (uint32_t)d->cb;

	/* root table */
	d->root = table_alloc(s, &d->root_pa);
	if (d->root == NULL) {
		return sgpu_fail(g, -12, "smmu: no page-table memory");
	}

	d->s2cr_val = (S2CR_TYPE_TRANS << 16) | (uint32_t)d->cb | (s->exids ? S2CR_EXIDVALID : 0);
	if (d->sme_reused) {
		d->smr_val = sgpu_rd(gr0(g), SMMU_SMR(sme));     /* keep the covering SMR */
	} else {
		d->smr_val = sid | (mask << 16) | (s->exids ? 0 : SMR_VALID);
	}

	int err = program_cb(g, d);
	if (!err) {
		err = program_sme(g, d);
	}
	if (err) {
		return err;
	}
	d->active = true;
	sgpu_log("smmu: %s SID %#x/%#x -> SME%d%s (old S2CR %#x) -> CB%d ASID %u, "
	    "TTBR0 %#llx", dom == SGPU_DOM_GPU ? "GPU" : "GMU", sid, mask, d->sme,
	    d->sme_reused ? " (reused firmware SMR)" : "", d->old_s2cr, d->cb, d->asid,
	    (unsigned long long)d->root_pa);
	return 0;
}

int
sgpu_smmu_init(struct sgpu *g)
{
	struct sgpu_smmu *s = &smmu_storage;
	int err;

	if (g->smmu_state == NULL) {        /* first init of this device */
		for (size_t i = 0; i < sizeof(*s); i++) {
			((uint8_t *)s)[i] = 0;
		}
		g->smmu_state = s;
	}
	if ((err = probe_ids(g)) != 0) {
		return err;
	}
	snapshot(g);

	uint32_t gfsr = sgpu_rd(gr0(g), SMMU_sGFSR);
	if (gfsr) {
		sgpu_log("smmu: stale global fault sGFSR %#x SYNR0 %#x SYNR1 %#x", gfsr,
		    sgpu_rd(gr0(g), SMMU_sGFSYNR0), sgpu_rd(gr0(g), SMMU_sGFSYNR1));
		sgpu_wr(gr0(g), SMMU_sGFSR, gfsr);
	}

	if ((err = setup_dom(g, SGPU_DOM_GPU, 0x000, 0x401)) != 0) {
		return err;
	}
	if ((err = setup_dom(g, SGPU_DOM_GMU, 0x005, 0x400)) != 0) {
		return err;
	}

	/*
	 * sCR0.CLIENTPD = 1 means the whole SMMU bypasses: our banks would
	 * never be used. Clear only that bit (and enable global fault
	 * reporting); leave USFCFG/VMID16EN/EXIDENABLE etc. as firmware set
	 * them, since they define how everybody else's entries are decoded.
	 */
	uint32_t scr0 = sgpu_rd(gr0(g), SMMU_sCR0);
	if (scr0 & sCR0_CLIENTPD) {
		sgpu_log("smmu: sCR0 %#x has CLIENTPD set (global bypass) -- clearing it", scr0);
		sgpu_wr(gr0(g), SMMU_sCR0, (scr0 & ~sCR0_CLIENTPD) | sCR0_GFRE | sCR0_GFIE);
		scr0 = sgpu_rd(gr0(g), SMMU_sCR0);
		if (scr0 & sCR0_CLIENTPD) {
			return sgpu_fail(g, -1, "smmu: sCR0.CLIENTPD write ignored (%#x)", scr0);
		}
	}
	sgpu_log("smmu: ready, sCR0 %#x, %llu table pages", scr0,
	    (unsigned long long)s->tables);
	return 0;
}

int
sgpu_smmu_restore(struct sgpu *g)
{
	struct sgpu_smmu *s = g->smmu_state;
	int err;

	if (s == NULL) {
		return -19;
	}
	for (int i = 0; i < SGPU_DOM_COUNT; i++) {
		struct smmu_dom *d = &s->dom[i];
		if (!d->active) {
			continue;
		}
		if ((err = program_cb(g, d)) != 0 || (err = program_sme(g, d)) != 0) {
			return err;
		}
	}
	return 0;
}

/* Registers of a domain's bank as read back, and the walk for one IOVA. */
void
sgpu_smmu_debug(struct sgpu *g, enum sgpu_smmu_domain dom, uint64_t iova)
{
	struct sgpu_smmu *s = g->smmu_state;
	if (s == NULL || dom >= SGPU_DOM_COUNT || !s->dom[dom].root) {
		return;
	}
	struct smmu_dom *d = &s->dom[dom];
	volatile uint8_t *c = cbase(g, d->cb);
	sgpu_log("smmu: CB%d CBAR %#x CBA2R %#x TCR %#x TCR2 %#x TTBR0 %#llx MAIR0 %#x",
	    d->cb, sgpu_rd(gr1(g), SMMU_CBAR(d->cb)), sgpu_rd(gr1(g), SMMU_CBA2R(d->cb)),
	    sgpu_rd(c, CB_TCR), sgpu_rd(c, CB_TCR2), (unsigned long long)sgpu_rd64(c, CB_TTBR0),
	    sgpu_rd(c, CB_MAIR0));
	uint64_t *t = d->root;
	for (int level = 1; level <= 3 && t != NULL; level++) {
		uint32_t idx = (uint32_t)(iova >> (level == 1 ? 30 : level == 2 ? 21 : 12)) & 0x1ff;
		uint64_t e = t[idx];
		sgpu_log("smmu: walk %#llx L%d[%u] = %#llx", (unsigned long long)iova, level, idx,
		    (unsigned long long)e);
		if (level == 3 || (e & 3) != 3) {
			break;
		}
		t = table_va(s, e & PTE_ADDR_MASK);
	}
}

uint32_t
sgpu_smmu_check_faults(struct sgpu *g)
{
	struct sgpu_smmu *s = g->smmu_state;
	uint32_t n = 0;

	if (s == NULL || g->stage_reached < SGPU_STAGE_SMMU) {
		return 0;
	}
	for (int i = 0; i < SGPU_DOM_COUNT; i++) {
		struct smmu_dom *d = &s->dom[i];
		if (!d->active) {
			continue;
		}
		volatile uint8_t *c = cbase(g, d->cb);
		uint32_t fsr = sgpu_rd(c, CB_FSR);
		if (!(fsr & FSR_FAULT)) {
			continue;
		}
		uint64_t far = sgpu_rd64(c, CB_FAR);
		uint32_t syn0 = sgpu_rd(c, CB_FSYNR0), syn1 = sgpu_rd(c, CB_FSYNR1);
		uint32_t sid = sgpu_rd(gr1(g), SMMU_CBFRSYNRA(d->cb)) & 0xffff;
		uint32_t cl = syn1 & 0xff;
		sgpu_log("smmu: %s fault CB%d FSR %#x (%s%s%s%s) FAR %#llx %s SID %#x "
		    "FSYNR0 %#x FSYNR1 %#x client %s", i == SGPU_DOM_GPU ? "GPU" : "GMU",
		    d->cb, fsr, (fsr & 2) ? "translation " : "", (fsr & 8) ? "permission " : "",
		    (fsr & 0x10) ? "external " : "", (fsr & 0x80) ? "address-size " : "",
		    (unsigned long long)far, (syn0 & 0x10) ? "write" : "read", sid, syn0, syn1,
		    i == SGPU_DOM_GPU ? (cl < 8 ? sgpu_nth("CP\0?\0?\0?\0CCU\0?\0CDP\0GMU\0", cl) :
		    "UCHE") : "GMU");
		sgpu_wr(c, CB_FSR, fsr);
		if (fsr & FSR_SS) {
			sgpu_wr(c, CB_RESUME, 1);       /* terminate the stalled transaction */
		}
		n++;
	}
	uint32_t gfsr = sgpu_rd(gr0(g), SMMU_sGFSR);
	if (gfsr) {
		sgpu_log("smmu: global fault sGFSR %#x SYNR0 %#x SYNR1 %#x (SID %#x) SYNR2 %#x",
		    gfsr, sgpu_rd(gr0(g), SMMU_sGFSYNR0), sgpu_rd(gr0(g), SMMU_sGFSYNR1),
		    sgpu_rd(gr0(g), SMMU_sGFSYNR1) & 0xffff, sgpu_rd(gr0(g), SMMU_sGFSYNR2));
		sgpu_wr(gr0(g), SMMU_sGFSR, gfsr);
		n++;
	}
	g->faults += n;
	return n;
}
#endif
