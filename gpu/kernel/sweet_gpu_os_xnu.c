/*
 * sweet_gpu_os_xnu.c - XNU implementation of the sgpu_os_* layer.
 * The only C file of the GPU driver that includes XNU headers; the
 * Linux host test (hosttest/os_host.c) provides the same functions.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include <stdarg.h>

#include "sweet_gpu.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

void
sgpu_log(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("SWEET GPU: %s\n", buf);
}

void
sgpu_udelay(uint32_t us)
{
	delay(us);
}

static int sgpu_sleep_chan;

void
sgpu_msleep(uint32_t ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&sgpu_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

uint64_t
sgpu_now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

volatile uint8_t *
sgpu_ioremap(uint64_t pa, size_t size)
{
	return (volatile uint8_t *)ml_io_map((vm_offset_t)pa, (vm_size_t)size);
}

/* sgpu_dma_alloc/sgpu_dma_free live in sweet_gpu_os_dma.cpp (IOKit). */

void
sgpu_cache_clean(const void *va, size_t len)
{
	if (len) {
		flush_dcache64((addr64_t)(uintptr_t)va, (unsigned)len, FALSE);
	}
}

/*
 * xnu-8796 has no exported invalidate-only primitive; clean+invalidate
 * (DC CIVAC) is equivalent for lines the CPU has not dirtied, which is
 * the only way this driver uses it.
 */
void
sgpu_cache_inval(const void *va, size_t len)
{
	if (len) {
		flush_dcache64((addr64_t)(uintptr_t)va, (unsigned)len, FALSE);
	}
}

void
sgpu_smc_call(struct sgpu_smc *a)
{
	register uint64_t x0 __asm__("x0") = a->x[0];
	register uint64_t x1 __asm__("x1") = a->x[1];
	register uint64_t x2 __asm__("x2") = a->x[2];
	register uint64_t x3 __asm__("x3") = a->x[3];
	register uint64_t x4 __asm__("x4") = a->x[4];
	register uint64_t x5 __asm__("x5") = a->x[5];
	register uint64_t x6 __asm__("x6") = a->x[6];
	register uint64_t x7 __asm__("x7") = a->x[7];

	__asm__ volatile("smc #0"
	    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3),
	    "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
	    :
	    : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
	    "x16", "x17", "memory");
	a->x[0] = x0;
	a->x[1] = x1;
	a->x[2] = x2;
	a->x[3] = x3;
	a->x[4] = x4;
	a->x[5] = x5;
	a->x[6] = x6;
	a->x[7] = x7;
}

bool
sgpu_bootarg_u32(const char *name, uint32_t *out)
{
	return PE_parse_boot_argn(name, out, sizeof(*out)) ? true : false;
}

bool
sgpu_dt_prop(const char *prop, const void **data, uint32_t *len)
{
	DTEntry node;
	unsigned int size = 0;

	if (SecureDTLookupEntry(NULL, "/sweet-gpu", &node) != kSuccess ||
	    SecureDTGetProperty(node, prop, data, &size) != kSuccess) {
		return false;
	}
	*len = size;
	return true;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
