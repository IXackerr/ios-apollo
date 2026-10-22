/*
 * main.c - host test of the SWEET Adreno 650 driver against model.c.
 *
 *   make -C hosttest && ./hosttest/sgpu_hosttest [-v]
 *
 * Runs the real driver sources (kernel/sweet_gpu_*.c) unchanged, compiled
 * with -DSGPU_HOST, through every bring-up stage, then libsweetgpu +
 * gputest through the host dispatcher, then failure scenarios. Exit code
 * 0 = all scenarios passed with zero model errors.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model.h"
#include "../kernel/sweet_gpu.h"

extern struct sgpu host_g;
extern int host_verbose;
extern bool host_dt_zap;
void host_dispatch_reset(void);
int gputest_main(int argc, char **argv);

static int failures;

#define EXPECT(cond, ...) do { if (!(cond)) { failures++; \
	printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void
fresh(const struct model_opts *o)
{
	/* the driver keeps static state per module (like a kernel); re-zero ours */
	memset(&host_g, 0, sizeof(host_g));
	model_reset(o);
	host_dispatch_reset();
	host_clear_bootargs();
}

static void
expect_gpu(uint32_t dw, uint32_t want, const char *name)
{
	uint32_t v = model_gpu_reg(dw);
	EXPECT(v == want, "%s (%#x) = %#x, want %#x", name, dw, v, want);
}

static void
scenario(const char *name)
{
	printf("\n=== %s ===\n", name);
}

static int
end_scenario(int before)
{
	EXPECT(model_errors == 0, "%d model errors", model_errors);
	printf("--- %s\n", failures == before ? "PASS" : "FAIL");
	return failures == before;
}

/* ---- 1: PM4 encoder reference vs. the spec's precomputed headers ---- */
static void
test_pm4(void)
{
	int f = failures;
	scenario("PM4 header encodings (spec 5.1 table)");
	EXPECT(ref_pkt7(0x48, 8) == 0x70c80008, "CP_ME_INIT");
	EXPECT(ref_pkt7(0x66, 1) == 0x70e60001, "CP_SET_SECURE_MODE");
	EXPECT(ref_pkt7(0x3f, 3) == 0x70bf8003, "CP_INDIRECT_BUFFER");
	EXPECT(ref_pkt7(0x46, 1) == 0x70460001, "CP_EVENT_WRITE/1");
	EXPECT(ref_pkt7(0x46, 4) == 0x70460004, "CP_EVENT_WRITE/4");
	EXPECT(ref_pkt7(0x53, 4) == 0x70d30004, "CP_SMMU_TABLE_UPDATE");
	EXPECT(ref_pkt7(0x3d, 5) == 0x703d8005, "CP_MEM_WRITE/5");
	EXPECT(ref_pkt7(0x3c, 6) == 0x70bc8006, "CP_WAIT_REG_MEM");
	EXPECT(ref_pkt7(0x5f, 1) == 0x70df0001, "CP_SET_PROTECTED_MODE");
	EXPECT(ref_pkt7(0x3e, 3) == 0x703e8003, "CP_REG_TO_MEM");
	EXPECT(ref_pkt7(0x10, 0) == 0x70108000, "CP_NOP");
	EXPECT(ref_pkt4(0x885, 1) == 0x48088501, "PKT4 CP_SCRATCH_REG(2)");
	EXPECT(ref_pkt4(0x50e, 1) == 0x40050e01, "PKT4 RBBM_PERFCTR_SRAM_INIT_CMD");
	end_scenario(f);
}

/* ---- 2: full bring-up + register audit ---- */
static void
test_bringup(void)
{
	int f = failures;
	struct model_opts o = { .zap_present = true, .speed_bin = 1 };
	scenario("stage 0: driver inert");
	fresh(&o);
	EXPECT(sgpu_bringup(&host_g, 0) == 0, "bringup(0)");
	EXPECT(model_accesses == 0, "stage 0 touched %llu registers",
	    (unsigned long long)model_accesses);

	scenario("full bring-up to stage 8 (zap present, speed bin 1)");
	fresh(&o);
	int err = sgpu_bringup(&host_g, SGPU_STAGE_USER);
	EXPECT(err == 0, "bring-up failed: %d (%s)", err, host_g.message);
	EXPECT(host_g.stage_reached == SGPU_STAGE_USER, "stage %u", host_g.stage_reached);

	/* HFI order; the second GX_BW_PERF_VOTE is DCVS raising the level
	 * for the selftest's submission */
	static const uint32_t want_hfi[] = { 4, 3, 14, 10, 30, 30 };
	EXPECT(model_hfi_count == 6, "%d HFI messages", model_hfi_count);
	for (int i = 0; i < 6 && i < model_hfi_count; i++) {
		EXPECT(model_hfi_log[i] == want_hfi[i], "HFI msg %d id %u want %u", i,
		    model_hfi_log[i], want_hfi[i]);
	}

	/* CP / RBBM values from cp-hwinit spec 1.1 */
	expect_gpu(0x802, 0x0000020c, "CP_RB_CNTL");
	expect_gpu(0x844, 0x0000006e, "CP_APRIV_CNTL");
	expect_gpu(0x038, 0x03d0e242, "RBBM_INT_0_MASK");
	expect_gpu(0x84f, 0x0000000b, "CP_PROTECT_CNTL");
	expect_gpu(0x850, 0x13fc0000, "CP_PROTECT(0)");
	expect_gpu(0x875, 0x806df887, "CP_PROTECT(37)");
	expect_gpu(0x87f, 0x8001f8c0, "CP_PROTECT(47)");
	expect_gpu(0x876, 0x00000000, "CP_PROTECT(38) untouched");
	expect_gpu(0x8e08, 0x00000816, "RB_NC_MODE_CNTL");
	expect_gpu(0xb604, 0x00000006, "TPL1_NC_MODE_CNTL");
	expect_gpu(0xae02, 0x00000026, "SP_NC_MODE_CNTL");
	expect_gpu(0xe01, 0x00600000, "UCHE_MODE_CNTL");
	expect_gpu(0x534, 0x00000001, "RBBM_NC_MODE_CNTL");
	expect_gpu(0x0ae, 0x8aa8aa82, "RBBM_CLOCK_CNTL");
	expect_gpu(0x0b0, 0x02222222, "RBBM_CLOCK_CNTL_SP0");
	expect_gpu(0x11a, 0x00000555, "RBBM_CLOCK_HYST_GMU_GX");
	expect_gpu(0x01f, 0x401fffff, "RBBM_INTERFACE_HANG_INT_CNTL");
	expect_gpu(0x3c03, 0x00071620, "GBIF_QSB_SIDE0");
	expect_gpu(0x8c2, 0x02000140, "CP_ROQ_THRESHOLDS_2");
	expect_gpu(0x9e00, 0x00300200, "PC_DBG_ECO_CNTL");
	expect_gpu(0xe09, 0xfffff000, "UCHE_TRAP_BASE lo");
	expect_gpu(0xe0a, 0x0000007f, "UCHE_TRAP_BASE hi");
	expect_gpu(0xb60c, 0x3f0243f0, "TPL1_BICUBIC_WEIGHTS_TABLE_4");
	for (uint32_t dw = 0x842; dw <= 0x842; dw++) expect_gpu(dw, 1, "CP_ADDR_MODE_CNTL");
	expect_gpu(0xf810, 1, "RBBM_SECVID_TSB_ADDR_MODE_CNTL");
	expect_gpu(0x808, 1, "CP_SQE_CNTL");
	/* GMU-side writes of hw_init */
	EXPECT(model_gmu_reg(0x24c24) == 0x00020202, "GMU CGC_MODE");
	EXPECT(model_gmu_reg(0x24c38) == 0xff000000, "GMU CX_BUSY_MASK");
	EXPECT((model_gmu_reg(0x14104) & 0xff) == 0x20, "GMU power counter select");
	EXPECT((model_gmu_reg(0x200) & 1) == 1, "SPTPRAC clock control restored");
	/* SMMU: spec 3.6 */
	EXPECT(model_smmu_reg(0xc00) == 0x00000000, "S2CR GPU %#x", model_smmu_reg(0xc00));
	EXPECT(model_smmu_reg(0x800) == 0x84010000, "SMR GPU %#x", model_smmu_reg(0x800));
	EXPECT(model_smmu_reg(0x804) == 0x84000005, "SMR GMU %#x", model_smmu_reg(0x804));
	EXPECT(model_smmu_reg(0xc04) == 0x00000001, "S2CR GMU %#x", model_smmu_reg(0xc04));
	EXPECT(model_smmu_reg(0x8000) == 0x1167, "CB0 SCTLR %#x", model_smmu_reg(0x8000));
	EXPECT(model_smmu_reg(0x8030) == 0x00802019, "CB0 TCR %#x", model_smmu_reg(0x8030));
	EXPECT(model_smmu_reg(0x8010) == 0x00038011, "CB0 TCR2 %#x", model_smmu_reg(0x8010));
	EXPECT(model_smmu_reg(0x8038) == 0xf404ff44, "CB0 MAIR0");
	EXPECT(model_smmu_reg(0x1000) == 0x0001f300, "CBAR0 %#x", model_smmu_reg(0x1000));
	EXPECT(model_smmu_reg(0x1800) == 0x00000001, "CBA2R0");
	EXPECT((model_smmu_reg(0x9024) >> 16) == 1, "CB1 TTBR0 ASID %#x", model_smmu_reg(0x9024));
	/* RSC */
	EXPECT(model_rscc_reg(0x10) & (1u << 24), "RSCC SDE clock gating");
	EXPECT(model_rscc_reg(0x48) == 0x80000000, "RSCC CMD2 DATA");
	EXPECT(model_pdc_reg(0x4500) == 0x80000001, "PDC enable");

	struct sgpu_info info;
	sgpu_fill_info(&host_g, &info);
	EXPECT(info.gpu_id == 650 && info.chip_id == 0x06050002 && info.gmem_size == 0x120000 &&
	    info.gmem_base == 0 && info.highest_bank_bit == 16 && info.max_contexts == 64,
	    "info fields");
	EXPECT(info.max_freq_hz == 670000000, "highest level %u Hz", info.max_freq_hz);
	EXPECT(host_g.speed_bin == 1, "speed bin %u", host_g.speed_bin);
	end_scenario(f);

	f = failures;
	scenario("DCVS: highest level after a submission, lowest when idle");
	uint32_t top = (sgpu_gmu_state(&host_g) >> 16) & 0xff;
	EXPECT(top > 1, "level after the selftest %u", top);
	sgpu_dcvs_tick(&host_g);
	EXPECT(((sgpu_gmu_state(&host_g) >> 16) & 0xff) == top, "dropped before the idle time");
	sgpu_msleep(120);
	sgpu_dcvs_tick(&host_g);
	EXPECT(((sgpu_gmu_state(&host_g) >> 16) & 0xff) == 1, "level %u after 120 ms idle",
	    (sgpu_gmu_state(&host_g) >> 16) & 0xff);
	EXPECT(model_hfi_count == 7 && model_hfi_log[6] == 30, "%d HFI messages", model_hfi_count);
	end_scenario(f);

	/* ---- user space on top ---- */
	f = failures;
	scenario("libsweetgpu + gputest (basic, 3000 submits = ring wraps, fault report)");
	char *argv[] = { "gputest", "-n", "3000", "-f" };
	int rc = gputest_main(4, argv);
	EXPECT(rc == 0, "gputest rc %d", rc);
	EXPECT(host_g.faults >= 1, "faults %u", host_g.faults);
	model_errors = 0;       /* the deliberate fault is reported as a model error too */
	end_scenario(f);

	/* ---- privilege: user IB writing the kernel ring ---- */
	f = failures;
	scenario("user IB cannot write privileged kernel buffers");
	{
		struct sgpu_mem_alloc m = { .size = 0x4000 };
		extern int sgpu_host_dispatch(uint32_t, const void *, size_t, void *, size_t *);
		size_t ol = sizeof(m);
		struct sgpu_ctx_create cc = { 0 };
		size_t cl = sizeof(cc);
		EXPECT(sgpu_host_dispatch(SGPU_SEL_CTX_CREATE, &cc, sizeof(cc), &cc, &cl) == 0, "ctx");
		EXPECT(sgpu_host_dispatch(SGPU_SEL_MEM_ALLOC, &m, sizeof(m), &m, &ol) == 0, "alloc");
		volatile uint32_t *c = (volatile uint32_t *)(uintptr_t)m.cpu_addr;
		c[0] = ref_pkt7(0x3d, 3);
		c[1] = (uint32_t)host_g.rb.iova;
		c[2] = (uint32_t)(host_g.rb.iova >> 32);
		c[3] = 0xbadbad;
		struct sgpu_submit s = { .ctx_id = cc.ctx_id, .num_ibs = 1 };
		s.ibs[0] = (struct sgpu_ib){ m.gpuaddr, 4, 0 };
		size_t sl = sizeof(s);
		uint32_t before = host_g.faults;
		uint32_t ring0 = ((volatile uint32_t *)host_g.rb.dma.va)[0];
		EXPECT(sgpu_host_dispatch(SGPU_SEL_SUBMIT, &s, sizeof(s), &s, &sl) == 0, "submit");
		struct sgpu_status st;
		size_t stl = sizeof(st);
		sgpu_host_dispatch(SGPU_SEL_GET_STATUS, NULL, 0, &st, &stl);
		EXPECT(st.faults > before, "no permission fault reported");
		EXPECT(((volatile uint32_t *)host_g.rb.dma.va)[0] == ring0, "ring was overwritten");
		/* an IB outside client memory is rejected before it reaches the GPU */
		s.ibs[0].gpuaddr = host_g.rb.iova;
		EXPECT(sgpu_host_dispatch(SGPU_SEL_SUBMIT, &s, sizeof(s), &s, &sl) != 0,
		    "kernel ring accepted as an IB");
	}
	model_errors = 0;
	end_scenario(f);

	/* ---- hang + recovery ---- */
	f = failures;
	scenario("GPU hang: detection, abandon, re-init");
	{
		extern int sgpu_host_dispatch(uint32_t, const void *, size_t, void *, size_t *);
		struct sgpu_ctx_create cc = { 0 };
		size_t cl = sizeof(cc);
		sgpu_host_dispatch(SGPU_SEL_CTX_CREATE, &cc, sizeof(cc), &cc, &cl);
		struct sgpu_submit s = { .ctx_id = cc.ctx_id, .num_ibs = 0 };
		size_t sl = sizeof(s);
		model_set_stall(true);
		EXPECT(sgpu_host_dispatch(SGPU_SEL_SUBMIT, &s, sizeof(s), &s, &sl) == 0, "submit");
		struct sgpu_wait_ts w = { .ctx_id = cc.ctx_id, .timestamp = s.timestamp,
			.timeout_ms = 5000 };
		size_t wl = 0;
		int werr = sgpu_host_dispatch(SGPU_SEL_WAIT_TS, &w, sizeof(w), NULL, &wl);
		EXPECT(werr == 0, "wait after recovery returned %d", werr);
		EXPECT(host_g.hangs == 1, "hangs %u", host_g.hangs);
		EXPECT(host_g.stage_reached == SGPU_STAGE_USER, "stage %u after recovery",
		    host_g.stage_reached);
		/* GPU usable again */
		EXPECT(sgpu_host_dispatch(SGPU_SEL_SUBMIT, &s, sizeof(s), &s, &sl) == 0, "submit 2");
		w.timestamp = s.timestamp;
		EXPECT(sgpu_host_dispatch(SGPU_SEL_WAIT_TS, &w, sizeof(w), NULL, &wl) == 0, "wait 2");
	}
	end_scenario(f);
}

/* ---- 3: secure-mode fallback paths ---- */
static void
test_no_zap(void)
{
	int f = failures;
	struct model_opts o = { 0 };
	scenario("no zap firmware: SECVID_TRUST_CNTL fallback (unlocked TZ)");
	host_dt_zap = false;
	fresh(&o);
	EXPECT(sgpu_bringup(&host_g, SGPU_STAGE_SELFTEST) == 0, "bring-up: %s", host_g.message);
	end_scenario(f);

	f = failures;
	scenario("no zap firmware on a locked TZ: XPU violation is visible");
	o.tz_protects_secvid = true;
	fresh(&o);
	sgpu_bringup(&host_g, SGPU_STAGE_SELFTEST);
	EXPECT(model_errors > 0, "expected the model to flag the violation");
	EXPECT(host_g.stage_reached < SGPU_STAGE_SELFTEST, "selftest must not pass in secure mode");
	model_errors = 0;
	host_dt_zap = true;
	end_scenario(f);
}

/* ---- 4: SMMU coexistence with firmware state ---- */
static void
test_smmu_firmware_state(void)
{
	int f = failures;
	struct model_opts o = { .fw_sme_sid2 = true };
	scenario("firmware SME (SID 2 -> live CB3) is left alone");
	fresh(&o);
	EXPECT(sgpu_bringup(&host_g, SGPU_STAGE_SELFTEST) == 0, "bring-up: %s", host_g.message);
	EXPECT(model_smmu_reg(0x800) == 0x84000002, "firmware SMR changed");
	EXPECT(model_smmu_reg(0xc00) == 3, "firmware S2CR changed");
	EXPECT(model_smmu_reg(0xb000) == 1, "firmware CB3 changed");
	end_scenario(f);

	f = failures;
	o = (struct model_opts){ .cb0_live = true };
	scenario("CB0 live at boot: refuse without sweetgpu_smmu_force");
	fresh(&o);
	EXPECT(sgpu_bringup(&host_g, SGPU_STAGE_SMMU) != 0, "should refuse");
	EXPECT(host_g.stage_reached == SGPU_STAGE_CLOCKS, "stage %u", host_g.stage_reached);
	fresh(&o);
	host_set_bootarg("sweetgpu_smmu_force", 1);
	EXPECT(sgpu_bringup(&host_g, SGPU_STAGE_SMMU) == 0, "forced: %s", host_g.message);
	end_scenario(f);

	f = failures;
	o = (struct model_opts){ .clientpd = true };
	scenario("SMMU in global bypass (CLIENTPD) at boot: cleared");
	fresh(&o);
	EXPECT(sgpu_bringup(&host_g, SGPU_STAGE_SELFTEST) == 0, "bring-up: %s", host_g.message);
	EXPECT(!(model_smmu_reg(0) & 1), "CLIENTPD still set");
	EXPECT(model_smmu_reg(0) & (1u << 10), "USFCFG lost");
	end_scenario(f);

	f = failures;
	o = (struct model_opts){ .hv_filters_s2cr = true };
	scenario("hypervisor drops S2CR writes: detected, bring-up stops");
	fresh(&o);
	EXPECT(sgpu_bringup(&host_g, SGPU_STAGE_SMMU) != 0, "should fail");
	EXPECT(strstr(host_g.message, "filtered") != NULL, "message: %s", host_g.message);
	end_scenario(f);
}

/* ---- 5: speed bins / frequency control ---- */
static void
test_freq(void)
{
	int f = failures;
	struct model_opts o = { .speed_bin = 3 };
	scenario("speed bin 3: 5 levels up to 525 MHz; sweetgpu_freq selects a level");
	fresh(&o);
	host_set_bootarg("sweetgpu_freq", 99);
	EXPECT(sgpu_bringup(&host_g, SGPU_STAGE_GMU) == 0, "bring-up: %s", host_g.message);
	EXPECT(sgpu_gmu_max_freq_hz(&host_g) == 525000000, "freq %u", sgpu_gmu_max_freq_hz(&host_g));
	EXPECT(sgpu_gmu_set_freq(&host_g, 2) == 0, "set_freq");
	EXPECT(sgpu_gmu_max_freq_hz(&host_g) == 400000000, "freq %u", sgpu_gmu_max_freq_hz(&host_g));
	EXPECT(sgpu_gmu_set_freq(&host_g, 6) != 0, "level 6 must not exist on bin 3");
	end_scenario(f);
}

int
main(int argc, char **argv)
{
	host_verbose = argc > 1 && !strcmp(argv[1], "-v");
	test_pm4();
	test_bringup();
	test_no_zap();
	test_smmu_firmware_state();
	test_freq();
	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
