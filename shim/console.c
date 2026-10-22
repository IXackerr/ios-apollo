/*
 * Minimal text console on the ABL splash framebuffer. With the MMU off
 * every access is Device memory, so stay aligned (-mstrict-align).
 */
#include "console.h"
#include "font.h"

#define SCALE		3
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

volatile uint32_t *con_fb = (volatile uint32_t *)FB_BASE;
/* Geometry of the buffer we draw into; the apollo display probe moves the
 * console to the buffer the display engine actually scans out. */
int con_w = FB_W, con_h = FB_H, con_stride = FB_STRIDE;
static uint32_t con_bg;
static int row;

void con_clear(uint32_t bg)
{
	for (int y = 0; y < con_h; y++) {
		volatile uint32_t *line = con_fb + y * (con_stride / 4);
		for (int x = 0; x < con_w; x++)
			line[x] = bg;
	}
	con_bg = bg;
	row = 0;
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > con_w || y0 + CH_H > con_h)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < SCALE; sy++) {
				volatile uint32_t *p = con_fb + (y0 + gy * SCALE + sy) * (con_stride / 4)
						       + x0 + gx * SCALE;
				for (int sx = 0; sx < SCALE; sx++)
					p[sx] = c;
			}
		}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_skip(void)
{
	row++;
}

void con_str(const char *label, const char *s)
{
	put_str(0, label, YELLOW);
	put_str(LABEL_COLS, s, WHITE);
	row++;
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	row++;
}

void con_rect(int x, int y, int w, int h, uint32_t color)
{
	for (int yy = y; yy < y + h && yy < con_h; yy++) {
		volatile uint32_t *line = con_fb + yy * (con_stride / 4);
		for (int xx = x; xx < x + w && xx < con_w; xx++)
			line[xx] = color;
	}
}

int con_text(int x, int y, int scale, const char *s, uint32_t fg, uint32_t bg)
{
	for (; *s; s++, x += 8 * scale) {
		char ch = *s < 0x20 || *s > 0x7e ? '?' : *s;
		const unsigned char *g = font8x16[ch - 0x20];
		if (x + 8 * scale > con_w || y + 16 * scale > con_h)
			break;
		for (int gy = 0; gy < 16 * scale; gy++) {
			volatile uint32_t *p = con_fb + (y + gy) * (con_stride / 4) + x;
			for (int gx = 0; gx < 8 * scale; gx++)
				p[gx] = (g[gy / scale] & (0x80 >> (gx / scale))) ? fg : bg;
		}
	}
	return x;
}

int con_text_width(const char *s, int scale)
{
	int n = 0;
	while (s[n])
		n++;
	return n * 8 * scale;
}
