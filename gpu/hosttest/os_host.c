/*
 * os_host.c - sgpu_os_* for the Linux host test (time is simulated:
 * sgpu_udelay advances model_now_us, so 1 s timeouts cost no real time).
 * MMIO, DMA memory and SMC are provided by model.c.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "model.h"
#include "../kernel/sweet_gpu.h"

int host_verbose = 1;
bool host_dt_zap = true;

void
sgpu_log(const char *fmt, ...)
{
	va_list ap;
	if (!host_verbose) {
		return;
	}
	va_start(ap, fmt);
	printf("  [%8llu us] ", (unsigned long long)model_now_us);
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
}

void sgpu_udelay(uint32_t us) { model_now_us += us; model_tick(); }
void sgpu_msleep(uint32_t ms) { model_now_us += 1000ull * ms; model_tick(); }
uint64_t sgpu_now_us(void) { return model_now_us; }
void sgpu_cache_clean(const void *va, size_t len) { (void)va; (void)len; }
void sgpu_cache_inval(const void *va, size_t len) { (void)va; (void)len; }

static struct { char name[48]; uint32_t v; } bootargs[16];
static int nbootargs;

void
host_set_bootarg(const char *name, uint32_t v)
{
	snprintf(bootargs[nbootargs].name, sizeof(bootargs[0].name), "%s", name);
	bootargs[nbootargs++].v = v;
}

void
host_clear_bootargs(void)
{
	nbootargs = 0;
}

bool
sgpu_bootarg_u32(const char *name, uint32_t *out)
{
	for (int i = 0; i < nbootargs; i++) {
		if (!strcmp(bootargs[i].name, name)) {
			*out = bootargs[i].v;
			return true;
		}
	}
	return false;
}

bool
sgpu_dt_prop(const char *prop, const void **data, uint32_t *len)
{
	static const uint64_t zap_region[2] = { 0x8681a000ull, 0x2000ull };
	static const uint32_t chip = 0x06050002;

	if (!strcmp(prop, "sqe-firmware")) {
		*data = fw_sqe; *len = fw_sqe_len; return true;
	}
	if (!strcmp(prop, "gmu-firmware")) {
		*data = fw_gmu; *len = fw_gmu_len; return true;
	}
	if (!strcmp(prop, "zap-firmware") && host_dt_zap) {
		*data = fw_zap; *len = fw_zap_len; return true;
	}
	if (!strcmp(prop, "zap-region")) {
		*data = zap_region; *len = sizeof(zap_region); return true;
	}
	if (!strcmp(prop, "chip-id")) {
		*data = &chip; *len = 4; return true;
	}
	return false;
}
