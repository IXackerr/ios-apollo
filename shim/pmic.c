/*
 * apollo PMIC probe, read only: SPMI arbiter v5 observer reads of the
 * WLED backlight (PM8150L, SID 5), the fuel gauge and charger (PM8150B,
 * SID 2), plus the arbiter's owner field for each peripheral, which says
 * whether this execution environment could write it.
 */
#include <stdint.h>
#include "console.h"
#include "pmic.h"

#define ARB_CORE	0x0c440000UL
#define ARB_CNFG	0x0c40a000UL
#define ARB_OBS		0x0e600000UL
#define ARB_CHNLS	0x0c600000UL
#define NO_APID		0xffff

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static unsigned apids;

static unsigned find_apid(unsigned ppid)
{
	for (unsigned i = 0; i < apids; i++) {
		uint32_t map = rd(ARB_CORE + 0x900 + 4 * i);
		if (map && ((map >> 8) & 0xfff) == ppid)
			return i;
	}
	return NO_APID;
}

/* One-byte EXT_READL through the observer; returns -1 on any failure. */
static int spmi_rd(unsigned sid, unsigned addr)
{
	unsigned apid = find_apid(sid << 8 | addr >> 8);
	if (apid == NO_APID)
		return -1;
	uintptr_t ch = ARB_OBS + 0x80 * apid;
	wr(ch, 1u << 27 | (addr & 0xff) << 4);
	for (int i = 0; i < 100000; i++) {
		uint32_t st = rd(ch + 0x08);
		if (st & 1)
			return st & 0x0e ? -1 : (int)(rd(ch + 0x18) & 0xff);
	}
	return -1;
}

/*
 * One-byte EXT_WRITEL on the APID's channel page. Only used to write a
 * register back with the value just read from it.
 */
static void write_back(unsigned sid, unsigned addr)
{
	unsigned apid = find_apid(sid << 8 | addr >> 8);
	int v = spmi_rd(sid, addr);
	if (apid == NO_APID || v < 0) {
		con_str("WRITE", "no apid / read failed");
		return;
	}
	uintptr_t ch = ARB_CHNLS + 0x10000 * apid;
	uint32_t before = rd(ch + 0x08), st = 0;
	wr(ch + 0x10, (uint32_t)v);
	wr(ch, (addr & 0xff) << 4);
	int i;
	for (i = 0; i < 100000; i++) {
		st = rd(ch + 0x08);
		if (st & 1)
			break;
	}
	con_hex2("WRITE", "addr=", addr << 16 | (unsigned)v, "apid=", apid);
	con_hex2("  status", "before=", before, "after=", st);
	con_hex2("  status", "polls=", (uint32_t)i, "readback=", (uint32_t)spmi_rd(sid, addr));
}

static void pair(const char *label, unsigned sid, unsigned a, unsigned b)
{
	int va = spmi_rd(sid, a), vb = b ? spmi_rd(sid, b) : 0;
	con_hex2(label, "a=", (uint32_t)a << 16 | (uint16_t)va, "b=", b ? (uint32_t)b << 16 | (uint16_t)vb : 0);
}

static void owner(const char *label, unsigned ppid)
{
	unsigned apid = find_apid(ppid);
	con_hex2(label, "apid=", apid, "owner=", apid == NO_APID ? 0xff : rd(ARB_CNFG + 0x700 + 4 * apid) & 7);
}

void test_pmic(void)
{
	static const uint16_t wled[] = {
		0xd804, 0xd805, 0xd807, 0xd808, 0xd810, 0xd846, 0xd84c, 0xd84d,
		0xd84e, 0xd8ef, 0xd946, 0xd947, 0xd950, 0xd951, 0xd952, 0xd953,
		0xd954, 0xd965, 0xd972, 0xd982, 0xd992, 0xd9b0,
	};
	static const uint16_t batt[] = {
		0x4005, 0x4105, 0x4205, 0x4305, 0x4009, 0x400a, 0x4010, 0x4210,
		0x41a0, 0x41a1, 0x41a2, 0x41a3, 0x41a6, 0x41a7, 0x41a8, 0x41a9,
		0x4288, 0x4289, 0x4150, 0x4151,
		0x1006, 0x1007, 0x100d, 0x110b, 0x1210, 0x1310, 0x1651, 0x1653,
	};

	uint32_t ver = rd(ARB_CORE);
	apids = rd(ARB_CORE + 4) & 0x7ff;
	if (apids > 512)
		apids = 512;
	con_hex2("SPMI", "arb=", ver, "apids=", apids);
	pair("REVID", 2, 0x0105, 0);
	pair("REVID", 4, 0x0103, 0x0105);
	owner("WLED d8", 0x5d8);
	owner("WLED d9", 0x5d9);
	owner("FG 40", 0x240);
	owner("FG 41", 0x241);
	owner("CHG 10", 0x210);
	con_skip();
	for (unsigned i = 0; i < sizeof(wled) / sizeof(wled[0]); i += 2)
		pair("SID5", 5, wled[i], wled[i + 1]);
	con_skip();
	for (unsigned i = 0; i < sizeof(batt) / sizeof(batt[0]); i += 2)
		pair("SID2", 2, batt[i], batt[i + 1]);
	con_skip();
	write_back(5, 0xd953);
}
