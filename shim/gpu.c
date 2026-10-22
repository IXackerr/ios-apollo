/*
 * apollo GPU probe: the Adreno 650 driver (gpu/kernel) run bare metal,
 * MMU off, with its firmware handed over as the ramdisk (shim/build.sh
 * GPUFW=). This file is the driver's OS layer for that setting.
 *
 * The log goes to the screen and to the pstore console zone the stock
 * recovery kernel reserves (ramoops at 0xb0000000, 2 MiB console zone,
 * no ECC). DDR survives the warm reset into recovery, which then shows
 * the log as /sys/fs/pstore/console-ramoops-0, so a run needs no photo.
 * A few bits flip across the reset, so the log is stored three times,
 * PSTORE_COPY bytes apart, and tools/gpu-log.py takes a bytewise vote.
 */
#include <stdarg.h>
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "gpu.h"
#include "sweet_gpu.h"

#define PSTORE_BASE	0xb0000000UL
#define PSTORE_SIZE	0x200000UL
#define PSTORE_SIG	0x43474244U	/* "DBGC", persistent_ram_buffer */
#define PSTORE_COPY	0x60000U

/* Free RAM well above anything ABL loads and below the XNU payload's top. */
#define DMA_BASE	0x1c0000000UL
#define DMA_SIZE	0x4000000UL

#define FW_MAGIC	0x57464753U	/* "SGFW", tools/mkgpufw.py */
#define FW_NAME_LEN	24

#define LOG_SCALE	2
#define LOG_LINE_H	(16 * LOG_SCALE + 2)
#define LOG_X		12

#define BG		0xff101418U
#define FG		0xffe8eaedU
#define ERR		0xffff6060U

static uint64_t rd_start, rd_end;
static uint64_t dma_next;
static int log_y;
static uint32_t pstore_len;
static struct sgpu gpu;

static inline uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v) :: "memory");
	return v;
}

static inline uint64_t freq(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

/* The compiler may turn loops into calls to these, and the driver uses
 * memset for structure initialisation. */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memset(void *d, int c, size_t n)
{
	uint8_t *p = d;
	while (n--)
		*p++ = (uint8_t)c;
	return d;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *d, const void *s, size_t n)
{
	uint8_t *p = d;
	const uint8_t *q = s;
	while (n--)
		*p++ = *q++;
	return d;
}

/* ---- formatting: the subset of printf the driver uses ---- */

struct out {
	char *buf;
	size_t size, len;
};

static void put(struct out *o, char c)
{
	if (o->len + 1 < o->size)
		o->buf[o->len] = c;
	o->len++;
}

static void put_num(struct out *o, uint64_t v, unsigned base, int width, int zero, int alt)
{
	char tmp[24];
	int n = 0;

	do {
		unsigned d = (unsigned)(v % base);
		tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		v /= base;
	} while (v);
	if (alt) {
		put(o, '0');
		put(o, 'x');
		width -= 2;
	}
	for (int i = n; i < width; i++)
		put(o, zero ? '0' : ' ');
	while (n)
		put(o, tmp[--n]);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct out o = { buf, size, 0 };

	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			put(&o, *fmt);
			continue;
		}
		int alt = 0, zero = 0, width = 0, lng = 0;
		fmt++;
		if (*fmt == '#') {
			alt = 1;
			fmt++;
		}
		if (*fmt == '0') {
			zero = 1;
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		while (*fmt == 'l' || *fmt == 'z') {
			lng = 1;
			fmt++;
		}
		switch (*fmt) {
		case 'd': {
			int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
			if (v < 0) {
				put(&o, '-');
				v = -v;
			}
			put_num(&o, (uint64_t)v, 10, width, zero, 0);
			break;
		}
		case 'u':
			put_num(&o, lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, width, zero, 0);
			break;
		case 'x':
		case 'p':
			put_num(&o, lng || *fmt == 'p' ? va_arg(ap, uint64_t) : va_arg(ap, unsigned),
				16, width, zero, alt || *fmt == 'p');
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			while (s && *s)
				put(&o, *s++);
			break;
		}
		case 'c':
			put(&o, (char)va_arg(ap, int));
			break;
		case '%':
			put(&o, '%');
			break;
		default:
			put(&o, '?');
			if (!*fmt)
				fmt--;
			break;
		}
	}
	if (size)
		buf[o.len < size ? o.len : size - 1] = 0;
	return (int)o.len;
}

static int snprintf_(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

/* ---- log: screen and pstore ---- */

static void pstore_put(const char *s)
{
	volatile uint32_t *hdr = (volatile uint32_t *)PSTORE_BASE;
	volatile uint8_t *data = (volatile uint8_t *)(PSTORE_BASE + 12);

	for (; *s && pstore_len < PSTORE_COPY; s++, pstore_len++)
		for (unsigned i = 0; i < 3; i++)
			data[i * PSTORE_COPY + pstore_len] = (uint8_t)*s;
	hdr[1] = 2 * PSTORE_COPY + pstore_len;	/* start: nothing wrapped */
	hdr[2] = 2 * PSTORE_COPY + pstore_len;	/* size */
	hdr[0] = PSTORE_SIG;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void screen_line(const char *s, uint32_t fg)
{
	int cols = (con_w - 2 * LOG_X) / (8 * LOG_SCALE);
	char line[160];

	while (*s) {
		int n = 0;
		while (s[n] && n < cols && n < (int)sizeof(line) - 1) {
			line[n] = s[n];
			n++;
		}
		line[n] = 0;
		s += n;
		if (log_y + LOG_LINE_H > con_h)
			log_y = 0;
		con_rect(0, log_y, con_w, LOG_LINE_H * 2, BG);
		con_text(LOG_X, log_y, LOG_SCALE, line, fg, BG);
		log_y += LOG_LINE_H;
	}
}

static void log_line(const char *s, uint32_t fg)
{
	char stamp[24];
	uint64_t us = sgpu_now_us();

	snprintf_(stamp, sizeof(stamp), "[%5u.%06u] ", (unsigned)(us / 1000000), (unsigned)(us % 1000000));
	pstore_put(stamp);
	pstore_put(s);
	pstore_put("\n");
	screen_line(s, fg);
}

void sgpu_log(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	log_line(buf, FG);
}

/* ---- the rest of the OS layer ---- */

uint64_t sgpu_now_us(void)
{
	return ticks() * 1000000 / freq();
}

void sgpu_udelay(uint32_t us)
{
	uint64_t end = ticks() + freq() * us / 1000000 + 1;
	while (ticks() < end)
		;
}

void sgpu_msleep(uint32_t ms)
{
	sgpu_udelay(ms * 1000);
}

volatile uint8_t *sgpu_ioremap(uint64_t pa, size_t size)
{
	(void)size;
	return (volatile uint8_t *)pa;
}

/* MMU off: every access is Device memory, so the DMA buffers are
 * uncached and need no maintenance. Nothing is ever freed. */
int sgpu_dma_alloc(size_t size, size_t align, uint32_t flags, struct sgpu_dma *out)
{
	(void)flags;
	if (align < 0x1000)
		align = 0x1000;
	uint64_t pa = (dma_next + align - 1) & ~(uint64_t)(align - 1);
	size = (size + 0xfff) & ~(size_t)0xfff;
	if (pa + size > DMA_BASE + DMA_SIZE)
		return -12;
	for (volatile uint64_t *p = (volatile uint64_t *)pa; p < (volatile uint64_t *)(pa + size); p++)
		*p = 0;
	dma_next = pa + size;
	*out = (struct sgpu_dma){ .va = (void *)pa, .pa = pa, .size = size };
	return 0;
}

void sgpu_dma_free(struct sgpu_dma *dma)
{
	(void)dma;
}

void sgpu_cache_clean(const void *va, size_t len)
{
	(void)va;
	(void)len;
}

void sgpu_cache_inval(const void *va, size_t len)
{
	(void)va;
	(void)len;
}

void sgpu_smc_call(struct sgpu_smc *a)
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
}

bool sgpu_bootarg_u32(const char *name, uint32_t *out)
{
	(void)name;
	(void)out;
	return false;
}

/* Firmware container: "SGFW", count, then count entries of
 * { char name[24]; u32 offset; u32 length } and the blobs. */
bool sgpu_dt_prop(const char *prop, const void **data, uint32_t *len)
{
	const volatile uint32_t *h = (const volatile uint32_t *)rd_start;

	if (rd_end - rd_start < 8 || h[0] != FW_MAGIC)
		return false;
	for (uint32_t i = 0; i < h[1]; i++) {
		const char *name = (const char *)(rd_start + 8 + 32 * i);
		const volatile uint32_t *e = (const volatile uint32_t *)(name + FW_NAME_LEN);
		int j = 0;
		while (j < FW_NAME_LEN && name[j] && name[j] == prop[j])
			j++;
		if (j < FW_NAME_LEN && (name[j] || prop[j]))
			continue;
		if (e[0] + (uint64_t)e[1] > rd_end - rd_start)
			return false;
		*data = (const void *)(rd_start + e[0]);
		*len = e[1];
		return true;
	}
	return false;
}

/* ---- the probe ---- */

void gpu_exception(uint64_t esr, uint64_t elr, uint64_t far)
{
	char buf[128];
	snprintf_(buf, sizeof(buf), "EXCEPTION esr %#lx elr %#lx far %#lx", esr, elr, far);
	log_line(buf, ERR);
}

int test_gpu(uint64_t dtb, unsigned stage)
{
	char buf[128];

	pstore_len = 0;
	for (volatile uint64_t *p = (volatile uint64_t *)(PSTORE_BASE + 12 - 4);
	     p < (volatile uint64_t *)(PSTORE_BASE + 12 + 3 * PSTORE_COPY); p++)
		*p = 0;
	log_y = 0;
	dma_next = DMA_BASE;
	con_rect(0, 0, con_w, con_h, BG);
	log_line("SWEET GPU shim probe", FG);
	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end)) {
		log_line("no initrd in /chosen", ERR);
		return -2;
	}
	snprintf_(buf, sizeof(buf), "firmware container %#lx..%#lx, el%u", rd_start, rd_end,
		  (unsigned)({ uint64_t e; __asm__("mrs %0, CurrentEL" : "=r"(e)); e >> 2; }));
	log_line(buf, FG);

	int err = sgpu_bringup(&gpu, stage);
	sgpu_smmu_check_faults(&gpu);
	snprintf_(buf, sizeof(buf), "result %d, reached stage %u of %u, %u faults", err,
		  gpu.stage_reached, stage, gpu.faults);
	log_line(buf, err ? ERR : FG);
	return err;
}
