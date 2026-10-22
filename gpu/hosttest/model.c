/*
 * model.c - see model.h. Register offsets/values come from the specs in
 * the specs (specs/ directory), written independently of the driver sources where possible
 * (e.g. PM4 parity is recomputed here, SMMU walks start from the live
 * register state, HFI messages are decoded field by field).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "model.h"
#include "../kernel/sweet_gpu.h"

int model_errors;
uint64_t model_now_us;
uint64_t model_accesses;
uint32_t model_hfi_log[64];
int model_hfi_count;
uint32_t model_cp_packets;
uint32_t model_cp_skipped;

static struct model_opts opt;

static void
merr(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "MODEL ERROR: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	model_errors++;
}

/* ------------------------------------------------------------------ */
/* regions                                                            */
/* ------------------------------------------------------------------ */

enum { R_GPU, R_CXDBG, R_GMU, R_GPUCC, R_SMMU, R_RSCC, R_PDC, R_PDCSEQ, R_GCC,
       R_CMDDB, R_QFPROM, R_ZAP, R_COUNT };
struct region { const char *name; uint64_t pa; uint32_t size; uint8_t *mem; };
static struct region regions[R_COUNT] = {
	[R_GPU]    = { "gpu",    0x03d00000, 0x40000 },
	[R_CXDBG]  = { "cxdbg",  0x03d61000, 0x800 },
	[R_GMU]    = { "gmu",    0x03d6a000, 0x30000 },
	[R_GPUCC]  = { "gpucc",  0x03d90000, 0x9000 },
	[R_SMMU]   = { "smmu",   0x03da0000, 0x10000 },
	[R_RSCC]   = { "rscc",   0x03de0000, 0x10000 },
	[R_PDC]    = { "pdc",    0x0b290000, 0x10000 },
	[R_PDCSEQ] = { "pdcseq", 0x0b490000, 0x10000 },
	[R_GCC]    = { "gcc",    0x00100000, 0x1f0000 },
	[R_CMDDB]  = { "cmddb",  0x80860000, 0x20000 },
	[R_QFPROM] = { "qfprom", 0x00784000, 0x1000 },
	[R_ZAP]    = { "zap",    0x8681a000, 0x2000 },
};

static inline uint32_t *reg32(int r, uint32_t off) { return (uint32_t *)(regions[r].mem + off); }

volatile uint8_t *
sgpu_ioremap(uint64_t pa, size_t size)
{
	int best = -1;
	for (int i = 0; i < R_COUNT; i++) {     /* most specific region wins (gmu covers gpucc) */
		if (pa >= regions[i].pa && pa + size <= regions[i].pa + regions[i].size &&
		    (best < 0 || regions[i].size < regions[best].size)) {
			best = i;
		}
	}
	if (best >= 0) {
		return regions[best].mem + (pa - regions[best].pa);
	}
	merr("ioremap of unknown range %#llx+%#zx", (unsigned long long)pa, size);
	return NULL;
}

static int
find_region(volatile uint8_t *p, uint32_t *off)
{
	for (int i = 0; i < R_COUNT; i++) {
		if ((uint8_t *)p >= regions[i].mem && (uint8_t *)p < regions[i].mem + regions[i].size) {
			*off = (uint32_t)((uint8_t *)p - regions[i].mem);
			return i;
		}
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* power state                                                        */
/* ------------------------------------------------------------------ */

#define GPUCC_CX_GDSCR  0x106c
#define GPUCC_GX_GDSCR  0x100c
#define GPUCC_AHB_CBCR  0x1078

static bool gx_on, gmu_running, gmu_started_hfi, oob_gpu_set, secure = true, zap_ok;
static bool cp_stall, sqe_running, me_inited;
static uint32_t rptr_m;

static bool
cx_on(void)
{
	return !(*reg32(R_GPUCC, GPUCC_CX_GDSCR) & 1) && (*reg32(R_GPUCC, GPUCC_AHB_CBCR) & 1);
}

/* ------------------------------------------------------------------ */
/* DMA memory (host os layer owns allocation; model translates PAs)   */
/* ------------------------------------------------------------------ */

struct hmem { uint64_t pa; size_t size; uint8_t *va; };
static struct hmem hmem[8192];
static int nhmem;
static uint64_t next_pa = 0x900000000ull;

int
sgpu_dma_alloc(size_t size, size_t align, uint32_t flags, struct sgpu_dma *out)
{
	(void)flags;
	memset(out, 0, sizeof(*out));
	size = (size + 0x3fff) & ~(size_t)0x3fff;
	if (align < 0x4000) {
		align = 0x4000;
	}
	void *va = NULL;
	if (posix_memalign(&va, 0x4000, size) || nhmem == 8192) {
		return 12;
	}
	memset(va, 0, size);
	next_pa = (next_pa + align - 1) & ~(uint64_t)(align - 1);
	hmem[nhmem++] = (struct hmem){ next_pa, size, va };
	out->va = va;
	out->pa = next_pa;
	out->size = size;
	next_pa += size + 0x4000;       /* guard gap: catches off-by-one walks */
	return 0;
}

void
sgpu_dma_free(struct sgpu_dma *dma)
{
	for (int i = 0; i < nhmem; i++) {
		if (hmem[i].va == dma->va) {
			free(hmem[i].va);
			hmem[i] = hmem[--nhmem];
			break;
		}
	}
	memset(dma, 0, sizeof(*dma));
}

void *
host_pa_to_va(uint64_t pa, size_t len)
{
	for (int i = 0; i < nhmem; i++) {
		if (pa >= hmem[i].pa && pa + len <= hmem[i].pa + hmem[i].size) {
			return hmem[i].va + (pa - hmem[i].pa);
		}
	}
	for (int i = 0; i < R_COUNT; i++) {
		if (pa >= regions[i].pa && pa + len <= regions[i].pa + regions[i].size) {
			return regions[i].mem + (pa - regions[i].pa);
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* SMMU                                                               */
/* ------------------------------------------------------------------ */

#define NUMSMR 24
#define NUMCB  8
static inline uint32_t *smmu_gr0(uint32_t o) { return reg32(R_SMMU, o); }
static inline uint32_t *smmu_gr1(uint32_t o) { return reg32(R_SMMU, 0x1000 + o); }
static inline uint32_t *smmu_cb(int cb, uint32_t o) { return reg32(R_SMMU, 0x8000 + 0x1000 * cb + o); }

static void
smmu_fault(int cb, uint64_t iova, bool write, uint32_t fsr_bit, uint32_t sid, const char *why)
{
	fprintf(stderr, "model: SMMU CB%d fault %s at %#llx (%s, SID %#x)\n", cb, why,
	    (unsigned long long)iova, write ? "write" : "read", sid);
	*smmu_cb(cb, 0x58) |= fsr_bit;
	*smmu_cb(cb, 0x60) = (uint32_t)iova;
	*smmu_cb(cb, 0x64) = (uint32_t)(iova >> 32);
	*smmu_cb(cb, 0x68) = write ? 0x10 : 0;
	*smmu_gr1(0x400 + 4 * cb) = sid;
}

/* returns host pointer to the byte at iova, NULL on fault (fault recorded) */
static void *
smmu_xlate(uint32_t sid, uint64_t iova, bool write, bool priv, bool quiet)
{
	uint32_t scr0 = *smmu_gr0(0);
	if (scr0 & 1) {
		if (!quiet) {
			merr("SMMU in global bypass (CLIENTPD) while SID %#x accesses %#llx", sid,
			    (unsigned long long)iova);
		}
		return NULL;
	}
	int cb = -1;
	for (int i = 0; i < NUMSMR; i++) {
		uint32_t smr = *smmu_gr0(0x800 + 4 * i), s2cr = *smmu_gr0(0xc00 + 4 * i);
		if (!(smr & (1u << 31))) {
			continue;
		}
		uint32_t id = smr & 0xffff, mask = (smr >> 16) & 0x7fff;
		if (((sid ^ id) & ~mask) == 0) {
			if (((s2cr >> 16) & 3) != 0) {
				merr("SID %#x matched SME%d but S2CR %#x is not TRANS", sid, i, s2cr);
				return NULL;
			}
			cb = (int)(s2cr & 0xff);
			break;
		}
	}
	if (cb < 0) {
		merr("SID %#x: no stream match (unidentified stream) for %#llx", sid,
		    (unsigned long long)iova);
		return NULL;
	}
	uint32_t sctlr = *smmu_cb(cb, 0), tcr = *smmu_cb(cb, 0x30);
	if (!(sctlr & 1)) {
		merr("SID %#x -> CB%d which is disabled", sid, cb);
		return NULL;
	}
	if ((tcr & 0x3f) != 25 || (tcr & 0x80)) {
		merr("CB%d TCR %#x: expected T0SZ 25 with TTBR0 walks enabled", cb, tcr);
		return NULL;
	}
	if (iova >> 39) {
		smmu_fault(cb, iova, write, 0x80, sid, "address size");
		return NULL;
	}
	uint64_t ttbr = (uint64_t)*smmu_cb(cb, 0x20) | (uint64_t)*smmu_cb(cb, 0x24) << 32;
	uint64_t table = ttbr & 0x0000fffffffff000ull;
	for (int level = 1; level <= 3; level++) {
		uint32_t shift = level == 1 ? 30 : level == 2 ? 21 : 12;
		uint64_t *t = host_pa_to_va(table, 4096);
		if (t == NULL) {
			merr("CB%d walk: table PA %#llx is not memory", cb, (unsigned long long)table);
			return NULL;
		}
		uint64_t e = t[(iova >> shift) & 0x1ff];
		if ((e & 3) != 3) {
			if (!quiet) {
				smmu_fault(cb, iova, write, 0x2, sid, "translation");
			}
			return NULL;
		}
		if (level < 3) {
			table = e & 0x0000fffffffff000ull;
			continue;
		}
		if (!(e & (1ull << 10))) {
			smmu_fault(cb, iova, write, 0x4, sid, "access flag");
			return NULL;
		}
		if (!priv && !(e & 0x40)) {
			smmu_fault(cb, iova, write, 0x8, sid, "permission (privileged page)");
			return NULL;
		}
		if (write && (e & 0x80)) {
			smmu_fault(cb, iova, write, 0x8, sid, "permission (read-only page)");
			return NULL;
		}
		uint8_t *page = host_pa_to_va(e & 0x0000fffffffff000ull, 4096);
		if (page == NULL) {
			merr("CB%d: PTE for %#llx points at %#llx, not memory", cb,
			    (unsigned long long)iova, (unsigned long long)(e & 0x0000fffffffff000ull));
			return NULL;
		}
		return page + (iova & 0xfff);
	}
	return NULL;
}

static uint32_t *
gpu_mem32(uint64_t iova, bool write, bool priv)
{
	if (iova & 3) {
		merr("unaligned GPU access %#llx", (unsigned long long)iova);
		return NULL;
	}
	return smmu_xlate(0x000, iova, write, priv, false);
}

static uint32_t *
gmu_mem32(uint32_t iova)
{
	return smmu_xlate(0x005, iova, true, true, false);
}

/* ------------------------------------------------------------------ */
/* cmd-db / firmware synthesis                                        */
/* ------------------------------------------------------------------ */

uint8_t *fw_sqe, *fw_gmu, *fw_zap;
uint32_t fw_sqe_len, fw_gmu_len, fw_zap_len;

static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void
put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void
build_cmddb(void)
{
	static const uint16_t lv[] = { 0, 16, 48, 64, 128, 144, 192, 224, 256, 320, 384, 416 };
	static const char *names[] = { "gfx.lvl", "cx.lvl", "mx.lvl" };
	uint8_t *db = regions[R_CMDDB].mem;
	put32(db, 1);
	put32(db + 4, 0x0c0330db);
	/* rsc_hdr[0]: ARC, header_offset 0, data_offset 3*24, cnt 3 */
	put16(db + 8, 3);
	put16(db + 10, 0);
	put16(db + 12, 3 * 24);
	put16(db + 14, 3);
	/* rsc_hdr[1]: VRM with one unrelated entry */
	put16(db + 24, 4);
	put16(db + 26, 0x200);
	put16(db + 28, 0x300);
	put16(db + 30, 1);
	memcpy(db + 144 + 0x200, "ldoa1\0\0\0", 8);
	for (int i = 0; i < 3; i++) {
		uint8_t *e = db + 144 + 24 * i;
		memset(e, 0, 24);
		memcpy(e, names[i], strlen(names[i]));
		put32(e + 16, 0x30000 + 0x10 * (uint32_t)i);
		put16(e + 20, sizeof(lv));
		put16(e + 22, (uint16_t)(i * sizeof(lv)));
		for (unsigned k = 0; k < sizeof(lv) / 2; k++) {
			put16(db + 144 + 3 * 24 + i * sizeof(lv) + 2 * k, lv[k]);
		}
	}
}

static uint8_t gmu_icache_block[0x100], gmu_itcm0[0xc0];

static void
build_firmware(void)
{
	/* SQE: header dword + version dword (0x0950 -> 0x095 >= 0.95) + body */
	fw_sqe_len = 0x1000;
	fw_sqe = calloc(1, fw_sqe_len);
	put32(fw_sqe, 0x00001234);
	put32(fw_sqe + 4, 0x016b0095);
	for (uint32_t i = 8; i < fw_sqe_len; i += 4) {
		put32(fw_sqe + i, 0xa6500000 | i);
	}

	/* GMU: same block structure as linux-firmware a650_gmu.bin v2.1.8 */
	uint8_t *b = calloc(1, 0x2000), *p = b;
	struct { uint32_t addr, size, type, value; } desc[] = {
		{ 0x00000000, 0, 1, 0x200 }, { 0x00004000, 0, 1, 0x8000 },
		{ 0x10004000, 0, 1, 0x3fe0 }, { 0x60000000, 0, 7, 0x1000 },
	};
	for (unsigned i = 0; i < 4; i++) {
		put32(p, desc[i].addr); put32(p + 4, 0); put32(p + 8, desc[i].type);
		put32(p + 12, desc[i].value); p += 16;
	}
	for (unsigned i = 0; i < sizeof(gmu_itcm0); i++) gmu_itcm0[i] = (uint8_t)(i * 7 + 1);
	for (unsigned i = 0; i < sizeof(gmu_icache_block); i++) gmu_icache_block[i] = (uint8_t)(0x5a ^ i);
	put32(p, 0); put32(p + 4, sizeof(gmu_itcm0)); put32(p + 8, 0xdeadbeef); put32(p + 12, 0xdeadbeef);
	memcpy(p + 16, gmu_itcm0, sizeof(gmu_itcm0)); p += 16 + sizeof(gmu_itcm0);
	put32(p, 0x4000); put32(p + 4, sizeof(gmu_icache_block)); put32(p + 8, 0xdeadbeef); put32(p + 12, 0xdeadbeef);
	memcpy(p + 16, gmu_icache_block, sizeof(gmu_icache_block)); p += 16 + sizeof(gmu_icache_block);
	/* a block straddling two 1 MiB icache chunks (0x103ff0..0x104010) */
	put32(p, 0x103ff0); put32(p + 4, 0x20); put32(p + 8, 0xdeadbeef); put32(p + 12, 0xdeadbeef);
	for (int i = 0; i < 0x20; i++) p[16 + i] = (uint8_t)(0xa0 + i);
	p += 16 + 0x20;
	put32(p, 0x10007fdc); put32(p + 4, 0x18); put32(p + 8, 0xdeadbeef); put32(p + 12, 0xdeadbeef);
	put32(p + 16, 0xffffffff); put32(p + 20, 0x20010008); put32(p + 24, 0xffffffff);
	put32(p + 28, 0x20010008); put32(p + 32, 0); put32(p + 36, 0x20000001);
	p += 16 + 0x18;
	fw_gmu = b;
	fw_gmu_len = (uint32_t)(p - b);

	/* zap: ELF32 with phdr0 (headers), hash segment, one LOAD at 0x8681a000 */
	fw_zap_len = 0x600;
	fw_zap = calloc(1, fw_zap_len);
	uint8_t *z = fw_zap;
	put32(z, 0x464c457f); z[4] = 1; z[5] = 1; z[6] = 1;
	put16(z + 0x10, 2); put16(z + 0x12, 0xa4); put32(z + 0x14, 1);
	put32(z + 0x1c, 0x34);                          /* e_phoff */
	put16(z + 0x28, 0x34); put16(z + 0x2a, 32); put16(z + 0x2c, 3);
	uint8_t *ph = z + 0x34;
	/* phdr0: NULL, offset 0, filesz = headers (0x34 + 3*32 = 0x94) */
	put32(ph + 0, 0); put32(ph + 4, 0); put32(ph + 16, 0x94); put32(ph + 24, 0x07000000);
	/* phdr1: hash segment, NULL type, MDT_TYPE_HASH */
	ph += 32;
	put32(ph + 0, 0); put32(ph + 4, 0x100); put32(ph + 12, 0x8681c000);
	put32(ph + 16, 0x40); put32(ph + 20, 0x1000); put32(ph + 24, 0x02200000);
	/* phdr2: LOAD */
	ph += 32;
	put32(ph + 0, 1); put32(ph + 4, 0x200); put32(ph + 8, 0x8681a000); put32(ph + 12, 0x8681a000);
	put32(ph + 16, 0x400); put32(ph + 20, 0x1000); put32(ph + 24, 0x5);
	for (int i = 0; i < 0x40; i++) z[0x100 + i] = 0x11;
	for (int i = 0; i < 0x400; i++) z[0x200 + i] = (uint8_t)(i ^ 0x3c);
}

/* ------------------------------------------------------------------ */
/* reset                                                              */
/* ------------------------------------------------------------------ */

void
model_reset(const struct model_opts *o)
{
	opt = *o;
	for (int i = 0; i < R_COUNT; i++) {
		if (regions[i].mem == NULL) {
			regions[i].mem = calloc(1, regions[i].size);
		} else {
			memset(regions[i].mem, 0, regions[i].size);
		}
	}
	model_errors = 0;
	model_accesses = 0;
	model_hfi_count = 0;
	model_cp_packets = 0;
	model_cp_skipped = 0;
	gx_on = gmu_running = gmu_started_hfi = oob_gpu_set = zap_ok = false;
	cp_stall = sqe_running = me_inited = false;
	secure = true;
	rptr_m = 0;

	/* clocks: GDSCs collapsed, branches off */
	*reg32(R_GPUCC, GPUCC_CX_GDSCR) = 1;
	*reg32(R_GPUCC, GPUCC_GX_GDSCR) = 1;
	*reg32(R_GCC, 0x00004) = 0x3e;                  /* GPLL0 L = 62 -> 1190 MHz */
	*reg32(R_GPUCC, 0x1120) = 0x80000000;           /* GMU RCG root off */

	/* SMMU IDs: MMU-500 r2p0, 4K pages, 8 pages/half, 8 CBs, 24 SMRs, 15-bit SIDs */
	*smmu_gr0(0x020) = (1u << 30) | (1u << 29) | (1u << 27) | (1u << 14) | (15u << 9) | NUMSMR;
	*smmu_gr0(0x024) = (2u << 28) | NUMCB;
	*smmu_gr0(0x028) = (1u << 12) | (5u << 8) | (1u << 4) | 1u;   /* 4K, UBS 48, OAS/IAS 36 */
	*smmu_gr0(0x03c) = 0x21;
	*smmu_gr0(0x000) = (1u << 10) | (opt.clientpd ? 1u : 0u);    /* USFCFG */
	if (opt.cb0_live) {
		*smmu_cb(0, 0) = 0x1;
	}
	if (opt.fw_sme_sid2) {
		*smmu_gr0(0x800) = (1u << 31) | (0x400u << 16) | 2;    /* SME0: SID 2/0x400 */
		*smmu_gr0(0xc00) = 3;                                   /* TRANS CB3 */
		*smmu_cb(3, 0) = 0x1;
	}
	/* qfprom speed bin: byte 0x19b bits [7:5] */
	*reg32(R_QFPROM, 0x198) = (opt.speed_bin & 7) << 29;
	build_cmddb();
	if (fw_sqe == NULL) {
		build_firmware();
	}
}

void
model_set_stall(bool s)
{
	cp_stall = s;
}

uint32_t model_gpu_reg(uint32_t dw) { return *reg32(R_GPU, dw * 4); }
uint32_t model_gmu_reg(uint32_t off) { return *reg32(R_GMU, off); }
uint32_t model_smmu_reg(uint32_t off) { return *reg32(R_SMMU, off); }
uint32_t model_rscc_reg(uint32_t off) { return *reg32(R_RSCC, off); }
uint32_t model_pdc_reg(uint32_t off) { return *reg32(R_PDC, off); }

/* ------------------------------------------------------------------ */
/* PM4 reference encoders + CP interpreter                            */
/* ------------------------------------------------------------------ */

static uint32_t
parity(uint32_t v)
{
	/* odd parity bit: 1 iff v has an even number of set bits */
	return (uint32_t)((__builtin_popcount(v) & 1) == 0);
}

uint32_t ref_pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | parity(cnt) << 15 | op << 16 | parity(op) << 23;
}

uint32_t ref_pkt4(uint32_t reg, uint32_t cnt)
{
	return 0x40000000u | cnt | parity(cnt) << 7 | reg << 8 | parity(reg) << 27;
}

static void gpu_reg_write(uint32_t dw, uint32_t v);
static uint32_t gpu_reg_read(uint32_t dw);

/* CP_PROTECT as programmed by the driver: entries 0..47, each
 * BASE[17:0] LEN[30:18] (protects len+1 regs) READ[31]; the last entry
 * (47) spans to the end when CP_PROTECT_CNTL.LAST_SPAN_INF_RANGE (bit 3). */
static bool
cp_protected(uint32_t reg, bool write)
{
	uint32_t cntl = *reg32(R_GPU, 0x84f * 4);
	if (!(cntl & 1)) {
		return false;
	}
	for (uint32_t i = 0; i < 48; i++) {
		uint32_t v = *reg32(R_GPU, (0x850 + i) * 4);
		if (v == 0) {
			continue;
		}
		uint32_t base = v & 0x3ffff, len = (v >> 18) & 0x1fff;
		bool rd = (v >> 31) & 1;
		bool inf = i == 47 && (cntl & 8);
		if (reg >= base && (inf || reg <= base + len) && (write || rd)) {
			return true;
		}
	}
	return false;
}

struct stream {
	uint64_t base;          /* ring: base iova; IB: start iova */
	uint32_t size;          /* dwords */
	uint32_t pos;
	bool     ring, priv;
	int      level;
};

static bool
fetch(struct stream *s, uint32_t *out)
{
	if (!s->ring && s->pos >= s->size) {
		merr("CP: packet runs past the end of an IB");
		return false;
	}
	uint32_t *p = gpu_mem32(s->base + 4ull * (s->pos % s->size), false, s->priv);
	if (p == NULL) {
		merr("CP: fetch fault at %#llx (%s)", (unsigned long long)(s->base + 4ull * s->pos),
		    s->ring ? "ring" : "IB");
		return false;
	}
	*out = *p;
	s->pos = s->ring ? (s->pos + 1) % s->size : s->pos + 1;
	return true;
}

static void
gpu_write_mem(uint64_t iova, uint32_t v, bool priv)
{
	if (secure && !priv) {
		merr("CP: write to %#llx while still in secure mode", (unsigned long long)iova);
		return;
	}
	uint32_t *p = gpu_mem32(iova, true, priv);
	if (p) {
		*p = v;
	}
}

static bool run_stream(struct stream *s, uint32_t end_pos);

static bool
exec_packet(struct stream *s)
{
	uint32_t hdr, d[64];
	if (!fetch(s, &hdr)) {
		return false;
	}
	model_cp_packets++;
	uint32_t type = hdr >> 28;
	/* memory accesses by packets: privileged only from the ring with APRIV RBPRIVLEVEL */
	bool mpriv = s->ring && (gpu_reg_read(0x844) & 4);
	if (type == 4) {
		uint32_t cnt = hdr & 0x7f, reg = (hdr >> 8) & 0x3ffff;
		if (hdr != ref_pkt4(reg, cnt)) {
			merr("CP: bad PKT4 header %#x (want %#x)", hdr, ref_pkt4(reg, cnt));
			return false;
		}
		for (uint32_t i = 0; i < cnt; i++) {
			uint32_t v;
			if (!fetch(s, &v)) return false;
			if (!s->ring && cp_protected(reg + i, true)) {
				merr("CP: IB wrote protected register %#x = %#x (CP_PROTECT fault)",
				    reg + i, v);
				continue;
			}
			gpu_reg_write(reg + i, v);
		}
		return true;
	}
	if (type != 7) {
		merr("CP: unknown packet type, header %#x at %u", hdr, s->pos - 1);
		return false;
	}
	uint32_t cnt = hdr & 0x3fff, op = (hdr >> 16) & 0x7f;
	if (hdr != ref_pkt7(op, cnt)) {
		merr("CP: bad PKT7 header %#x (want %#x)", hdr, ref_pkt7(op, cnt));
		return false;
	}
	if (cnt > 64) {
		if (opt.permissive_pm4 && !s->ring) {
			for (uint32_t i = 0; i < cnt; i++) {    /* e.g. big CP_LOAD_STATE6 */
				uint32_t dummy;
				if (!fetch(s, &dummy)) return false;
			}
			model_cp_skipped++;
			return true;
		}
		merr("CP: oversized packet op %#x cnt %u", op, cnt);
		return false;
	}
	for (uint32_t i = 0; i < cnt; i++) {
		if (!fetch(s, &d[i])) return false;
	}
	uint64_t a;
	switch (op) {
	case 0x10:      /* NOP */
		break;
	case 0x48:      /* ME_INIT */
		if (!s->ring || cnt != 8 || d[0] != 0x2f || d[1] != 3 || d[2] != 0x20000000) {
			merr("CP_ME_INIT malformed");
		}
		me_inited = true;
		break;
	case 0x66:      /* SET_SECURE_MODE */
		if (!s->ring) {
			merr("CP_SET_SECURE_MODE from an IB");
		} else if (d[0] == 0) {
			if (!zap_ok) {
				merr("CP_SET_SECURE_MODE 0 without an authenticated zap shader");
			}
			secure = false;
		}
		break;
	case 0x3d:      /* MEM_WRITE */
		a = d[0] | (uint64_t)d[1] << 32;
		for (uint32_t i = 2; i < cnt; i++) {
			gpu_write_mem(a + 4 * (i - 2), d[i], mpriv);
		}
		break;
	case 0x46:      /* EVENT_WRITE */
		if (!me_inited) {
			merr("CP: event before CP_ME_INIT");
		}
		if (cnt == 4 && ((d[0] & 0xff) == 4 || (d[0] & 0xff) == 0x16 ||
		    (d[0] & (1u << 30)))) {     /* CACHE_FLUSH_TS, RB_DONE_TS, TIMESTAMP */
			a = d[1] | (uint64_t)d[2] << 32;
			gpu_write_mem(a, d[3], mpriv);
			if (d[0] & (1u << 31)) {
				*reg32(R_GPU, 0x201 * 4) |= 1u << 20;
			}
		} else if (cnt != 1 && !opt.permissive_pm4) {
			merr("CP_EVENT_WRITE cnt %u event %#x unexpected", cnt, d[0]);
		}
		break;
	case 0x3e: {    /* REG_TO_MEM */
		uint32_t reg = d[0] & 0x3ffff, n = (d[0] >> 18) & 0xfff;
		a = d[1] | (uint64_t)d[2] << 32;
		for (uint32_t i = 0; i < n; i++) {
			if (!s->ring && cp_protected(reg + i, false)) {
				merr("CP: IB read protected register %#x", reg + i);
			}
			gpu_write_mem(a + 4 * i, gpu_reg_read(reg + i), mpriv);
		}
		break;
	}
	case 0x3f: {    /* INDIRECT_BUFFER */
		if (cnt != 3) {
			merr("CP_INDIRECT_BUFFER cnt %u", cnt);
			break;
		}
		if (s->level >= 2) {
			merr("CP: IB nesting deeper than IB2");
			break;
		}
		struct stream ib = { d[0] | (uint64_t)d[1] << 32, d[2] & 0xfffff, 0, false, false,
			s->level + 1 };
		if (secure) {
			merr("CP: user IB executed while the GPU is still in secure mode");
		}
		if (!run_stream(&ib, ib.size)) {
			return false;
		}
		break;
	}
	default:
		if (opt.permissive_pm4 && !s->ring) {
			model_cp_skipped++;
		} else {
			merr("CP: unhandled opcode %#x", op);
		}
	}
	return true;
}

static bool
run_stream(struct stream *s, uint32_t end_pos)
{
	while (s->pos != end_pos) {
		if (!exec_packet(s)) {
			return false;
		}
	}
	return true;
}

static void
cp_run(void)
{
	if (cp_stall || !sqe_running) {
		return;
	}
	uint64_t base = (uint64_t)gpu_reg_read(0x800) | (uint64_t)gpu_reg_read(0x801) << 32;
	uint32_t cntl = gpu_reg_read(0x802);
	if (cntl != 0x20c) {
		merr("CP_RB_CNTL %#x", cntl);
	}
	uint32_t size = 1u << ((cntl & 0x3f) + 1);     /* BUFSZ = log2(dwords/2)... 12 -> 8192 */
	uint32_t wptr = gpu_reg_read(0x807);
	if (wptr >= size) {
		merr("CP_RB_WPTR %u beyond ring", wptr);
		return;
	}
	struct stream s = { base, size, rptr_m, true, (gpu_reg_read(0x844) & 2) != 0, 0 };
	if (!s.priv) {
		merr("ring fetched unprivileged (CP_APRIV_CNTL %#x)", gpu_reg_read(0x844));
	}
	if (!run_stream(&s, wptr)) {
		merr("CP stopped at %u", s.pos);
		sqe_running = false;
		return;
	}
	rptr_m = wptr;
	model_now_us += 2;      /* the GPU takes time too */
	uint64_t ra = (uint64_t)gpu_reg_read(0x804) | (uint64_t)gpu_reg_read(0x805) << 32;
	uint32_t *sh = gpu_mem32(ra, true, (gpu_reg_read(0x844) & 8) != 0);
	if (sh) {
		*sh = rptr_m;
	}
}

static uint32_t
gpu_reg_read(uint32_t dw)
{
	uint64_t ticks = model_now_us * 192 / 10;
	switch (dw) {
	case 0x980: return (uint32_t)ticks;
	case 0x981: return (uint32_t)(ticks >> 32);
	case 0x400: return (uint32_t)(ticks * 30);
	case 0x401: return (uint32_t)((ticks * 30) >> 32);
	case 0x210: return 0;                           /* RBBM_STATUS idle */
	case 0x806: return rptr_m;
	case 0x017: return *reg32(R_GPU, 0x16 * 4);     /* RBBM_GBIF_HALT_ACK */
	case 0x3c46: return *reg32(R_GPU, 0x3c45 * 4);  /* GBIF_HALT_ACK */
	}
	return *reg32(R_GPU, dw * 4);
}

static void
gpu_reg_write(uint32_t dw, uint32_t v)
{
	if (dw * 4 >= regions[R_GPU].size) {
		merr("GPU register %#x out of range", dw);
		return;
	}
	switch (dw) {
	case 0x037:     /* RBBM_INT_CLEAR_CMD */
		*reg32(R_GPU, 0x201 * 4) &= ~v;
		return;
	case 0x808:     /* CP_SQE_CNTL */
		if (v == 1) {
			uint64_t sqe = (uint64_t)gpu_reg_read(0x830) | (uint64_t)gpu_reg_read(0x831) << 32;
			uint32_t *ucode = gpu_mem32(sqe, false, false);    /* ICACHE fetch unprivileged */
			if (ucode == NULL || *ucode != 0x016b0095) {
				merr("SQE microcode not found at %#llx (first dword %#x)",
				    (unsigned long long)sqe, ucode ? *ucode : 0);
			}
			sqe_running = true;
			me_inited = false;
			rptr_m = 0;
		} else if (v == 3) {
			sqe_running = false;    /* halt also clears a wedged CP */
			cp_stall = false;
		}
		break;
	case 0x807:     /* CP_RB_WPTR */
		*reg32(R_GPU, dw * 4) = v;
		cp_run();
		return;
	case 0xf400:    /* RBBM_SECVID_TRUST_CNTL */
		if (opt.tz_protects_secvid) {
			merr("XPU violation: CPU wrote RBBM_SECVID_TRUST_CNTL");
			return;
		}
		if (v == 0) {
			secure = false;
		}
		break;
	}
	*reg32(R_GPU, dw * 4) = v;
}

/* ------------------------------------------------------------------ */
/* GMU / HFI peer                                                     */
/* ------------------------------------------------------------------ */

static uint32_t hfi_levels;

static void
hfi_ack(uint32_t *qtbl, uint32_t cmd_hdr)
{
	uint32_t *h = qtbl + 6 + 12;
	uint32_t *ring = gmu_mem32(h[1]);
	if (ring == NULL) {
		return;
	}
	uint32_t wr = h[11];
	uint32_t msg[4] = { ((cmd_hdr >> 20) << 20) | (4u << 8) | 126, cmd_hdr, 0, 0 };
	for (int i = 0; i < 4; i++) {
		ring[wr] = msg[i];
		wr = (wr + 1) % 1024;
	}
	h[11] = wr;
	*reg32(R_GMU, 0x14648) |= 1;
}

static void
hfi_process(void)
{
	uint32_t *qtbl = gmu_mem32(*reg32(R_GMU, 0x14614));
	if (qtbl == NULL || !gmu_running) {
		merr("HFI doorbell without a running GMU / queue table");
		return;
	}
	if (qtbl[1] != 120 || qtbl[2] != 6 || qtbl[3] != 12 || qtbl[4] != 2 || qtbl[5] != 2) {
		merr("HFI queue table header %u %u %u %u %u", qtbl[1], qtbl[2], qtbl[3], qtbl[4], qtbl[5]);
	}
	uint32_t *h = qtbl + 6;
	if (h[2] != 0xa00 || qtbl[6 + 12 + 2] != 0xa01 || h[3] != 1024) {
		merr("HFI queue headers type %#x/%#x size %u", h[2], qtbl[6 + 12 + 2], h[3]);
	}
	uint32_t *ring = gmu_mem32(h[1]);
	if (ring == NULL) {
		return;
	}
	while (h[10] != h[11]) {
		uint32_t rd = h[10];
		if (rd & 3) {
			merr("HFI: message not 4-dword aligned (%u)", rd);
		}
		uint32_t hdr = ring[rd], n = (hdr >> 8) & 0xff, id = hdr & 0xff;
		uint32_t m[160];
		for (uint32_t i = 0; i < n && i < 160; i++) {
			m[i] = ring[(rd + i) % 1024];
		}
		uint32_t next = rd + n;
		while (next & 3) {
			if (ring[next % 1024] != 0xfafafafa) {
				merr("HFI: pad dword %#x", ring[next % 1024]);
			}
			next++;
		}
		h[10] = next % 1024;
		if (model_hfi_count < 64) {
			model_hfi_log[model_hfi_count++] = id;
		}
		switch (id) {
		case 4:
			if (n != 59 || m[1] < 2 || m[1] > 16 || m[2] != 2) {
				merr("PERF_TABLE size %u levels %u/%u", n, m[1], m[2]);
				break;
			}
			hfi_levels = m[1];
			for (uint32_t i = 1; i < m[1]; i++) {
				if (m[3 + 3 * i] == 0 || m[5 + 3 * i] <= m[5 + 3 * (i - 1)] ||
				    m[4 + 3 * i] != 0xffffffff) {
					merr("PERF_TABLE level %u: vote %#x freq %u", i, m[3 + 3 * i], m[5 + 3 * i]);
				}
			}
			if (m[51 + 2] == 0 || m[52 + 2] != 200000) {
				merr("PERF_TABLE GMU level: vote %#x freq %u", m[53], m[54]);
			}
			break;
		case 3:
			if (n != 160 || m[1] != 1 || m[3] != 3 || m[24] != 0x50000 || m[6] != 0x500a4) {
				merr("BW_TABLE malformed");
			}
			break;
		case 14:
			if (n != 2) merr("CORE_FW_START size %u", n);
			break;
		case 10:
			if (n != 1) merr("START size %u", n);
			gmu_started_hfi = true;
			break;
		case 30:
			if (n != 4 || !gmu_started_hfi || m[1] != 1 || m[2] == 0 || m[2] >= hfi_levels) {
				merr("GX_BW_PERF_VOTE size %u started %d ack %u level %u/%u", n,
				    gmu_started_hfi, m[1], m[2], hfi_levels);
			}
			break;
		case 33:
			gx_on = false;
			gmu_started_hfi = false;
			break;
		default:
			merr("HFI: unknown message id %u", id);
		}
		hfi_ack(qtbl, hdr);
	}
}

static void
gmu_check_boot(void)
{
	/* things that must be in place when the M3 is released from reset */
	if (*reg32(R_RSCC, 0x600) != 0xeaaae5a0 || *reg32(R_RSCC, 0x610) != 0x0020edad)
		merr("RSC sequencer not programmed");
	if (*reg32(R_PDC, 0x4500) != 0x80000001)
		merr("PDC not enabled (%#x)", *reg32(R_PDC, 0x4500));
	if (*reg32(R_GMU, 0x14608) != 0x06050200)
		merr("GMU chip id %#x", *reg32(R_GMU, 0x14608));
	if (*reg32(R_GMU, 0x14004) != 2 || *reg32(R_GMU, 0x14610) != 1)
		merr("GMU boot config/qtbl info");
	if (*reg32(R_GMU, 0x24c44) != 0x802800a0)
		merr("AHB fence range %#x", *reg32(R_GMU, 0x24c44));
	if (*reg32(R_GMU, 0x140b4) != 0x4052)
		merr("CM3_CFG %#x", *reg32(R_GMU, 0x140b4));
	if ((*reg32(R_GMU, 0x143a0) & 0xf11) != 0xf11)
		merr("RPMH_CTRL %#x", *reg32(R_GMU, 0x143a0));
	if (memcmp(regions[R_GMU].mem + 0x3000, gmu_itcm0, sizeof(gmu_itcm0)))
		merr("ITCM does not hold the firmware");
	uint8_t *ic = (uint8_t *)gmu_mem32(0x4000);
	if (ic == NULL || memcmp(ic, gmu_icache_block, sizeof(gmu_icache_block)))
		merr("GMU icache block not visible at GMU VA 0x4000");
	for (int i = 0; i < 0x20; i++) {
		uint8_t *q = (uint8_t *)smmu_xlate(5, 0x103ff0 + (uint32_t)i, false, true, false);
		if (q == NULL || *q != 0xa0 + i) {
			merr("GMU block across icache chunks wrong at +%#x", i);
			break;
		}
	}
	uint32_t logm = *reg32(R_GMU, 0x14400);
	if ((logm & 0xfff) != 3 || gmu_mem32(logm & ~0xfffu) == NULL)
		merr("GMU log buffer %#x", logm);
	if (*reg32(R_GMU, 0x14000) != 0) { /* released */ }
}

static void
gmu_write(uint32_t off, uint32_t v)
{
	uint32_t *r = reg32(R_GMU, off);
	switch (off) {
	case 0x14000:   /* CM3_SYSRESET */
		if (v == 0 && *r == 1) {
			gmu_check_boot();
			*reg32(R_GMU, 0x14070) = 0x100;          /* INIT_RESULT */
			gmu_running = true;
		} else if (v == 1) {
			gmu_running = false;
		}
		break;
	case 0x14618:   /* HFI_CTRL_INIT */
		if (v & 1) {
			if (!gmu_running) merr("HFI init before the GMU runs");
			*reg32(R_GMU, 0x14600) = 1;
		}
		break;
	case 0x14644:   /* GMU2HOST_INTR_CLR */
		*reg32(R_GMU, 0x14648) &= ~v;
		return;
	case 0x14650:   /* HOST2GMU_INTR_SET */
		if (v & 1) {
			hfi_process();
		}
		if (v & (1u << 30)) {
			if (!gmu_started_hfi) {
				merr("OOB GPU_SET before HFI START");
			} else {
				oob_gpu_set = true;
				gx_on = true;
				*reg32(R_GMU, 0x14648) |= 1u << 31;
			}
		}
		if (v & (1u << 28)) {
			*reg32(R_GMU, 0x14648) |= 1u << 30;
		}
		if (v & (1u << 31)) {
			oob_gpu_set = false;
		}
		return;
	case 0x24c1c:   /* RSCC_CONTROL_REQ */
		if (v & 2) *reg32(R_GMU, 0x24c20) |= 2;
		if (v & 1) *reg32(R_RSCC, 0x10) |= 1u << 16;
		if (v == 0) { /* release */ }
		break;
	}
	*r = v;
}

static uint32_t
gmu_read(uint32_t off)
{
	switch (off) {
	case 0x14340:   /* SPTPRAC_PWR_CLK_STATUS */
		return gx_on ? 0 : 0xc0;
	case 0x24c30:   /* CX_BUSY_STATUS */
		return 0;
	case 0x143b0:   /* RPMH_POWER_STATE */
		return 0;
	}
	return *reg32(R_GMU, off);
}

/* ------------------------------------------------------------------ */
/* register access entry points                                       */
/* ------------------------------------------------------------------ */

static uint32_t
clk_read(int r, uint32_t off)
{
	uint32_t v = *reg32(r, off);
	if (r == R_GCC) {
		if (off == 0x0) return v | (1u << 30);  /* GPLL0 active */
		if (off == 0x71004 || off == 0x7100c || off == 0x71018 || off == 0x71154)
			return (v & 1) ? v & ~(1u << 31) : v | (1u << 31);
	} else {
		if (off == 0x1078 || off == 0x1098 || off == 0x109c)
			return (v & 1) ? v & ~(1u << 31) : v | (1u << 31);
		if (off == 0x1120) return v & ~1u;      /* RCG update done */
		if (off == 0x1540) return (*reg32(R_GPUCC, GPUCC_CX_GDSCR) & 1) ? 0 : 1u << 31;
		if (off == 0x1010) return (*reg32(R_GPUCC, GPUCC_GX_GDSCR) & 1) ? 1u << 15 : 0;
	}
	return v;
}

static bool
power_ok(int r, uint32_t off, bool write)
{
	(void)off;
	if ((r == R_GMU || r == R_SMMU || r == R_RSCC) && !cx_on()) {
		merr("%s %s at +%#x with CX off (bus hang on hardware)", write ? "write" : "read",
		    regions[r].name, off);
		return false;
	}
	if (r == R_GPU && !gx_on) {
		merr("GPU register %s +%#x with GX off", write ? "write" : "read", off);
		return false;
	}
	return true;
}

uint32_t
sgpu_host_rd(volatile uint8_t *base, uint32_t off)
{
	uint32_t o;
	int r = find_region(base + off, &o);
	model_accesses++;
	if (r < 0) {
		merr("read from unmapped pointer");
		return 0xffffffff;
	}
	if (o & 3) {
		merr("unaligned read %s+%#x", regions[r].name, o);
	}
	if (!power_ok(r, o, false)) {
		return 0xffffffff;
	}
	switch (r) {
	case R_GCC:
	case R_GPUCC:
		return clk_read(r, o);
	case R_GMU:
		return gmu_read(o);
	case R_GPU:
		return gpu_reg_read(o / 4);
	case R_RSCC:
		if (o == 0x404) return 0;
		if (o == 0xd18 || o == 0xfb8 || o == 0x1258 || o == 0x14f8) return 1;
		break;
	case R_SMMU:
		if (o >= 0x8000 && (o & 0xfff) == 0x7f4) return 0;      /* TLBSTATUS */
		if (o == 0x074) return 0;
		break;
	}
	return *reg32(r, o);
}

void
sgpu_host_wr(volatile uint8_t *base, uint32_t off, uint32_t v)
{
	uint32_t o;
	int r = find_region(base + off, &o);
	model_accesses++;
	if (r < 0) {
		merr("write to unmapped pointer");
		return;
	}
	if (!power_ok(r, o, true)) {
		return;
	}
	switch (r) {
	case R_GMU:
		gmu_write(o, v);
		return;
	case R_GPU:
		gpu_reg_write(o / 4, v);
		return;
	case R_SMMU:
		if (o >= 0x20 && o <= 0x3c) {
			return;                         /* ID registers are read-only */
		}
		if (o == 0x048) {                       /* sGFSR w1c */
			*reg32(r, o) &= ~v;
			return;
		}
		if (o >= 0x8000 && (o & 0xfff) == 0x58) {
			*reg32(r, o) &= ~v;             /* FSR w1c */
			return;
		}
		if (o >= 0xc00 && o < 0xc00 + 4 * NUMSMR && opt.hv_filters_s2cr) {
			return;
		}
		if (o >= 0x800 && o < 0x800 + 4 * NUMSMR) {
			int i = (int)(o - 0x800) / 4;
			if (opt.fw_sme_sid2 && i == 0) {
				merr("driver modified the firmware-owned SME0");
			}
		}
		if (o >= 0x8000 && o < 0x9000 && opt.cb0_live && (*reg32(r, 0x8000) & 1)) {
			/* driver may only touch a live CB0 when forced */
		}
		break;
	}
	*reg32(r, o) = v;
}

uint64_t
sgpu_host_rd64(volatile uint8_t *base, uint32_t off)
{
	return (uint64_t)sgpu_host_rd(base, off) | (uint64_t)sgpu_host_rd(base, off + 4) << 32;
}

void
sgpu_host_wr64(volatile uint8_t *base, uint32_t off, uint64_t v)
{
	sgpu_host_wr(base, off, (uint32_t)v);
	sgpu_host_wr(base, off + 4, (uint32_t)(v >> 32));
}

void
model_tick(void)
{
}

/* ------------------------------------------------------------------ */
/* TrustZone                                                          */
/* ------------------------------------------------------------------ */

static bool pas_inited;

void
sgpu_smc_call(struct sgpu_smc *a)
{
	uint32_t fn = (uint32_t)a->x[0];
	uint64_t r0 = 0, r1 = 0;

	switch (fn & 0x3fffffff) {
	case 0x02000601:        /* INFO is_call_available */
		r1 = 1;
		break;
	case 0x02000207:        /* PIL is_supported */
		r1 = a->x[2] == 13;
		break;
	case 0x02000201: {      /* PIL init_image(13, meta_pa) */
		const uint8_t *meta = host_pa_to_va(a->x[3], 0x94);
		if (a->x[2] != 13 || a->x[1] != 0x82 || meta == NULL ||
		    meta[0] != 0x7f || meta[1] != 'E') {
			merr("TZ: bad PAS init_image (id %llu arginfo %#llx)",
			    (unsigned long long)a->x[2], (unsigned long long)a->x[1]);
			r0 = (uint64_t)-4;
			break;
		}
		pas_inited = true;
		break;
	}
	case 0x02000202:        /* mem_setup */
		break;
	case 0x02000205:        /* auth_and_reset(13) */
		if (!pas_inited || a->x[2] != 13) {
			merr("TZ: auth_and_reset without init");
			r0 = (uint64_t)-4;
			break;
		}
		if (regions[R_ZAP].mem[0] != 0x3c) {
			merr("TZ: zap segment not copied into the carve-out");
		}
		zap_ok = true;
		break;
	default:
		merr("TZ: unexpected SMC %#x", fn);
		r0 = (uint64_t)-1;
	}
	a->x[0] = r0;
	a->x[1] = r1;
	a->x[2] = 0;
	a->x[3] = 0;
}
