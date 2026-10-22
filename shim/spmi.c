/*
 * SPMI PMIC arbiter v5, read side: the observer channel of each APID.
 */
#include <stdint.h>
#include "spmi.h"

#define ARB_CORE	0x0c440000UL
#define ARB_OBS		0x0e600000UL
#define NO_APID		0xffff

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

unsigned spmi_apid(unsigned ppid)
{
	unsigned apids = rd(ARB_CORE + 4) & 0x7ff;

	if (apids > 512)
		apids = 512;
	for (unsigned i = 0; i < apids; i++) {
		uint32_t map = rd(ARB_CORE + 0x900 + 4 * i);
		if (map && ((map >> 8) & 0xfff) == ppid)
			return i;
	}
	return NO_APID;
}

int spmi_rd(unsigned sid, unsigned addr)
{
	unsigned apid = spmi_apid(sid << 8 | addr >> 8);
	if (apid == NO_APID)
		return -1;
	uintptr_t ch = ARB_OBS + 0x80 * apid;
	wr(ch, 1u << 27 | (addr & 0xff) << 4);	/* EXT_READL, one byte */
	for (int i = 0; i < 100000; i++) {
		uint32_t st = rd(ch + 0x08);
		if (st & 1)
			return st & 0x0e ? -1 : (int)(rd(ch + 0x18) & 0xff);
	}
	return -1;
}
