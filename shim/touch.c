/*
 * apollo touch probe: Novatek NT36675 on QUP0 SE4 (GENI SPI) with the
 * firmware handed over as the ramdisk (shim/build.sh TOUCHFW=).
 *
 * Polled and bare metal, MMU off: clocks, pins, SPI, reset, chip ID,
 * firmware download, then touch reports drawn on the panel as squares.
 * The IC has no flash; its firmware lives in SRAM and is loaded on every
 * boot. Nothing is written to storage or to any PMIC.
 *
 * Register maps: GCC/TLMM/GENI from the SM8250 device tree and the Linux
 * QUP/GENI headers; the NT36xxx SPI framing and download sequence from the
 * vendor driver, used as protocol documentation only.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "touch.h"

#define GCC		0x00100000UL
#define GCC_VOTE	(GCC + 0x52008)
#define SE4_RCG		(GCC + 0x174d0)
#define QUP0_HW_VER	0x009c0004UL
#define SE		0x00990000UL
#define TLMM_NORTH	0x0f900000UL
#define TLMM_SOUTH	0x0f500000UL

#define PIN_CS		(TLMM_NORTH + 11 * 0x1000)
#define PIN_RST		(TLMM_SOUTH + 38 * 0x1000)
#define PIN_INT		(TLMM_SOUTH + 39 * 0x1000)
#define PIN_CFG		0x0
#define PIN_IO		0x4
#define PIN_INTR_CFG	0x8
#define PIN_INTR_STS	0xc

#define GENI_FORCE_DEFAULT	0x020
#define GENI_OUTPUT_CTRL	0x024
#define GENI_CGC_CTRL		0x028
#define GENI_STATUS		0x040
#define GENI_M_CLK_CFG		0x048
#define GENI_IF_DISABLE		0x064
#define GENI_FW_REV		0x068
#define GENI_CLK_SEL		0x07c
#define SPI_CPHA		0x224
#define SPI_LOOPBACK		0x22c
#define SPI_CPOL		0x230
#define SPI_DEMUX_INV		0x24c
#define SPI_DEMUX_SEL		0x250
#define GENI_BYTE_GRAN		0x254
#define GENI_DMA_MODE		0x258
#define SPI_TRANS_CFG		0x25c
#define GENI_TX_PACK0		0x260
#define GENI_TX_PACK1		0x264
#define SPI_WORD_LEN		0x268
#define SPI_TX_LEN		0x26c
#define SPI_RX_LEN		0x270
#define GENI_RX_PACK0		0x284
#define GENI_RX_PACK1		0x288
#define GENI_M_CMD		0x600
#define GENI_M_CMD_CTRL		0x604
#define GENI_M_IRQ		0x610
#define GENI_M_IRQ_EN		0x614
#define GENI_M_IRQ_CLR		0x618
#define GENI_S_IRQ_EN		0x644
#define GENI_S_IRQ_CLR		0x648
#define GENI_TX_FIFO		0x700
#define GENI_RX_FIFO		0x780
#define GENI_TX_STATUS		0x800
#define GENI_RX_STATUS		0x804
#define GENI_RX_WM		0x810
#define GENI_RX_RFR		0x814
#define SE_DMA_TX_IRQ_CLR	0xc44
#define SE_DMA_RX_IRQ_CLR	0xd44
#define SE_GSI_EVENT_EN		0xe18
#define SE_IRQ_EN		0xe1c
#define SE_HW_PARAM_0		0xe24
#define SE_DMA_GENERAL_CFG	0xe30

#define M_CMD_DONE	(1u << 0)
#define M_ERRORS	0x3300000eu
#define OP_TX_ONLY	1u
#define OP_FULL_DUPLEX	3u
#define FRAGMENTATION	(1u << 2)

/* NT36675 memory map */
#define ENG_RST		0x7fff80u
#define SWRST_N8	0x03f0feu
#define SPI_RD_FAST	0x03f310u
#define TRIM_ID		0x01f64eu
#define TRIM_ID_NEW	0x03f004u
#define BOOT_RDY	0x03f10du
#define ILM_CRC		0x03f100u
#define DLM_CRC		0x03f104u
#define ILM_LEN		0x03f118u
#define ILM_DES		0x03f128u
#define DLM_DES		0x03f12cu
#define DLM_LEN		0x03f130u
#define BLD_CRC_FLAG	0x03f133u
#define BLD_CRC_EN	0x03f30eu
#define EVENT_BUF	0x022d00u
#define HOST_CMD	(EVENT_BUF + 0x50)
#define RESET_STATE	(EVENT_BUF + 0x60)
#define FW_INFO		(EVENT_BUF + 0x78)

#define FW_SIZE		0x22000u
#define CHUNK		64512u
#define POINT_LEN	65u
#define FINGERS		10u

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static const uint8_t *fw;
static unsigned fifo_depth;

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v) :: "memory");
	return v;
}

static uint64_t freq(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static void sleep_us(unsigned us)
{
	uint64_t end = ticks() + freq() / 1000000 * us;
	while (ticks() < end)
		;
}

static int wait_clear(uintptr_t a, uint32_t mask, unsigned us)
{
	for (unsigned i = 0; i < us; i++) {
		if (!(rd(a) & mask))
			return 0;
		sleep_us(1);
	}
	return -1;
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---- clocks and pins ---- */

/* Returns the SE clock level for CLK_SEL, or -1 if 19.2 MHz is not available. */
static int clocks(void)
{
	static const uint32_t branches[] = { 0x17004, 0x17008, 0x23000, 0x23008, 0x174cc };

	wr(GCC_VOTE, rd(GCC_VOTE) | 0x43c0);
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		/* Under hardware gating (HW_CTL) CLK_OFF reads set while idle. */
		if (rd(GCC + branches[i]) & 2)
			continue;
		if (wait_clear(GCC + branches[i], 1u << 31, 1000)) {
			con_hex2("CLK", "halted=", branches[i], "cbcr=", rd(GCC + branches[i]));
			return -1;
		}
	}
	con_hex2("QUP0", "hw_ver=", rd(QUP0_HW_VER), "vote=", rd(GCC_VOTE));

	uint32_t dfs = rd(SE4_RCG + 0x14);
	if (!(dfs & 1)) {
		/* TCXO 19.2 MHz, no divider, no MND */
		wr(SE4_RCG + 4, (rd(SE4_RCG + 4) & ~0x10371fu) | 1);
		wr(SE4_RCG, rd(SE4_RCG) | 1);
		if (wait_clear(SE4_RCG, 1, 1000))
			return -1;
		con_hex2("RCG", "dfs=", dfs, "cfg=", rd(SE4_RCG + 4));
		return 0;
	}
	/* DFS: the SE picks a level; find the plain TCXO one. */
	int pick = -1;
	for (unsigned l = 0; l < 8; l++) {
		uint32_t cfg = rd(SE4_RCG + 0x1c + 4 * l);
		if (pick < 0 && (cfg & 0x371f) <= 1)
			pick = (int)l;
		if (l < 4)
			con_hex2("DFS", "lvl=", l, "cfg=", cfg);
	}
	return pick;
}

static void pins(void)
{
	for (unsigned n = 8; n <= 10; n++)
		wr(TLMM_NORTH + n * 0x1000 + PIN_CFG, 0x084);	/* qup4, 6 mA */
	wr(PIN_CS + PIN_IO, 2);				/* CS high (idle) */
	wr(PIN_CS + PIN_CFG, 0x280);			/* gpio, out, 6 mA */
	wr(PIN_RST + PIN_IO, 0);
	wr(PIN_RST + PIN_CFG, 0x2c3);			/* gpio, out, 8 mA, pull-up */
	wr(PIN_INT + PIN_CFG, 0x1c1);			/* gpio, in, pull-down */
	wr(PIN_INT + PIN_INTR_CFG, 0x76);		/* latch rising edges, no IRQ */
	wr(PIN_INT + PIN_INTR_STS, 0);
}

/* ---- GENI SPI, FIFO mode, polled ---- */

static int spi_init(int clk_sel)
{
	uint32_t proto = (rd(SE + GENI_FW_REV) >> 8) & 0xff;
	uint32_t ifdis = rd(SE + GENI_IF_DISABLE);
	uint32_t hw = rd(SE + SE_HW_PARAM_0);

	fifo_depth = (hw >> 16) & 0x3f;
	con_hex2("SE4", "proto=", proto, "if_dis=", ifdis);
	con_hex2("SE4", "hw0=", hw, "depth=", fifo_depth);
	if (proto != 1 || (ifdis & 1) || fifo_depth < 4)
		return -1;

	wr(SE + SE_GSI_EVENT_EN, 0);
	wr(SE + GENI_M_IRQ_CLR, ~0u);
	wr(SE + GENI_S_IRQ_CLR, ~0u);
	wr(SE + SE_DMA_TX_IRQ_CLR, ~0u);
	wr(SE + SE_DMA_RX_IRQ_CLR, ~0u);
	wr(SE + SE_IRQ_EN, ~0u);
	wr(SE + GENI_CGC_CTRL, rd(SE + GENI_CGC_CTRL) | 0x7f);
	wr(SE + SE_DMA_GENERAL_CFG, rd(SE + SE_DMA_GENERAL_CFG) | 0xf);
	wr(SE + GENI_OUTPUT_CTRL, 0x7f);
	wr(SE + GENI_FORCE_DEFAULT, 1);
	wr(SE + GENI_DMA_MODE, rd(SE + GENI_DMA_MODE) & ~1u);
	wr(SE + GENI_RX_WM, fifo_depth - 3);
	wr(SE + GENI_RX_RFR, fifo_depth - 2);
	wr(SE + GENI_M_IRQ_EN, rd(SE + GENI_M_IRQ_EN) | 0x7fc0007f);
	wr(SE + GENI_S_IRQ_EN, rd(SE + GENI_S_IRQ_EN) | 0x03003e3e);
	wr(SE + SPI_TRANS_CFG, rd(SE + SPI_TRANS_CFG) & ~2u);

	/* 8-bit words, four per FIFO entry, byte 0 first, MSB first */
	wr(SE + GENI_TX_PACK0, 0x0007f8fe);
	wr(SE + GENI_TX_PACK1, 0x000ffefe);
	wr(SE + GENI_RX_PACK0, 0x0007f8fe);
	wr(SE + GENI_RX_PACK1, 0x000ffefe);
	wr(SE + GENI_BYTE_GRAN, 0);
	wr(SE + SPI_WORD_LEN, 8 - 4);

	/* SPI mode 0, chip select 0 (unused: CS is a GPIO) */
	wr(SE + SPI_LOOPBACK, 0);
	wr(SE + SPI_DEMUX_SEL, 0);
	wr(SE + SPI_CPHA, 0);
	wr(SE + SPI_CPOL, 0);
	wr(SE + SPI_DEMUX_INV, 0);

	/* 19.2 MHz / 2 = 9.6 MHz SCLK */
	wr(SE + GENI_CLK_SEL, (uint32_t)clk_sel & 7);
	wr(SE + GENI_M_CLK_CFG, (2u << 4) | 1);
	return 0;
}

static void spi_abort(void)
{
	wr(SE + GENI_M_CMD_CTRL, 4);	/* cancel */
	if (wait_clear(SE + GENI_STATUS, 1, 10000)) {
		wr(SE + GENI_M_CMD_CTRL, 2);	/* abort */
		(void)wait_clear(SE + GENI_STATUS, 1, 10000);
	}
	while (rd(SE + GENI_RX_STATUS) & 0x1ffffff)
		(void)rd(SE + GENI_RX_FIFO);
	wr(SE + GENI_M_IRQ_CLR, ~0u);
}

/*
 * One chip-select frame: hdr[0..nh) then data[0..nd) on MOSI. With rx,
 * the frame is full duplex and rx receives every byte clocked in.
 */
static int spi_frame(const uint8_t *hdr, unsigned nh, const uint8_t *data,
		     unsigned nd, uint8_t *rx)
{
	unsigned n = nh + nd, sent = 0, got = 0;

	if (wait_clear(SE + GENI_STATUS, 1, 10000))
		spi_abort();
	wr(SE + GENI_M_IRQ_CLR, ~0u);
	wr(PIN_CS + PIN_IO, 0);
	wr(SE + SPI_TX_LEN, n);
	if (rx)
		wr(SE + SPI_RX_LEN, n);
	wr(SE + GENI_M_CMD, ((rx ? OP_FULL_DUPLEX : OP_TX_ONLY) << 27) | FRAGMENTATION);

	uint64_t deadline = ticks() + freq() / 10 + freq() / 1000000 * n * 2;
	int err = -1;
	while (ticks() < deadline) {
		while (sent < n && (rd(SE + GENI_TX_STATUS) & 0x0fffffff) < fifo_depth) {
			uint32_t w = 0;
			for (unsigned b = 0; b < 4 && sent < n; b++, sent++) {
				uint8_t v = sent < nh ? hdr[sent] : data[sent - nh];
				w |= (uint32_t)v << (8 * b);
			}
			wr(SE + GENI_TX_FIFO, w);
		}
		if (rx) {
			uint32_t wc = rd(SE + GENI_RX_STATUS) & 0x1ffffff;
			while (wc--) {
				uint32_t w = rd(SE + GENI_RX_FIFO);
				for (unsigned b = 0; b < 4 && got < n; b++)
					rx[got++] = (uint8_t)(w >> (8 * b));
			}
		}
		uint32_t m = rd(SE + GENI_M_IRQ);
		if (m & M_ERRORS)
			break;
		if ((m & M_CMD_DONE) && sent == n && (!rx || got == n ||
		    !(rd(SE + GENI_RX_STATUS) & 0x1ffffff))) {
			wr(SE + GENI_M_IRQ_CLR, m);
			err = (!rx || got == n) ? 0 : -1;
			break;
		}
	}
	wr(PIN_CS + PIN_IO, 2);
	if (err)
		spi_abort();
	return err;
}

/* ---- NT36xxx framing ---- */

static int nt_page(uint32_t addr)
{
	uint8_t c[3] = { 0xff, (uint8_t)(addr >> 15), (uint8_t)(addr >> 7) };
	return spi_frame(c, 3, 0, 0, 0);
}

static int nt_write(uint32_t addr, const uint8_t *data, unsigned n)
{
	uint8_t c = (uint8_t)((addr & 0x7f) | 0x80);
	if (nt_page(addr))
		return -1;
	return spi_frame(&c, 1, data, n, 0);
}

static int nt_write1(uint32_t addr, uint8_t v)
{
	return nt_write(addr, &v, 1);
}

static uint8_t rxbuf[2 + 128];
static const uint8_t zeros[128];

/* Reads n <= 128 bytes; the first two clocked-in bytes are command and dummy. */
static int nt_read(uint32_t addr, uint8_t *out, unsigned n)
{
	uint8_t c = (uint8_t)(addr & 0x7f);
	if (nt_page(addr) || spi_frame(&c, 1, zeros, n + 1, rxbuf))
		return -1;
	for (unsigned i = 0; i < n; i++)
		out[i] = rxbuf[2 + i];
	return 0;
}

static void bootloader_reset(void)
{
	nt_write1(SWRST_N8, 0x69);
	sleep_us(5000);
	nt_write1(SPI_RD_FAST, 0x00);
}

static int chip_id(void)
{
	static const uint32_t addrs[2] = { TRIM_ID, TRIM_ID_NEW };
	uint8_t id[6];

	for (int t = 0; t < 5; t++) {
		bootloader_reset();
		for (unsigned a = 0; a < 2; a++) {
			if (nt_read(addrs[a], id, 6))
				continue;
			con_hex2("TRIM", "lo=", le32(id), "hi=", id[4] | id[5] << 8);
			if (id[4] == 0x66 && id[5] == 0x03 && (id[3] == 0x72 || id[3] == 0x75))
				return 0;
		}
		sleep_us(10000);
	}
	return -1;
}

/* ---- firmware download (hardware CRC path) ---- */

struct part { uint32_t bin, sram, size, crc; };

static int fw_parts(struct part *p, unsigned max)
{
	unsigned n = 0;
	uint32_t end = le32(fw);

	if (end != 0x100 || fw[0x1fffd] != 'N' || fw[0x1fffe] != 'V' || fw[0x1ffff] != 'T' ||
	    (uint8_t)(fw[0x1f000] + fw[0x1f001]) != 0xff)
		return -1;
	p[n++] = (struct part){ le32(fw + 0x00), le32(fw + 0x04), le32(fw + 0x08), le32(fw + 0x18) };
	p[n++] = (struct part){ le32(fw + 0x0c), le32(fw + 0x10), le32(fw + 0x14), le32(fw + 0x1c) };
	for (uint32_t off = 0x30; off < end && n < max; off += 16) {
		uint32_t sram = le32(fw + off), size = le32(fw + off + 4), bin = le32(fw + off + 8);
		if (size)
			p[n++] = (struct part){ bin, sram, size, le32(fw + off + 12) };
	}
	for (unsigned i = 0; i < n; i++)
		if (p[i].bin + p[i].size + 1 > FW_SIZE)
			return -1;
	return (int)n;
}

static int write_part(const struct part *p)
{
	uint32_t sram = p->sram, bin = p->bin, left = p->size + 1;

	while (left) {
		uint32_t len = left > CHUNK ? CHUNK : left;
		if (nt_write(sram, fw + bin, len))
			return -1;
		sram += len;
		bin += len;
		left -= len;
	}
	return 0;
}

static int set_crc_bank(uint32_t des, uint32_t len_reg, uint32_t crc_reg, const struct part *p)
{
	uint8_t a[3] = { (uint8_t)p->sram, (uint8_t)(p->sram >> 8), (uint8_t)(p->sram >> 16) };
	uint8_t l[3] = { (uint8_t)p->size, (uint8_t)(p->size >> 8), (uint8_t)((p->size >> 16) & 1) };
	uint8_t c[4] = { (uint8_t)p->crc, (uint8_t)(p->crc >> 8), (uint8_t)(p->crc >> 16), (uint8_t)(p->crc >> 24) };

	return nt_write(des, a, 3) || nt_write(len_reg, l, 3) || nt_write(crc_reg, c, 4);
}

static int reset_state(uint8_t lo, unsigned tries)
{
	uint8_t s[5];

	for (unsigned i = 0; i < tries; i++) {
		if (!nt_read(RESET_STATE, s, 5) && s[0] >= lo && s[0] <= 0xaf)
			return s[0];
		sleep_us(10000);
	}
	return -1;
}

static int download_once(const struct part *p, int n)
{
	uint8_t v;

	bootloader_reset();
	for (int i = 0; i < n; i++)
		if (write_part(&p[i]))
			return -1;
	if (set_crc_bank(ILM_DES, ILM_LEN, ILM_CRC, &p[0]) ||
	    set_crc_bank(DLM_DES, DLM_LEN, DLM_CRC, &p[1]))
		return -1;
	if (nt_read(BLD_CRC_EN, &v, 1) || nt_write1(BLD_CRC_EN, v | 0x80))
		return -1;
	nt_write1(RESET_STATE, 0);
	nt_write1(HOST_CMD, 0xae);
	nt_write1(BOOT_RDY, 1);
	sleep_us(5000);
	return reset_state(0xa0, 11);
}

static int download(void)
{
	struct part p[16];
	int n = fw_parts(p, 16);
	uint8_t flag = 0;

	if (n < 2) {
		con_str("FW", "bad image");
		return -1;
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		uint64_t t0 = ticks();
		int st = download_once(p, n);
		con_hex2("FW", "state=", (uint32_t)st, "ms=", (uint32_t)((ticks() - t0) * 1000 / freq()));
		if (st >= 0)
			return 0;
		nt_read(BLD_CRC_FLAG, &flag, 1);
		con_hex2("FW", "attempt=", (uint32_t)attempt, "crc_flag=", flag);
	}
	return -1;
}

/* ---- touch reports ---- */

static void square(int x, int y, uint32_t color)
{
	for (int dy = -12; dy <= 12; dy++)
		for (int dx = -12; dx <= 12; dx++) {
			int px = x + dx, py = y + dy;
			if (px >= 0 && px < FB_W && py >= 0 && py < FB_H)
				con_fb[py * (FB_STRIDE / 4) + px] = color;
		}
}

static const uint32_t finger_color[FINGERS] = {
	0xffff4040, 0xff40ff40, 0xff4080ff, 0xffffff40, 0xffff40ff,
	0xff40ffff, 0xffffa040, 0xffa040ff, 0xffffffff, 0xff808080,
};

static void report(const uint8_t *p, unsigned *count)
{
	for (unsigned i = 0; i < FINGERS; i++) {
		const uint8_t *f = p + 6 * i;
		unsigned id = f[0] >> 3, state = f[0] & 7;
		if (id == 0 || id > FINGERS || (state != 1 && state != 2))
			continue;
		int x = f[1] << 4 | f[3] >> 4;
		int y = f[2] << 4 | (f[3] & 0x0f);
		square(x, y, finger_color[id - 1]);
		if ((*count)++ < 8)
			con_hex2("TOUCH", "id=", id, "xy=", (uint32_t)x << 16 | (uint32_t)y);
	}
}

void test_touch(uint64_t dtb, unsigned seconds)
{
	uint64_t rd_start, rd_end;
	uint8_t info[38], pkt[POINT_LEN];

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end) ||
	    rd_end - rd_start < FW_SIZE) {
		con_str("TOUCH", "no firmware in the ramdisk");
		return;
	}
	fw = (const uint8_t *)(uintptr_t)rd_start;

	con_str("TOUCH", "clocks");
	int clk_sel = clocks();
	if (clk_sel < 0) {
		con_str("TOUCH", "no usable SE4 clock");
		return;
	}
	con_str("TOUCH", "pins");
	pins();
	if (spi_init(clk_sel)) {
		con_str("TOUCH", "SE4 is not a FIFO SPI engine");
		return;
	}

	/* Hardware reset as the vendor driver does it: engineering reset while RESX is low. */
	con_str("TOUCH", "reset");
	nt_write1(ENG_RST, 0x5a);
	sleep_us(1000);
	wr(PIN_RST + PIN_IO, 2);
	sleep_us(10000);

	con_str("TOUCH", "chip id");
	if (chip_id()) {
		con_str("TOUCH", "no NT36675 answer");
		return;
	}
	con_str("TOUCH", "firmware");
	if (download()) {
		con_str("TOUCH", "firmware did not start");
		return;
	}
	if (!nt_read(FW_INFO, info, sizeof(info)))
		con_hex2("FWINFO", "ver=", info[0] | info[1] << 8,
			 "xy=", (uint32_t)(info[4] << 8 | info[5]) << 16 | (uint32_t)(info[6] << 8 | info[7]));
	con_hex2("TOUCH", "rek=", (uint32_t)reset_state(0xa1, 51), "secs=", seconds);
	con_str("TOUCH", "draw on the screen");

	nt_page(EVENT_BUF);
	unsigned count = 0;
	uint64_t end = ticks() + freq() * seconds;
	wr(PIN_INT + PIN_INTR_STS, 0);
	while (ticks() < end) {
		if (!(rd(PIN_INT + PIN_INTR_STS) & 1))
			continue;
		wr(PIN_INT + PIN_INTR_STS, 0);
		if (!nt_read(EVENT_BUF, pkt, POINT_LEN))
			report(pkt, &count);
	}
	con_hex("TOUCH events", count);
}
