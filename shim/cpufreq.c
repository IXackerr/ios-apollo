/*
 * apollo CPU frequency probe, read only: the three EPSS frequency domains
 * (qcom,cpufreq-hw-epss in the phone's device tree), their lookup tables
 * and the level the bootloader left each domain at.
 */
#include <stdint.h>
#include "console.h"
#include "cpufreq.h"

#define EPSS_ENABLE		0x000
#define EPSS_DOMAIN_STATE	0x020
#define EPSS_FREQ_LUT		0x100
#define EPSS_PERF_STATE		0x320
#define LUT_ROWS		40
#define XO_KHZ			19200

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }

static uint32_t lut_mhz(uint32_t row)
{
	return (row & 0xff) * XO_KHZ / 1000;
}

void test_cpufreq(void)
{
	static const uintptr_t domains[3] = { 0x18591000, 0x18592000, 0x18593000 };

	for (unsigned d = 0; d < 3; d++) {
		uintptr_t base = domains[d];
		uint32_t state = rd(base + EPSS_DOMAIN_STATE);
		uint32_t perf = rd(base + EPSS_PERF_STATE);
		unsigned rows = 0;
		uint32_t prev = 0, lo = 0, hi = 0;

		for (unsigned i = 0; i < LUT_ROWS; i++) {
			uint32_t mhz = lut_mhz(rd(base + EPSS_FREQ_LUT + 4 * i));
			if (!mhz || (i && mhz == prev))
				break;
			if (!rows)
				lo = mhz;
			hi = mhz;
			prev = mhz;
			rows++;
		}
		con_hex2(d == 0 ? "EPSS0" : d == 1 ? "EPSS1" : "EPSS2",
			 "en=", rd(base + EPSS_ENABLE), "perf=", perf);
		con_hex2("  state", "raw=", state, "cur_mhz=", lut_mhz(state));
		con_hex2("  lut", "rows=", rows, "perf_mhz=",
			 perf < rows ? lut_mhz(rd(base + EPSS_FREQ_LUT + 4 * perf)) : 0);
		con_hex2("  lut", "min_mhz=", lo, "max_mhz=", hi);
		con_skip();
	}
}
