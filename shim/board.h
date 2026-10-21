#pragma once
/*
 * Per-board constants of the boot shim. Build with BOARD=apollo
 * (shim/build.sh) for the Xiaomi Mi 10T / 10T Pro; the default is sweet.
 *
 * Sources:
 *   sweet  - live FDT of the Redmi Note 10 Pro (NOTES.md)
 *   apollo - stock kona DTB from the phone's boot.img and its live FDT;
 *            mainline sm8250.dtsi + sm8250-xiaomi-apollo.dts
 *            (github.com/royka1/apollo-linux) for the rest
 */

#if defined(BOARD_APOLLO)

#define BOARD_NAME		"apollo (SM8250)"
/* sm8250.dtsi timer: <GIC_PPI 13>, <14>, <11>, <10> -> phys 30, virt 27.
 * sweet (SM7150) used PPI 2/3 -> 18/19. The probe tests both pairs. */
#define VTIMER_INTID		27
#define PTIMER_INTID		30
#define ALT_VTIMER_INTID	19	/* sweet numbering, tested as a fallback */
/* intc@17a00000: GICD 0x17a00000, GICR 0x17a60000 (8 x 0x20000) */
#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17a60000UL
/* qcom,msm-imem@146bf000 restart_reason@65c and restart@c264000 (pshold).
 * ABL reads the same magic as on sweet and stays in fastboot. */
#define HAVE_IMEM_RESTART	1
#define IMEM_RESTART_REASON	0x146bf65cUL
#define RESTART_BOOTLOADER	0x77665500U
#define HAVE_PSHOLD		1
#define PSHOLD			0x0c264000UL
/* qcom,wdt@17c10000, kpss register layout (DIAG=3) */
#define APSS_WDT		0x17c10000UL
/* cpu@0..@300 Cortex-A55, cpu@400..@600 Cortex-A77, cpu@700 A77 prime */
#define NR_CPUS			8

#else /* sweet */

#define BOARD_NAME		"sweet (SM7150)"
#define VTIMER_INTID		19	/* DT: <GIC_PPI 3 ...>, not the usual 27 */
#define PTIMER_INTID		18	/* DT: <GIC_PPI 2 ...> */
#define ALT_VTIMER_INTID	27
#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17a60000UL
#define HAVE_IMEM_RESTART	1
#define IMEM_RESTART_REASON	0x146aa65cUL	/* qcom,msm-imem restart_reason@65c */
#define RESTART_BOOTLOADER	0x77665500U
#define HAVE_PSHOLD		1
#define PSHOLD			0x0c264000UL
#define NR_CPUS			8

#endif
