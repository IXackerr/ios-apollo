/*
 * sweet_gpu_cmddb.c - read-only RPMh command DB lookups for the GMU.
 *
 * The GMU votes for the GX/CX/MX rails itself (through its own RSC, see
 * sweet_gpu_gmu.c), but the vote words it is handed in the HFI perf table
 * are built from the ARC "hlvl -> vlvl" tables that only the command DB
 * knows. Same data structure as Linux drivers/soc/qcom/cmd-db.c:
 *
 *   header (144 bytes): u32 version, u8 magic[4] = {0xdb,0x30,0x03,0x0c},
 *     rsc_hdr[8] (16 bytes: u16 slv_id, header_offset, data_offset, cnt,
 *     version, reserved[3]), u32 checksum, u32 reserved
 *   entry (24 bytes): u8 id[8], u32 priority[2], u32 addr, u16 len, u16 offset
 *   entry i of slave s: 144 + hdr.header_offset + 24*i
 *   aux data of entry:  144 + hdr.data_offset + entry.offset, entry.len bytes
 *   slave ids: 3 ARC, 4 VRM, 5 BCM
 *
 * SM8250 places the DB directly in reserved memory (stock DT:
 * reserved-memory@80860000 "qcom,cmd-db", 0x20000). (sweet_rpmh.c on
 * SM7150 reads a locator at 0x0c3f000c instead; do not reuse that here.)
 * Mapped as Device memory, so only aligned 32-bit loads.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(SGPU_HOST)
#include "sweet_gpu.h"

#define CMDDB_PA        0x80860000ull
#define CMDDB_SIZE      0x20000u
#define CMDDB_HEADER    144u
#define CMDDB_ENTRY     24u
#define CMDDB_MAGIC     0x0c0330dbu     /* bytes db 30 03 0c, little endian */
#define CMDDB_SLV_ARC   3u

static volatile uint8_t *db;

static uint32_t
db32(uint32_t off)
{
	return sgpu_rd(db, off & ~3u);
}

static uint16_t
db16(uint32_t off)
{
	return (uint16_t)(db32(off) >> (8u * (off & 2u)));
}

static int
db_map(void)
{
	if (db) {
		return 0;
	}
	db = sgpu_ioremap(CMDDB_PA, CMDDB_SIZE);
	if (!db) {
		return -12;
	}
	if (db32(4) != CMDDB_MAGIC) {
		sgpu_log("cmd-db: bad magic %#x at %#llx", db32(4), (unsigned long long)CMDDB_PA);
		db = NULL;
		return -22;
	}
	return 0;
}

/* Find entry by name: returns entry offset and its slave header offset. */
static int
db_find(const char *res, uint32_t *entry, uint32_t *hdr)
{
	uint32_t key[2] = { 0, 0 };
	int err = db_map();

	if (err) {
		return err;
	}
	for (unsigned i = 0; i < 8 && res[i]; i++) {
		key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8u * (i % 4));
	}
	for (unsigned s = 0; s < 8; s++) {
		uint32_t h = 8u + 16u * s;
		uint16_t slv = db16(h), cnt = db16(h + 6);
		if (slv == 0) {
			continue;
		}
		uint32_t base = CMDDB_HEADER + db16(h + 2);
		if (base + (uint32_t)cnt * CMDDB_ENTRY > CMDDB_SIZE) {
			return -22;
		}
		for (unsigned j = 0; j < cnt; j++) {
			uint32_t e = base + CMDDB_ENTRY * j;
			if (db32(e) == key[0] && db32(e + 4) == key[1]) {
				*entry = e;
				*hdr = h;
				return 0;
			}
		}
	}
	return -2;
}

int
sgpu_cmddb_addr(const char *res, uint32_t *addr)
{
	uint32_t e, h;
	int err = db_find(res, &e, &h);

	if (!err) {
		*addr = db32(e + 16);
	}
	return err;
}

int
sgpu_cmddb_arc(const char *res, struct sgpu_arc *out)
{
	uint32_t e, h;
	int err = db_find(res, &e, &h);

	if (err) {
		sgpu_log("cmd-db: %s not found (%d)", res, err);
		return err;
	}
	if (db16(h) != CMDDB_SLV_ARC) {
		return -22;
	}
	uint32_t len = db16(e + 20);
	uint32_t aux = CMDDB_HEADER + db16(h + 4) + db16(e + 22);
	if (len == 0 || (len & 1) || aux + len > CMDDB_SIZE) {
		return -22;
	}
	out->addr = db32(e + 16);
	out->count = len / 2;
	if (out->count > SGPU_ARC_MAX_LEVELS) {
		out->count = SGPU_ARC_MAX_LEVELS;
	}
	for (uint32_t i = 0; i < out->count; i++) {
		out->level[i] = db16(aux + 2 * i);
	}
	sgpu_log("cmd-db: %s addr %#x, %u levels, first %u last %u", res, out->addr,
	    out->count, out->level[0], out->level[out->count - 1]);
	return 0;
}
#endif
