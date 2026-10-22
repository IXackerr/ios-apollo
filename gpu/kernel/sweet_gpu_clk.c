/*
 * sweet_gpu_clk.c - GPU clocks and the CX power domain on SM8250.
 *
 * No clock framework here: the registers are programmed directly, in the
 * order Linux's a6xx_gmu_resume() ends up producing through gcc-sm8250.c,
 * gpucc-sm8250.c, clk-rcg2.c, clk-branch.c and gdsc.c. Full derivation with
 * file:line references: docs/specs/clocks-scm-zap-a650.md.
 *
 * What the CPU owns (and this file does):
 *   GCC:    GPLL0 vote, GPLL0(+div) votes towards GPU_CC, GPU config AHB,
 *           memnoc/axi/snoc-dvm branches
 *   GPU_CC: GMU RCG at 200 MHz from GPLL0 even (no PLL needed), the
 *           AHB / CX_GMU / CXO branches, the CX GDSC
 * What the GMU firmware owns: the GX GDSC and the GX rail (gfx.lvl).
 * GPU_CC_PLL1 (500 MHz GMU option) is not used.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(SGPU_HOST)
#include "sweet_gpu.h"

/* GCC (base 0x00100000), offsets */
#define GCC_GPLL0_MODE                  0x00000
#define GCC_GPLL0_L_VAL                 0x00004
#define GCC_APCS_GPLL_VOTE              0x52018         /* bit0 = GPLL0 */
#define GCC_APCS_CLOCK_VOTE             0x52000
#define   GCC_VOTE_GPU_GPLL0            (1u << 15)
#define   GCC_VOTE_GPU_GPLL0_DIV        (1u << 16)
#define GCC_GPU_CFG_AHB_CBCR            0x71004
#define GCC_GPU_MEMNOC_GFX_CBCR         0x7100c
#define GCC_GPU_SNOC_DVM_GFX_CBCR       0x71018
#define GCC_GPU_MISC                    0x71028         /* [1:0] = 3 at Linux GCC probe */
#define GCC_DDRSS_GPU_AXI_CBCR          0x71154

/* GPU_CC (base 0x03d90000), offsets */
#define GPUCC_GX_GDSCR                  0x100c
#define GPUCC_GX_CFG_GDSCR              0x1010
#define GPUCC_CX_GDSCR                  0x106c
#define GPUCC_AHB_CBCR                  0x1078
#define GPUCC_CX_GMU_CBCR               0x1098
#define GPUCC_CXO_CBCR                  0x109c
#define GPUCC_GMU_CMD_RCGR              0x1120
#define GPUCC_GMU_CFG_RCGR              0x1124
#define GPUCC_GX_CLAMP_IO               0x1508
#define GPUCC_CX_GDS_HW_CTRL            0x1540

/* clock register fields */
#define PLL_ACTIVE_FLAG                 (1u << 30)
#define CBCR_CLK_ENABLE                 (1u << 0)
#define CBCR_CLK_OFF                    (1u << 31)
#define CBCR_FSM_SHIFT                  28
#define RCG_CMD_UPDATE                  (1u << 0)
#define RCG_CMD_ROOT_OFF                (1u << 31)
#define RCG_CFG_SRC_DIV_MASK            0x1fu
#define RCG_CFG_SRC_SEL_MASK            (7u << 8)
#define RCG_CFG_MODE_MASK               (3u << 12)
#define RCG_CFG_HW_CLK_CTRL             (1u << 20)
#define GMU_SRC_GPLL0_DIV               6u      /* parent map: gcc_gpu_gpll0_div_clk_src */
#define GMU_DIV_200MHZ                  2u      /* 300 MHz / 1.5 -> pre_div 2*1.5-1 */
#define GDSC_SW_COLLAPSE                (1u << 0)
#define GDSC_INIT_MASK                  0x00fff006u     /* HW_CTRL|SW_OVERRIDE|waits */
#define GDSC_INIT_VAL                   0x00282000u     /* EN_REST 2, EN_FEW 8, CLK_DIS 2 */
#define GDS_HW_CTRL_PWR_ON              (1u << 31)
#define GX_CFG_POWER_DOWN_COMPLETE      (1u << 15)

static bool
branch_is_on(uint32_t cbcr)
{
	return !(cbcr & CBCR_CLK_OFF) || ((cbcr >> CBCR_FSM_SHIFT) & 7u) == 2u;
}

/* Enable a branch clock; poll = false for HALT_DELAY branches. */
static int
branch_enable(struct sgpu *g, volatile uint8_t *base, uint32_t off, bool poll, const char *name)
{
	sgpu_rmw(base, off, 0, CBCR_CLK_ENABLE);
	sgpu_wmb();
	if (!poll) {
		sgpu_udelay(10);
		return 0;
	}
	for (int i = 0; i < 200; i++) {
		if (branch_is_on(sgpu_rd(base, off))) {
			return 0;
		}
		sgpu_udelay(1);
	}
	return sgpu_fail(g, -16, "clock %s stuck off, CBCR %#x", name, sgpu_rd(base, off));
}

int
sgpu_clk_probe(struct sgpu *g)
{
	sgpu_log("gcc: GPLL0 mode %#x L %u (%u MHz), vote %#x/%#x misc %#x",
	    sgpu_rd(g->gcc, GCC_GPLL0_MODE), sgpu_rd(g->gcc, GCC_GPLL0_L_VAL) & 0xffff,
	    (sgpu_rd(g->gcc, GCC_GPLL0_L_VAL) & 0xffff) * 192 / 10,
	    sgpu_rd(g->gcc, GCC_APCS_GPLL_VOTE), sgpu_rd(g->gcc, GCC_APCS_CLOCK_VOTE),
	    sgpu_rd(g->gcc, GCC_GPU_MISC));
	sgpu_log("gcc: cfg-ahb %#x memnoc %#x snoc-dvm %#x axi %#x",
	    sgpu_rd(g->gcc, GCC_GPU_CFG_AHB_CBCR), sgpu_rd(g->gcc, GCC_GPU_MEMNOC_GFX_CBCR),
	    sgpu_rd(g->gcc, GCC_GPU_SNOC_DVM_GFX_CBCR), sgpu_rd(g->gcc, GCC_DDRSS_GPU_AXI_CBCR));
	/*
	 * GPU_CC sits behind GCC_GPU_CFG_AHB_CLK. Reading it with that clock
	 * off can hang the bus, so only report it when the clock is on.
	 */
	if (branch_is_on(sgpu_rd(g->gcc, GCC_GPU_CFG_AHB_CBCR))) {
		sgpu_log("gpucc: cx-gdscr %#x hw %#x gx-gdscr %#x clamp %#x gmu-rcg %#x/%#x",
		    sgpu_rd(g->gpucc, GPUCC_CX_GDSCR), sgpu_rd(g->gpucc, GPUCC_CX_GDS_HW_CTRL),
		    sgpu_rd(g->gpucc, GPUCC_GX_GDSCR), sgpu_rd(g->gpucc, GPUCC_GX_CLAMP_IO),
		    sgpu_rd(g->gpucc, GPUCC_GMU_CMD_RCGR), sgpu_rd(g->gpucc, GPUCC_GMU_CFG_RCGR));
	} else {
		sgpu_log("gpucc: config AHB clock off, not read");
	}
	return 0;
}

static int
gmu_rcg_200mhz(struct sgpu *g)
{
	uint32_t cfg = sgpu_rd(g->gpucc, GPUCC_GMU_CFG_RCGR);

	cfg &= ~(RCG_CFG_SRC_DIV_MASK | RCG_CFG_SRC_SEL_MASK | RCG_CFG_MODE_MASK |
	    RCG_CFG_HW_CLK_CTRL);
	cfg |= (GMU_SRC_GPLL0_DIV << 8) | GMU_DIV_200MHZ;
	sgpu_wr(g->gpucc, GPUCC_GMU_CFG_RCGR, cfg);
	sgpu_rmw(g->gpucc, GPUCC_GMU_CMD_RCGR, 0, RCG_CMD_UPDATE);
	if (sgpu_poll(g->gpucc, GPUCC_GMU_CMD_RCGR, RCG_CMD_UPDATE, 0, 500)) {
		return sgpu_fail(g, -16, "GMU RCG update stuck, cmd %#x cfg %#x",
		           sgpu_rd(g->gpucc, GPUCC_GMU_CMD_RCGR), sgpu_rd(g->gpucc, GPUCC_GMU_CFG_RCGR));
	}
	return 0;
}

static int
cx_gdsc_on(struct sgpu *g)
{
	sgpu_rmw(g->gpucc, GPUCC_CX_GDSCR, GDSC_INIT_MASK, GDSC_INIT_VAL);
	sgpu_rmw(g->gpucc, GPUCC_CX_GDSCR, GDSC_SW_COLLAPSE, 0);    /* our vote: on */
	sgpu_wmb();
	sgpu_udelay(1);
	if (sgpu_poll(g->gpucc, GPUCC_CX_GDS_HW_CTRL, GDS_HW_CTRL_PWR_ON,
	    GDS_HW_CTRL_PWR_ON, 2000)) {
		return sgpu_fail(g, -60, "CX GDSC did not power on, gdscr %#x hw %#x",
		           sgpu_rd(g->gpucc, GPUCC_CX_GDSCR), sgpu_rd(g->gpucc, GPUCC_CX_GDS_HW_CTRL));
	}
	sgpu_udelay(1);
	return 0;
}

int
sgpu_clk_enable(struct sgpu *g)
{
	int err;

	/* GCC one-time setup, as Linux's GCC probe does */
	sgpu_rmw(g->gcc, GCC_GPU_MISC, 0, 3u);
	err = branch_enable(g, g->gcc, GCC_GPU_CFG_AHB_CBCR, true, "gpu-cfg-ahb");
	if (err) {
		return err;
	}

	/* GPLL0 (shared, normally already running) and its two GPU taps */
	sgpu_rmw(g->gcc, GCC_APCS_GPLL_VOTE, 0, 1u);
	if (sgpu_poll(g->gcc, GCC_GPLL0_MODE, PLL_ACTIVE_FLAG, PLL_ACTIVE_FLAG, 1500)) {
		return sgpu_fail(g, -60, "GPLL0 not active, mode %#x", sgpu_rd(g->gcc, GCC_GPLL0_MODE));
	}
	sgpu_rmw(g->gcc, GCC_APCS_CLOCK_VOTE, 0, GCC_VOTE_GPU_GPLL0 | GCC_VOTE_GPU_GPLL0_DIV);
	sgpu_wmb();
	sgpu_udelay(10);

	/* GPU_CC one-time setup (gpucc probe): CX_GMU wake/sleep, GX GDSC waits */
	sgpu_rmw(g->gpucc, GPUCC_CX_GMU_CBCR, 0xff0u, 0xff0u);
	sgpu_rmw(g->gpucc, GPUCC_GX_GDSCR, GDSC_INIT_MASK, GDSC_INIT_VAL);

	/* Power-up order of a6xx_gmu_resume(): CX GDSC, GMU rate, clocks */
	err = cx_gdsc_on(g);
	if (!err) {
		err = gmu_rcg_200mhz(g);
	}
	if (!err) {
		err = branch_enable(g, g->gpucc, GPUCC_AHB_CBCR, false, "gpucc-ahb");
	}
	if (!err) {
		err = branch_enable(g, g->gpucc, GPUCC_CX_GMU_CBCR, true, "cx-gmu");
	}
	if (!err) {
		err = branch_enable(g, g->gpucc, GPUCC_CXO_CBCR, true, "cxo");
	}
	if (!err) {
		err = branch_enable(g, g->gcc, GCC_DDRSS_GPU_AXI_CBCR, true, "ddrss-gpu-axi");
	}
	if (!err) {
		err = branch_enable(g, g->gcc, GCC_GPU_MEMNOC_GFX_CBCR, true, "memnoc-gfx");
	}
	if (!err) {
		/* SMMU "iface" clock */
		err = branch_enable(g, g->gcc, GCC_GPU_SNOC_DVM_GFX_CBCR, true, "snoc-dvm-gfx");
	}
	if (err) {
		return err;
	}
	sgpu_log("clocks on: gmu rcg %#x/%#x (root %s), cx gdsc hw %#x",
	    sgpu_rd(g->gpucc, GPUCC_GMU_CMD_RCGR), sgpu_rd(g->gpucc, GPUCC_GMU_CFG_RCGR),
	    (sgpu_rd(g->gpucc, GPUCC_GMU_CMD_RCGR) & RCG_CMD_ROOT_OFF) ? "OFF" : "on",
	    sgpu_rd(g->gpucc, GPUCC_CX_GDS_HW_CTRL));
	return 0;
}

/*
 * Reverse of the above. The GMU must already have dropped GX; this then
 * forces GX off (as Linux's a6xx_gmu_stop does through the gx genpd) and
 * releases the CPU's CX vote.
 */
void
sgpu_clk_disable(struct sgpu *g)
{
	sgpu_rmw(g->gpucc, GPUCC_GX_GDSCR, 0, GDSC_SW_COLLAPSE);
	if (sgpu_poll(g->gpucc, GPUCC_GX_CFG_GDSCR, GX_CFG_POWER_DOWN_COMPLETE,
	    GX_CFG_POWER_DOWN_COMPLETE, 2000)) {
		sgpu_log("GX GDSC did not report power down (cfg %#x)",
		    sgpu_rd(g->gpucc, GPUCC_GX_CFG_GDSCR));
	}
	sgpu_rmw(g->gpucc, GPUCC_GX_CLAMP_IO, 0, 1u);
	sgpu_rmw(g->gcc, GCC_GPU_SNOC_DVM_GFX_CBCR, CBCR_CLK_ENABLE, 0);
	sgpu_rmw(g->gcc, GCC_GPU_MEMNOC_GFX_CBCR, CBCR_CLK_ENABLE, 0);
	sgpu_rmw(g->gcc, GCC_DDRSS_GPU_AXI_CBCR, CBCR_CLK_ENABLE, 0);
	sgpu_rmw(g->gpucc, GPUCC_CXO_CBCR, CBCR_CLK_ENABLE, 0);
	sgpu_rmw(g->gpucc, GPUCC_CX_GMU_CBCR, CBCR_CLK_ENABLE, 0);
	sgpu_rmw(g->gpucc, GPUCC_AHB_CBCR, CBCR_CLK_ENABLE, 0);
	sgpu_rmw(g->gpucc, GPUCC_CX_GDSCR, 0, GDSC_SW_COLLAPSE);
	sgpu_udelay(500);
}
#endif
