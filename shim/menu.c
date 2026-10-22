/*
 * Boot menu: shown for a few seconds before the payload boots. Volume up
 * and down move, power selects; with no key pressed the first entry boots.
 * Keys are read from the PMIC (power and volume down in the PON block,
 * volume up on a PMIC GPIO), so only the board's key wiring is needed.
 */
#include <stdint.h>
#include "board.h"
#include "console.h"
#include "menu.h"
#include "spmi.h"

#define BG		0xff0d0f14U
#define FG		0xffe8eaedU
#define DIM		0xff8a9099U
#define ACCENT		0xff2f6fedU
#define ITEMS		6
#define AUTOBOOT_S	3

#define KEY_POWER	1u
#define KEY_UP		2u
#define KEY_DOWN	4u

/* Labels are NUL-separated in one string: a table of string pointers
 * would need absolute relocations, which the shim cannot have. */
static const char labels[] =
	"iOS\0"
	"iOS - verbose\0"
	"iOS - safe mode\0"
	"Reboot to fastboot\0"
	"Reboot to recovery\0"
	"Power off\0";
static const char hints[] =
	"Normal boot\0"
	"Kernel log on the screen\0"
	"One CPU, stock clocks, no touch\0"
	"Boot or flash images from a computer\0"
	"The recovery partition (TWRP)\0"
	"Turn the phone off\0";

static const char *nth(const char *list, int i)
{
	while (i-- > 0)
		while (*list++)
			;
	return list;
}

#define label(i)	nth(labels, i)
#define hint(i)		nth(hints, i)

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

static unsigned keys(void)
{
	int pon = spmi_rd(0, PON_RT_STS);
	int up = spmi_rd(VOLUP_SID, VOLUP_GPIO_BASE + 0x10);
	unsigned k = 0;

	if (pon >= 0 && (pon & 1))
		k |= KEY_POWER;
	if (pon >= 0 && (pon & 2))
		k |= KEY_DOWN;
	if (up >= 0 && !(up & 1))		/* active low */
		k |= KEY_UP;
	return k;
}

static void center(int y, int scale, const char *s, uint32_t fg)
{
	con_text((con_w - con_text_width(s, scale)) / 2, y, scale, s, fg, BG);
}

#define LIST_Y		900
#define ROW_H		150
#define MARGIN_X	60

static void draw_item(int i, int selected)
{
	int y = LIST_Y + i * ROW_H;
	uint32_t bg = selected ? ACCENT : BG;

	con_rect(MARGIN_X, y, con_w - 2 * MARGIN_X, ROW_H - 24, bg);
	con_text(MARGIN_X + 48, y + (ROW_H - 24 - 64) / 2, 4, label(i), selected ? 0xffffffffU : FG, bg);
}

static void draw_hint(int i)
{
	int y = LIST_Y + ITEMS * ROW_H + 40;
	con_rect(0, y, con_w, 64, BG);
	center(y, 3, hint(i), DIM);
}

static void draw_countdown(int secs)
{
	char line[] = "Booting iOS in 0 s";
	int y = con_h - 260;

	con_rect(0, y, con_w, 64, BG);
	if (secs >= 0) {
		line[15] = (char)('0' + secs);
		center(y, 3, line, FG);
	}
}

static void draw(int sel)
{
	con_rect(0, 0, con_w, con_h, BG);
	center(380, 7, "iOS 16", FG);
	center(540, 3, "16.7.16 (20H392)", DIM);
	center(600, 3, BOARD_MODEL, DIM);
	for (int i = 0; i < ITEMS; i++)
		draw_item(i, i == sel);
	draw_hint(sel);
	center(con_h - 160, 2, "Volume up / down: choose    Power: start", DIM);
}

enum menu_action boot_menu(void)
{
	uint64_t f = freq(), deadline = ticks() + f * AUTOBOOT_S;
	unsigned prev = keys();	/* a key still held from power-on is not a press */
	int sel = 0, counting = 1, shown = AUTOBOOT_S;

	draw(sel);
	draw_countdown(shown);
	for (;;) {
		uint64_t now = ticks();
		if (counting) {
			if (now >= deadline)
				return MENU_BOOT;
			int left = (int)((deadline - now + f - 1) / f);
			if (left != shown) {
				shown = left;
				draw_countdown(left);
			}
		}
		unsigned k = keys(), pressed = k & ~prev;
		prev = k;
		if (pressed && counting) {
			counting = 0;
			draw_countdown(-1);
		}
		if (pressed & (KEY_UP | KEY_DOWN)) {
			int next = (sel + (pressed & KEY_UP ? ITEMS - 1 : 1)) % ITEMS;
			draw_item(sel, 0);
			draw_item(next, 1);
			draw_hint(next);
			sel = next;
		}
		if (pressed & KEY_POWER)
			return (enum menu_action)sel;
		uint64_t until = ticks() + f / 50;	/* 20 ms */
		while (ticks() < until)
			;
	}
}

const char *menu_boot_args(enum menu_action a)
{
	if (a == MENU_BOOT_VERBOSE)
		return "sweetfb_noconsole=0";
	if (a == MENU_BOOT_SAFE)
		return "sweetfb_noconsole=0 cpus=1 cpufreq=0 sweettouch=0 sweetbacklight=0 sweetaccel=0";
	return "";
}
