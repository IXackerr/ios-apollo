/*
 * Milestone 2: one boot answers the questions that decide the XNU port.
 *   - CPU feature registers (16K granule, GIC sysreg interface)
 *   - PSCI/SMCCC version and which PSCI calls the Qualcomm TZ offers
 *   - SMP: PSCI CPU_ON for the 7 secondaries, each reports its MPIDR
 *   - GICv3 Group 1 IRQ from the virtual timer (INTID 19 on Qualcomm)
 * Any unexpected exception paints a blue panic screen.
 */
#include <stdint.h>
#include "console.h"
#include "mmu.h"
#include "loader.h"
#include "ufs.h"
#include "touch.h"
#include "gpu.h"
#include "cpufreq.h"
#include "pmic.h"
#include "menu.h"
#include "board.h"

#define GICR_SIZE		0x100000UL
#define GICR_PE_SIZE		0x20000UL
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_ICFGR1		(GICR_SGI + 0xc04)
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_OFF		0x84000008UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];

static const char *stage;	/* set at runtime: no absolute pointers in data */
static volatile uint64_t sec_mpidr[NR_CPUS];
static volatile uint64_t sec_midr[NR_CPUS], sec_mmfr0[NR_CPUS], sec_isar0[NR_CPUS];
static volatile uint64_t sec_probe[4];	/* CPU1 redistributor write test */
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

/* ICC_SGI1R_EL1 value targeting the calling core only. */
static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 |	/* Aff2 */
	       ((m >> 8) & 0xff) << 16 |	/* Aff1 */
	       (uint64_t)id << 24 |
	       (1U << (m & 0xf));		/* target list: Aff0 */
}

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_skip();
	con_str("RESET", after_s > 20 ? "in 40s" : "in 20s");
	delay_ms(after_s * 1000);
#if HAVE_IMEM_RESTART
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
#endif
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
#if HAVE_PSHOLD
	wr32(PSHOLD, 0);
#endif
	for (;;)
		__asm__ volatile("wfe");
}

/* Reboot at once into the bootloader mode `reason` selects (IMEM magic). */
static void __attribute__((noreturn)) reboot_now(uint32_t reason)
{
#if HAVE_IMEM_RESTART
	wr32(IMEM_RESTART_REASON, reason);
	__asm__ volatile("dsb sy" ::: "memory");
#else
	(void)reason;
#endif
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions ---- */

static void irq_handler(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID || id == ALT_VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	con_clear(BLUE);
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
#if defined(GPU_PROBE)
	gpu_exception(rsr("esr_el1"), rsr("elr_el1"), rsr("far_el1"));
	delay_ms(10000);
	reboot_now(RESTART_RECOVERY);
#endif
	reset(40);
}

/* ---- SMP ---- */

static uintptr_t find_gicr(void);

void secondary_main(uint64_t idx)
{
	sec_midr[idx] = rsr("midr_el1");
	sec_mmfr0[idx] = rsr("id_aa64mmfr0_el1");
	sec_isar0[idx] = rsr("id_aa64isar0_el1");
	__asm__ volatile("dsb sy" ::: "memory");
	sec_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 63);
	if (idx == 1) {
		uintptr_t r = find_gicr();
		if (r) {
			sec_probe[0] = rd32(r + GICR_IGROUPR0);
			wr32(r + GICR_IGROUPR0, 0xffffffff);
			sec_probe[1] = rd32(r + GICR_IGROUPR0);
			wr8(r + GICR_IPRIORITYR + 19, 0xa0);
			sec_probe[2] = rd32(r + GICR_IPRIORITYR + 16);
		}
		sec_probe[3] = r ? 1 : 0xdead;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

/*
 * Per-core features that decide the iOS port: 16K granule (TGran16,
 * ID_AA64MMFR0[23:20], 1 or 2 = supported) and LSE atomics (ID_AA64ISAR0
 * [23:20] = 2), which the A11 userland uses. "f=" packs tg16<<4 | atomic.
 */
static uint32_t feat(uint64_t mmfr0, uint64_t isar0)
{
	return (uint32_t)(((mmfr0 >> 20) & 0xf) << 4 | ((isar0 >> 20) & 0xf));
}

static void test_smp(void)
{
	stage = "smp";
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	int64_t ret[NR_CPUS];

	con_hex2("CPU0", "midr=", (uint32_t)rsr("midr_el1"), "f=",
		 feat(rsr("id_aa64mmfr0_el1"), rsr("id_aa64isar0_el1")));
	for (int i = 1; i < NR_CPUS; i++)
		ret[i] = smc(PSCI_CPU_ON, (uint64_t)i << 8, (uintptr_t)secondary_entry, i);
	delay_ms(500);
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (((uint64_t)i << 8) == self) {
			con_str(label, "is boot cpu");
			continue;
		}
		if (!sec_mpidr[i]) {
			con_hex2(label, "ret=", (uint32_t)ret[i], "mpidr=", 0xdead0000);
			continue;
		}
		con_hex2(label, "midr=", (uint32_t)sec_midr[i], "f=",
			 feat(sec_mmfr0[i], sec_isar0[i]));
	}
}

/* ---- GIC + timer ---- */

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void gicd_wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
}

/* Run one interrupt source for ~300 ms and report what the GIC saw. */
static void irq_window(const char *label, uintptr_t r, int src)
{
	uint64_t f = rsr("cntfrq_el0");
	uint32_t pend = 0, act = 0, isr = 0, hppi = 0x3ff, ist = 0;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	} else if (src == PTIMER_INTID) {
		wsr("cntp_tval_el0", f / 100);
		wsr("cntp_ctl_el0", 1);
	}
	isb();
	/* sample state with IRQs still masked, after the first period */
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
	ist = src == PTIMER_INTID ? rsr("cntp_ctl_el0") : rsr("cntv_ctl_el0");
	pend = rd32(r + GICR_ISPENDR0);
	act = rd32(r + GICR_ISACTIVER0);
	isr = rsr("isr_el1");
	hppi = rsr("icc_hppir1_el1");

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	/* SGI: one already sent above, send 9 more 10 ms apart -> expect 10 */
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	wsr("cntp_ctl_el0", 0);
	isb();

	con_hex2(label, "irqs=", irq_count, "other=", irq_other);
	con_hex2(" pre", "pend=", pend, "act=", act);
	con_hex2(" pre", "isr=", isr, "hppi=", hppi);
	con_hex2(" pre", "tctl=", ist, "last=", irq_last);
}

static void test_gic(void)
{
	stage = "gic sre";
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg iface, skip");
		return;
	}

	stage = "gic dist";
	uint32_t dctlr = rd32(GICD_BASE);
	/* Non-secure view (DS=0): bit0 EnableGrp1, bit1 EnableGrp1A, bit4 ARE_NS; Linux writes 0x13 */
	wr32(GICD_BASE, dctlr | 0x13);
	gicd_wait_rwp();
	con_hex2("GICD", "was=", dctlr, "now=", rd32(GICD_BASE));

	stage = "gic redist";
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	stage = "gic config";
	uint32_t grp = rd32(r + GICR_IGROUPR0);
	wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	uint32_t en_before = rd32(r + GICR_ISENABLER0);
	uint32_t pri_before = rd32(r + GICR_IPRIORITYR + 16);	/* INTIDs 16..19 */
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	/* Both vtimer numberings: the board's and the other Qualcomm one, so a
	 * wrong guess shows up as "last=" instead of a silent hang. */
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << ALT_VTIMER_INTID) |
	     (1U << PTIMER_INTID) | (1U << SGI_INTID));
	con_hex2("ENABLE", "was=", en_before, "now=", rd32(r + GICR_ISENABLER0));
	(void)grp;
	(void)pri_before;

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_hex2("ICC", "ctlr=", ctlr, "grpen1=", rsr("icc_igrpen1_el1"));

	stage = "gic vtimer";
	irq_window("VTIMER", r, VTIMER_INTID);
	stage = "gic sgi";
	irq_window("SGI1", r, SGI_INTID);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	isb();
}

/* ---- main ---- */

static void __attribute__((unused)) psci_line(const char *label, uint64_t fn)
{
	con_hex2(label, "feat=", smc(PSCI_FEATURES, fn, 0, 0), 0, 0);
}

static uint64_t scratch[8192];	/* 64KB, lives in our (later cacheable) image */

static uint64_t ticks_fill(uint32_t c)
{
	uint64_t t = now();
	con_clear(c);
	return now() - t;
}

static uint64_t ticks_mem(void)
{
	uint64_t t = now();
	for (int pass = 0; pass < 16; pass++)
		for (int i = 0; i < 8192; i++)
			scratch[i] = scratch[i] * 3 + i;
	return now() - t;
}

static uint32_t to_us(uint64_t ticks)
{
	return ticks * 10 / 192;	/* 19.2 MHz */
}

#if defined(DIAG)
/*
 * Black-screen diagnostics (BOARD=apollo builds with DIAG=1, 2 or 3).
 * DIAG=1: PSCI SYSTEM_RESET as the very first action. A reboot within
 *         seconds proves ABL started the shim.
 * DIAG=3: reset ladder (PSCI, PSCI RESET2, watchdog bite, PS_HOLD), 3 s apart.
 * DIAG=2: A) fill the whole cont_splash region with 0xff bytes (white in
 *         any RGB format) and hold 15 s; B) read the display engine's
 *         source pipes (DPU SSPP, sm8250 catalog) to find the buffer that
 *         is really scanned out, draw the console there and hold 60 s;
 *         then reset. Reading the MDP hangs if its clocks are off: then
 *         the phone stays at stage A and must be reset by hand.
 */
#if DIAG == 2
#define SPLASH_BASE	0x9c000000UL
#define SPLASH_SIZE	0x2300000UL
#define MDP_BASE	0x0ae01000UL	/* display-controller@ae01000 "mdp" */
#define SSPP_SRC_SIZE	0x00
#define SSPP_SRC0_ADDR	0x14
#define SSPP_YSTRIDE0	0x24
#define SSPP_FORMAT	0x30

static const uint32_t sspp_off[8] = {
	0x4000, 0x6000, 0x8000, 0xa000,		/* VIG0-3 */
	0x24000, 0x26000, 0x28000, 0x2a000,	/* DMA0-3 */
};
/* names as one string: the shim may hold no absolute pointers in data */
static const char sspp_names[] = "VIG0VIG1VIG2VIG3DMA0DMA1DMA2DMA3";

static void sspp_name(int i, char out[5])
{
	for (int k = 0; k < 4; k++)
		out[k] = sspp_names[i * 4 + k];
	out[4] = 0;
}

static void fill64(uintptr_t base, uint64_t size, uint64_t v)
{
	volatile uint64_t *p = (volatile uint64_t *)base;
	for (uint64_t i = 0; i < size / 8; i++)
		p[i] = v;
}

#endif /* DIAG == 2 */

static void __attribute__((noreturn)) diag_main(void)
{
#if DIAG == 1
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
#elif DIAG == 3
	/*
	 * Reset ladder, 3 s apart, so the moment of the reboot tells which
	 * method works (and that the shim runs at all). The IMEM restart
	 * reason is written first, so any of them should end in fastboot:
	 *   ~0 s  PSCI SYSTEM_RESET      ~3 s  PSCI SYSTEM_RESET2 (warm)
	 *   ~6 s  APSS watchdog bite (qcom,wdt@17c10000, kpss layout, as
	 *         Linux qcom_wdt_restart; 32768 Hz sleep clock, 128 ms)
	 *   ~9 s  PS_HOLD low (restart@c264000)
	 * No reboot at all: the shim never ran or hangs in its first steps.
	 */
#if HAVE_IMEM_RESTART
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	__asm__ volatile("dsb sy" ::: "memory");
#endif
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	delay_ms(3000);
	smc(PSCI_SYSTEM_RESET2, 0, 0, 0);
	delay_ms(3000);
#if defined(APSS_WDT)
	wr32(APSS_WDT + 0x08, 0);		/* WDT_EN */
	wr32(APSS_WDT + 0x04, 1);		/* WDT_RST */
	wr32(APSS_WDT + 0x10, 4194);		/* WDT_BARK_TIME */
	wr32(APSS_WDT + 0x14, 4194);		/* WDT_BITE_TIME */
	wr32(APSS_WDT + 0x08, 1);		/* WDT_EN */
	__asm__ volatile("dsb sy" ::: "memory");
#endif
	delay_ms(3000);
#if HAVE_PSHOLD
	wr32(PSHOLD, 0);
	__asm__ volatile("dsb sy" ::: "memory");
#endif
	for (;;)
		__asm__ volatile("wfe");
#else
	uint32_t size[8], addr[8], stride[8], fmt[8];
	char name[5];
	int pick = -1;

	fill64(SPLASH_BASE, SPLASH_SIZE, ~0ULL);
	delay_ms(15000);

	for (int i = 0; i < 8; i++) {
		uintptr_t b = MDP_BASE + sspp_off[i];
		size[i] = rd32(b + SSPP_SRC_SIZE);
		addr[i] = rd32(b + SSPP_SRC0_ADDR);
		stride[i] = rd32(b + SSPP_YSTRIDE0) & 0xffff;
		fmt[i] = rd32(b + SSPP_FORMAT);
		if (pick < 0 && addr[i] && size[i] && stride[i])
			pick = i;
	}

	if (pick >= 0) {
		int w = size[pick] & 0xffff, h = size[pick] >> 16;
		int bpp = ((fmt[pick] >> 9) & 3) + 1;
		uintptr_t a = addr[pick];
		if (bpp == 4 && a >= 0x80000000UL) {
			con_fb = (volatile uint32_t *)a;
			con_w = w;
			con_h = h;
			con_stride = stride[pick];
			con_clear(RED);
			con_str("DIAG", "scanout found");
			sspp_name(pick, name);
			con_str("PIPE", name);
			con_skip();
			for (int i = 0; i < 8; i++) {
				sspp_name(i, name);
				con_hex2(name, "addr=", addr[i], "size=", size[i]);
				con_hex2("", "strd=", stride[i], "fmt=", fmt[i]);
			}
		} else if (a >= 0x80000000UL) {
			/* not 32 bpp: horizontal white/black bands, 64 lines each */
			for (int y = 0; y < h; y++)
				fill64(a + (uint64_t)y * stride[pick], (uint64_t)stride[pick] & ~7UL,
				       (y / 64) & 1 ? 0 : ~0ULL);
		}
	} else {
		/* no active pipe: alternate black/white over the splash region */
		fill64(SPLASH_BASE, SPLASH_SIZE, 0);
		delay_ms(2000);
		fill64(SPLASH_BASE, SPLASH_SIZE, ~0ULL);
	}
	delay_ms(60000);
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
#endif
}
#endif

void shim_main(uint64_t dtb)
{
	struct mmu_info mi;

#if defined(DIAG)
	diag_main();
#endif

	/* An xnu payload in the ramdisk means: boot menu, then boot it (MMU stays off). */
	stage = "menu";
	const char *prefix = "";
	if (loader_has_payload(dtb)) {
		enum menu_action a = boot_menu();
		switch (a) {
		case MENU_FASTBOOT:
			reboot_now(RESTART_BOOTLOADER);
		case MENU_RECOVERY:
			reboot_now(RESTART_RECOVERY);
		case MENU_POWER_OFF:
			smc(PSCI_SYSTEM_OFF, 0, 0, 0);
			reboot_now(RESTART_BOOTLOADER);
		default:
			prefix = menu_boot_args(a);
			con_clear(0xff000000U);
			break;
		}
	} else {
		con_clear(RED);
	}
	stage = "loader";
	con_str("SHIM", "sweet milestone 4: xnu loader");
	con_skip();
	const char *why = loader_boot(dtb, prefix);
	con_str("LOADER", why);
	con_skip();

#if defined(PMIC_PROBE)
	stage = "pmic";
	con_str("BOARD", BOARD_NAME);
	test_pmic();
	con_skip();
	con_str("PMIC", "done, reset in ~2 min");
	delay_ms(120000);
	reset(20);
#endif
#if defined(CPUFREQ_PROBE)
	stage = "cpufreq";
	con_str("BOARD", BOARD_NAME);
	con_skip();
	test_cpufreq();
	con_str("CPUFREQ", "done, reset in ~2 min");
	delay_ms(120000);
	reset(20);
#endif
#if defined(GPU_PROBE)
	/* The log is in pstore: recovery shows it, no photo needed. */
	stage = "gpu";
	test_gpu(dtb, GPU_PROBE);
	delay_ms(30000);
	reboot_now(RESTART_RECOVERY);
#endif
#if defined(TOUCH_PROBE)
	stage = "touch";
	con_str("BOARD", BOARD_NAME);
	test_touch(dtb, 240);
	con_skip();
	con_str("TOUCH", "done, reset in ~3 min");
	delay_ms(180000);
	reset(20);
#endif

	/*
	 * No payload: hardware probe, two screens, MMU off, nothing written
	 * to storage. Page 1: CPU features of every core (PSCI CPU_ON),
	 * PSCI version, GICv3 + virtual timer + SGI. Page 2: internal UFS.
	 */
	con_str("BOARD", BOARD_NAME);
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "smccc=", smc(SMCCC_VERSION, 0, 0, 0));
	con_skip();
	test_smp();
	con_skip();
	test_gic();
	con_skip();
	con_str("PAGE1", "done, UFS page in 60s");
	delay_ms(60000);

	stage = "ufs";
	con_clear(RED);
	con_str("BOARD", BOARD_NAME);
	con_skip();
	test_ufs();
	con_skip();
	/* Keep the probe results on screen; reset after 3 min. */
	con_str("UFS", "probe done, reset in ~3 min");
	delay_ms(180000);
	reset(20);

	stage = "fill off";
	uint64_t fill_off = ticks_fill(RED);
	uint64_t mem_off = ticks_mem();

	stage = "mmu enable";
	int rc = mmu_enable(&mi);

	stage = "fill on";
	uint64_t fill_on = rc ? 0 : ticks_fill(RED);
	uint64_t mem_on = rc ? 0 : ticks_mem();
	if (rc)
		con_clear(RED);

	con_str("SHIM", "sweet milestone 3");
	con_skip();
	con_hex2("DTB", "at=", dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	con_hex2("IMAGE", "lo=", mi.img_lo, "hi=", mi.img_hi);
	con_hex2("MMU", "rc=", rc, "l3=", mi.n_l3);
	con_hex2("TTBR0", "pa=", mi.ttbr, "pa_hi=", mi.ttbr >> 32);
	con_hex("SCTLR0", mi.sctlr_before);
	con_hex("SCTLR1", mi.sctlr_after);
	con_hex("TCR", rsr("tcr_el1"));
	con_hex2("PARANGE", "mmfr0=", rsr("id_aa64mmfr0_el1") & 0xf, "tg16=", (rsr("id_aa64mmfr0_el1") >> 20) & 0xf);
	con_skip();
	con_hex("PAR img", mmu_probe((uintptr_t)shim_main));
	con_hex("PAR fb", mmu_probe(FB_PHYS + 0x1000));
	con_hex("PAR gic", mmu_probe(GICD_BASE));
	con_hex("PAR 12G", mmu_probe(0x300000000UL));
	con_skip();
	con_hex2("FILL us", "off=", to_us(fill_off), "on=", to_us(fill_on));
	con_hex2("MEM us", "off=", to_us(mem_off), "on=", to_us(mem_on));
	con_skip();

	stage = "psci";
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "smccc=", smc(SMCCC_VERSION, 0, 0, 0));
	con_skip();
	test_gic();

	stage = "done";
	reset(40);
}
