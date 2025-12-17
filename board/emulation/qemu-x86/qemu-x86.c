// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2025 Patrick Rudolph
 */

#include <config.h>
#include <init.h>
#include <usb.h>

int board_late_init(void)
{
	/* start usb so that usb keyboard can be used as input device */
	if (CONFIG_IS_ENABLED(USB_KEYBOARD))
		usb_init();

	return 0;
}