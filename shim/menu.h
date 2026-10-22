#pragma once

/* In menu order. */
enum menu_action {
	MENU_BOOT,
	MENU_BOOT_VERBOSE,
	MENU_BOOT_SAFE,
	MENU_FASTBOOT,
	MENU_RECOVERY,
	MENU_POWER_OFF,
};

/* Draws the menu and waits for a choice (or the autoboot timeout). */
enum menu_action boot_menu(void);
/* Boot-args to put in front of the payload's own for a boot entry. */
const char *menu_boot_args(enum menu_action a);
