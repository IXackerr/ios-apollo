/*
 * sweet_gpu_core.c - staged bring-up of the Adreno 650 on SM8250 (apollo).
 *
 * boot-arg sweetgpu=N stops after stage N (sweet_gpu.h enum sgpu_stage).
 * Every stage logs what it did and what it read back, so a phone photo of
 * the console (or the persistent log, patch 0047) shows exactly how far
 * the hardware got. Default 0: the driver maps nothing and touches no
 * register, so a kernel with this patch behaves like one without it.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(SGPU_HOST)
#include <stdarg.h>
#include "sweet_gpu.h"

#if defined(SGPU_HOST)
#include <stdio.h>
#define sgpu_vsnprintf vsnprintf
#else
extern int vsnprintf(char *, size_t, const char *, va_list);
#define sgpu_vsnprintf vsnprintf
#endif

/* NUL-separated, not a pointer table: the driver also runs in the boot
 * shim, which cannot apply absolute relocations. */
static const char stage_list[] =
	"off\0probe\0clocks\0smmu\0gmu\0gx\0cp\0selftest\0user\0";

const char *
sgpu_nth(const char *list, unsigned i)
{
	while (i-- > 0) {
		while (*list++) {
		}
	}
	return list;
}

#define stage_names(i) sgpu_nth(stage_list, (unsigned)(i))

int
sgpu_poll(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t want,
    uint32_t timeout_us)
{
	uint64_t start = sgpu_now_us();

	for (;;) {
		if ((sgpu_rd(base, off) & mask) == want) {
			return 0;
		}
		if (sgpu_now_us() - start > timeout_us) {
			return -60;     /* ETIMEDOUT */
		}
		sgpu_udelay(1);
	}
}

int
sgpu_fail(struct sgpu *g, int err, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	sgpu_vsnprintf(g->message, sizeof(g->message), fmt, ap);
	va_end(ap);
	g->last_error = err;
	sgpu_log("FAILED at stage %u (%s): %s (err %d)", g->stage_reached + 1,
	    g->stage_reached + 1 <= SGPU_STAGE_MAX ? stage_names(g->stage_reached + 1) : "?",
	    g->message, err);
	return err;
}

static int
map_mmio(struct sgpu *g)
{
	/* Physical layout from sm8250.dtsi and the stock kona DT (qcom,kgsl-3d0, qcom,gmu). */
	g->gpu    = sgpu_ioremap(0x03d00000, 0x40000);
	g->cx_dbgc = sgpu_ioremap(0x03d61000, 0x800);
	g->gmu     = sgpu_ioremap(0x03d6a000, 0x30000);
	g->gpucc   = sgpu_ioremap(0x03d90000, 0x9000);
	g->smmu    = sgpu_ioremap(0x03da0000, 0x10000);
	g->rscc    = sgpu_ioremap(0x03de0000, 0x10000);
	g->pdc     = sgpu_ioremap(0x0b290000, 0x10000);
	g->pdc_seq = sgpu_ioremap(0x0b490000, 0x10000);
	g->gcc     = sgpu_ioremap(0x00100000, 0x1f0000);
	if (!g->gpu || !g->cx_dbgc || !g->gmu || !g->gpucc || !g->smmu ||
	    !g->rscc || !g->pdc || !g->pdc_seq || !g->gcc) {
		return sgpu_fail(g, -12, "MMIO mapping failed");
	}
	return 0;
}

static int
load_firmware_refs(struct sgpu *g)
{
	const void *p;
	uint32_t len;

	if (sgpu_dt_prop("sqe-firmware", &p, &len)) {
		g->sqe_fw = p;
		g->sqe_fw_len = len;
	}
	if (sgpu_dt_prop("gmu-firmware", &p, &len)) {
		g->gmu_fw = p;
		g->gmu_fw_len = len;
	}
	if (sgpu_dt_prop("zap-firmware", &p, &len)) {
		g->zap_fw = p;
		g->zap_fw_len = len;
	}
	if (sgpu_dt_prop("zap-region", &p, &len) && len == 16) {
		const uint64_t *r = p;
		g->zap_pa = r[0];
		g->zap_size = (uint32_t)r[1];
	}
	if (sgpu_dt_prop("chip-id", &p, &len) && len == 4) {
		g->chip_id = *(const uint32_t *)p;
	} else {
		g->chip_id = 0x06050002;
	}
	if (sgpu_dt_prop("speed-bin", &p, &len) && len == 4) {
		g->speed_bin = *(const uint32_t *)p;
	}
	sgpu_log("firmware: sqe %u bytes, gmu %u bytes, zap %u bytes, zap region %#llx+%#x, chip %#x",
	    g->sqe_fw_len, g->gmu_fw_len, g->zap_fw_len,
	    (unsigned long long)g->zap_pa, g->zap_size, g->chip_id);
	return 0;
}

static int
run_stage(struct sgpu *g, uint32_t s)
{
	switch (s) {
	case SGPU_STAGE_PROBE: {
		int err = map_mmio(g);
		if (!err) {
			err = load_firmware_refs(g);
		}
		if (!err) {
			err = sgpu_va_init(g);
		}
		if (!err) {
			err = sgpu_clk_probe(g);
		}
		if (!err) {
			err = sgpu_scm_probe(g);
		}
		return err;
	}
	case SGPU_STAGE_CLOCKS:
		return sgpu_clk_enable(g);
	case SGPU_STAGE_SMMU:
		return sgpu_smmu_init(g);
	case SGPU_STAGE_GMU:
		if (!g->gmu_fw) {
			return sgpu_fail(g, -2, "no gmu-firmware in /sweet-gpu (mkadt --gpu-fw)");
		}
		return sgpu_gmu_init(g);
	case SGPU_STAGE_GX:
		return sgpu_gmu_gx_on(g);
	case SGPU_STAGE_CP:
		if (!g->sqe_fw) {
			return sgpu_fail(g, -2, "no sqe-firmware in /sweet-gpu");
		}
		return sgpu_cp_init(g);
	case SGPU_STAGE_SELFTEST:
		return sgpu_cp_selftest(g);
	case SGPU_STAGE_USER:
		return 0;       /* SweetAdreno.cpp publishes the service */
	default:
		return -22;
	}
}

int
sgpu_bringup(struct sgpu *g, uint32_t stage)
{
	if (stage > SGPU_STAGE_MAX) {
		stage = SGPU_STAGE_MAX;
	}
	g->stage_requested = stage;
	sgpu_log("bring-up to stage %u (%s)", stage, stage_names(stage));
	while (g->stage_reached < stage) {
		uint32_t s = g->stage_reached + 1;
		uint64_t t0 = sgpu_now_us();
		int err = run_stage(g, s);
		if (err) {
			if (!g->last_error) {
				sgpu_fail(g, err, "stage %s returned an error", stage_names(s));
			}
			return err;
		}
		g->stage_reached = s;
		sgpu_log("stage %u (%s) OK in %llu us", s, stage_names(s),
		    (unsigned long long)(sgpu_now_us() - t0));
	}
	return 0;
}

void
sgpu_fill_info(struct sgpu *g, struct sgpu_info *info)
{
	*info = (struct sgpu_info){
		.uapi_version      = SGPU_UAPI_VERSION,
		.gpu_id            = 650,
		.chip_id           = g->chip_id,
		.gmem_size         = SGPU_A650_GMEM_SIZE,
		.gmem_base         = 0,            /* a650 family: GMEM not in UCHE range */
		.uche_trap_base    = SGPU_UCHE_TRAP_BASE,
		.va_start          = SGPU_VA_USER_BASE,
		.va_size           = SGPU_VA_USER_SIZE,
		.highest_bank_bit  = 16,
		.ubwc_mode         = 4,
		.min_access_length = 32,
		.timer_freq_hz     = 19200000,
		.max_freq_hz       = sgpu_gmu_max_freq_hz(g),
		.max_contexts      = SGPU_MAX_CTX,
		.flags             = g->io_coherent ? SGPU_INFO_IO_COHERENT : 0,
	};
}
#endif
