/*
 * sweet_gpu_scm.c - Qualcomm TrustZone (SCM) calls and the GPU zap shader.
 *
 * The Adreno boots in "secure" mode: CP may not run unprotected command
 * streams until a TrustZone-signed microcode blob (the zap shader, PAS id
 * 13) has been authenticated. After that the kernel emits
 * CP_SET_SECURE_MODE 0 on the ring (sweet_gpu_cp.c). Linux:
 * adreno_gpu.c zap_shader_load_mdt() + mdt_loader.c + qcom_scm.c.
 * Derivation: docs/specs/clocks-scm-zap-a650.md, parts B and C.
 *
 * SMCCC, owner SIP, 64-bit convention:
 *   x0 = 0x42000000 | svc << 8 | cmd   (yielding/"STD" call; FAST = 0xC2..)
 *   x1 = arginfo = nargs | type_i << (4 + 2 i)   (0 VAL, 1 RO, 2 RW)
 *   x2.. = args; return x0 = status, x1.. = results
 *   x0 == 1 (INTERRUPTED): reissue with x0 = 1 and x6 from the reply
 *   x0 == 2 (WAITQ_SLEEP): needs an SCM interrupt SM8250 does not have
 *   x0 == -12 (busy): sleep 30 ms, retry up to 22 times
 *
 * The zap image is the phone's own Xiaomi-signed a650_zap (mdt+bNN joined
 * into one ELF32 by tools/mkzap.py) embedded in the ADT; it is copied
 * unchanged into the 8 KiB carve-out pil_gpu_region @ 0x8681a000.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(SGPU_HOST)
#include "sweet_gpu.h"

#define SCM_STD(svc, cmd)       (0x42000000u | ((svc) << 8) | (cmd))
#define SCM_FAST(svc, cmd)      (0xc2000000u | ((svc) << 8) | (cmd))
#define SCM_SVC_BOOT            0x01u
#define SCM_SVC_PIL             0x02u
#define SCM_SVC_INFO            0x06u
#define SCM_BOOT_SET_REMOTE     0x0au
#define SCM_PIL_INIT_IMAGE      0x01u
#define SCM_PIL_MEM_SETUP       0x02u
#define SCM_PIL_AUTH_RESET      0x05u
#define SCM_PIL_IS_SUPPORTED    0x07u
#define SCM_INFO_IS_CALL_AVAIL  0x01u
#define SCM_ARG_VAL             0u
#define SCM_ARG_RW              2u
#define SCM_ARGS(n)             ((uint64_t)(n))
#define GPU_PAS_ID              13u
#define SCM_INTERRUPTED         1
#define SCM_WAITQ_SLEEP         2
#define SCM_EBUSY               (-12)

/* ELF32 */
#define PT_LOAD                 1u
#define MDT_TYPE_MASK           (7u << 24)
#define MDT_TYPE_HASH           (2u << 24)
#define MDT_RELOCATABLE         (1u << 27)

static bool scm_arm64;

static int
scm_errno(int64_t x0)
{
	switch (x0) {
	case 0:   return 0;
	case -1:  return -5;    /* EIO */
	case -2:
	case -3:  return -22;   /* EINVAL */
	case -4:  return -95;   /* EOPNOTSUPP */
	case -5:  return -12;   /* ENOMEM */
	case -12: return -16;   /* EBUSY */
	default:  return -22;
	}
}

/* One SCM call with the retry rules above. res may be NULL. */
static int
scm_call(uint32_t fn, uint64_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t res[3])
{
	struct sgpu_smc s;
	int busy = 0;

	for (;;) {
		for (int i = 0; i < 8; i++) {
			s.x[i] = 0;
		}
		s.x[0] = fn;
		s.x[1] = arginfo;
		s.x[2] = a0;
		s.x[3] = a1;
		s.x[4] = a2;
		sgpu_smc_call(&s);
		while ((int64_t)s.x[0] == SCM_INTERRUPTED) {
			uint64_t x6 = s.x[6];
			s.x[0] = SCM_INTERRUPTED;
			s.x[1] = arginfo;
			s.x[2] = a0;
			s.x[3] = a1;
			s.x[4] = a2;
			s.x[5] = 0;
			s.x[6] = x6;
			s.x[7] = 0;
			sgpu_smc_call(&s);
		}
		if ((int64_t)s.x[0] == SCM_WAITQ_SLEEP) {
			sgpu_log("scm: fn %#x asked to wait on an SCM IRQ (unsupported)", fn);
			return -5;
		}
		if ((int64_t)s.x[0] == SCM_EBUSY && ++busy < 22) {
			sgpu_msleep(30);
			continue;
		}
		break;
	}
	if (res) {
		res[0] = s.x[1];
		res[1] = s.x[2];
		res[2] = s.x[3];
	}
	return scm_errno((int64_t)s.x[0]);
}

static bool
scm_call_available(uint32_t svc, uint32_t cmd)
{
	uint64_t res[3];
	int err = scm_call(SCM_STD(SCM_SVC_INFO, SCM_INFO_IS_CALL_AVAIL), SCM_ARGS(1),
	    0x02000000u | (svc << 8) | cmd, 0, 0, res);
	return err == 0 && res[0] != 0;
}

int
sgpu_scm_probe(struct sgpu *g)
{
	struct sgpu_smc s = { .x = {
		SCM_FAST(SCM_SVC_INFO, SCM_INFO_IS_CALL_AVAIL), SCM_ARGS(1),
		0x02000000u | (SCM_SVC_INFO << 8) | SCM_INFO_IS_CALL_AVAIL } };

	sgpu_smc_call(&s);
	scm_arm64 = s.x[0] == 0 && s.x[1] == 1;
	if (!scm_arm64) {
		return sgpu_fail(g, -95, "SCM: 64-bit SMCCC convention not detected (x0 %#llx x1 %#llx)",
		           (unsigned long long)s.x[0], (unsigned long long)s.x[1]);
	}
	uint64_t res[3] = { 0 };
	bool init = scm_call_available(SCM_SVC_PIL, SCM_PIL_INIT_IMAGE);
	bool auth = scm_call_available(SCM_SVC_PIL, SCM_PIL_AUTH_RESET);
	bool sup = scm_call_available(SCM_SVC_PIL, SCM_PIL_IS_SUPPORTED) &&
	    scm_call(SCM_STD(SCM_SVC_PIL, SCM_PIL_IS_SUPPORTED), SCM_ARGS(1), GPU_PAS_ID,
	    0, 0, res) == 0 && res[0] != 0;
	sgpu_log("scm: arm64 convention, PIL init %d auth %d, GPU PAS %d", init, auth, sup);
	return 0;
}

/* ---- ELF32 helpers (little endian, bounds-checked) ---- */

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

struct phdr {
	uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

static int
elf_phdr(const uint8_t *fw, uint32_t len, uint32_t i, struct phdr *p)
{
	uint32_t phoff = le32(fw + 0x1c);
	uint16_t phentsize = le16(fw + 0x2a);
	uint64_t at = (uint64_t)phoff + (uint64_t)i * phentsize;

	if (phentsize != 32 || at + 32 > len) {
		return -22;
	}
	const uint8_t *q = fw + at;
	p->type = le32(q);
	p->offset = le32(q + 4);
	p->vaddr = le32(q + 8);
	p->paddr = le32(q + 12);
	p->filesz = le32(q + 16);
	p->memsz = le32(q + 20);
	p->flags = le32(q + 24);
	p->align = le32(q + 28);
	return 0;
}

static bool
phdr_loadable(const struct phdr *p)
{
	return p->type == PT_LOAD && (p->flags & MDT_TYPE_MASK) != MDT_TYPE_HASH && p->memsz;
}

/* Copy to Device memory: aligned 32-bit stores only. */
static void
copy_to_io(volatile uint8_t *dst, uint32_t off, const uint8_t *src, uint32_t n, uint32_t memsz)
{
	uint32_t words = (memsz + 3) / 4;
	for (uint32_t w = 0; w < words; w++) {
		uint32_t v = 0;
		for (uint32_t b = 0; b < 4; b++) {
			uint32_t idx = w * 4 + b;
			if (idx < n) {
				v |= (uint32_t)src[idx] << (8 * b);
			}
		}
		sgpu_wr(dst, off + w * 4, v);
	}
	sgpu_wmb();
}

int
sgpu_zap_load(struct sgpu *g)
{
	const uint8_t *fw = g->zap_fw;
	uint32_t len = g->zap_fw_len;
	struct phdr p0, p;
	int err;

	if (g->zap_loaded) {
		return 0;       /* a6xx: loaded once per boot, no resume call */
	}
	if (!fw || len < 0x34 || le32(fw) != 0x464c457fu || fw[4] != 1) {
		return sgpu_fail(g, -22, "zap: not an ELF32 image (%u bytes)", len);
	}
	if (!g->zap_pa || !g->zap_size) {
		return sgpu_fail(g, -22, "zap: no zap-region in /sweet-gpu");
	}
	uint16_t phnum = le16(fw + 0x2c);
	if (phnum < 2 || elf_phdr(fw, len, 0, &p0) || p0.type == PT_LOAD) {
		return sgpu_fail(g, -22, "zap: bad program headers");
	}

	/* layout: min/max of loadable segments, relocatable flag, hash segment */
	uint64_t min_addr = ~0ull, max_addr = 0;
	bool relocate = false;
	int hash = -1;
	for (uint32_t i = 0; i < phnum; i++) {
		if (elf_phdr(fw, len, i, &p)) {
			return sgpu_fail(g, -22, "zap: phdr %u out of range", i);
		}
		if (hash < 0 && i >= 1 && (p.flags & MDT_TYPE_MASK) == MDT_TYPE_HASH) {
			hash = (int)i;
		}
		if (!phdr_loadable(&p)) {
			continue;
		}
		relocate |= (p.flags & MDT_RELOCATABLE) != 0;
		if (p.paddr < min_addr) {
			min_addr = p.paddr;
		}
		if ((uint64_t)p.paddr + p.memsz > max_addr) {
			max_addr = (uint64_t)p.paddr + p.memsz;
		}
	}
	max_addr = (max_addr + 0xfff) & ~0xfffull;
	if (hash < 0 || min_addr == ~0ull) {
		return sgpu_fail(g, -22, "zap: no hash or loadable segment");
	}
	uint64_t mem_size = max_addr - min_addr;
	if (mem_size > g->zap_size) {
		return sgpu_fail(g, -7, "zap: needs %#llx bytes, region is %#x",
		           (unsigned long long)mem_size, g->zap_size);
	}

	/* metadata = ELF header + phdrs (phdr[0].filesz) followed by the hash segment */
	struct phdr ph;
	elf_phdr(fw, len, (uint32_t)hash, &ph);
	if (p0.filesz > len || ph.offset > len || ph.filesz > len - ph.offset) {
		return sgpu_fail(g, -22, "zap: metadata out of range");
	}
	/*
	 * Linux allocates this from the SCM device's DMA pool, i.e. below
	 * 4 GiB. On apollo XNU owns only the upper DDR bank, so prefer a
	 * low bounce buffer from the ADT (scm-bounce = <pa size>, a free page
	 * in 0xa0000000.. that ABL used for the shim) and fall back to kernel
	 * memory above 4 GiB, which TrustZone may or may not accept.
	 */
	uint32_t meta_len = p0.filesz + ph.filesz;
	uint64_t res[3] = { 0 };
	struct sgpu_dma meta;
	err = sgpu_dma_alloc(meta_len, 4096, 0, &meta);
	if (err) {
		return sgpu_fail(g, -12, "zap: no metadata buffer");
	}
	uint8_t *m = meta.va;
	for (uint32_t i = 0; i < p0.filesz; i++) {
		m[i] = fw[i];
	}
	for (uint32_t i = 0; i < ph.filesz; i++) {
		m[p0.filesz + i] = fw[ph.offset + i];
	}
	uint64_t meta_pa = meta.pa;
	const void *bp;
	uint32_t blen;
	if (sgpu_dt_prop("scm-bounce", &bp, &blen) && blen == 16 &&
	    ((const uint64_t *)bp)[1] >= meta_len) {
		meta_pa = ((const uint64_t *)bp)[0];
		volatile uint8_t *bva = sgpu_ioremap(meta_pa, ((const uint64_t *)bp)[1]);
		if (!bva) {
			sgpu_dma_free(&meta);
			return sgpu_fail(g, -12, "zap: cannot map scm-bounce");
		}
		copy_to_io(bva, 0, m, meta_len, meta_len);
	} else {
		/* TZ reads (and XPU-locks) it by physical address: push it to DRAM */
		sgpu_cache_clean(meta.va, meta.size);
		sgpu_cache_inval(meta.va, meta.size);
	}
	sgpu_log("zap: metadata %u bytes at %#llx%s", meta_len, (unsigned long long)meta_pa,
	    meta_pa == meta.pa ? " (kernel memory, no scm-bounce)" : " (low bounce)");
	err = scm_call(SCM_STD(SCM_SVC_PIL, SCM_PIL_INIT_IMAGE),
	    SCM_ARGS(2) | (SCM_ARG_RW << 6), GPU_PAS_ID, meta_pa, 0, res);
	sgpu_dma_free(&meta);
	if (err || res[0]) {
		return sgpu_fail(g, err ? err : -5, "zap: PAS init_image failed (err %d x1 %#llx)",
		           err, (unsigned long long)res[0]);
	}
	if (relocate) {
		err = scm_call(SCM_STD(SCM_SVC_PIL, SCM_PIL_MEM_SETUP), SCM_ARGS(3),
		    GPU_PAS_ID, g->zap_pa, mem_size, res);
		if (err || res[0]) {
			return sgpu_fail(g, err ? err : -5, "zap: PAS mem_setup failed");
		}
	}

	/* copy segments into the carve-out */
	volatile uint8_t *region = sgpu_ioremap(g->zap_pa, g->zap_size);
	if (!region) {
		return sgpu_fail(g, -12, "zap: cannot map region");
	}
	uint64_t reloc = relocate ? min_addr : g->zap_pa;
	for (uint32_t i = 0; i < phnum; i++) {
		elf_phdr(fw, len, i, &p);
		if (!phdr_loadable(&p)) {
			continue;
		}
		if (p.paddr < reloc || p.paddr - reloc + p.memsz > mem_size ||
		    p.filesz > p.memsz || p.offset > len || p.filesz > len - p.offset) {
			return sgpu_fail(g, -22, "zap: segment %u (paddr %#x) does not fit", i, p.paddr);
		}
		copy_to_io(region, (uint32_t)(p.paddr - reloc), fw + p.offset, p.filesz, p.memsz);
	}
	sgpu_log("zap: %u segments, %#llx bytes at %#llx (%s)", phnum,
	    (unsigned long long)mem_size, (unsigned long long)g->zap_pa,
	    relocate ? "relocatable" : "fixed");

	err = scm_call(SCM_STD(SCM_SVC_PIL, SCM_PIL_AUTH_RESET), SCM_ARGS(1),
	    GPU_PAS_ID, 0, 0, res);
	if (err || res[0]) {
		return sgpu_fail(g, err ? err : -5, "zap: PAS auth_and_reset refused (err %d x1 %#llx)",
		           err, (unsigned long long)res[0]);
	}
	g->zap_loaded = true;
	sgpu_log("zap: authenticated by TrustZone");
	return 0;
}

/* a5xx-style resume; kept as an experiment knob (sweetgpuzapresume=1). */
int
sgpu_zap_resume(struct sgpu *g)
{
	uint64_t res[3] = { 0 };
	int err = scm_call(SCM_STD(SCM_SVC_BOOT, SCM_BOOT_SET_REMOTE), SCM_ARGS(2),
	    0 /* SCM_GPU_ZAP_SHADER_RESUME */, GPU_PAS_ID, 0, res);
	if (err) {
		return sgpu_fail(g, err, "zap: set_remote_state resume failed");
	}
	return 0;
}
#endif
