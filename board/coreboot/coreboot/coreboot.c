// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018, Bin Meng <bmeng.cn@gmail.com>
 */

#include <splash.h>
#include <init.h>
#include <smbios.h>
#include <asm/cb_sysinfo.h>
#include <efi_loader.h>

#if IS_ENABLED(CONFIG_EFI_HAVE_CAPSULE_SUPPORT)
/*
 * One updatable image: the flash region in COREBOOT_CAPSULE_DFU_ALT_INFO.
 * Its GUID is generated from the device-tree compatible and the name.
 */
struct efi_fw_image fw_images[] = {
	{
		.fw_name = u"COREBOOT-RECOVERY-A",
		.image_index = 1,
	},
};

struct efi_capsule_update_info update_info = {
	.dfu_string = CONFIG_COREBOOT_CAPSULE_DFU_ALT_INFO,
	.num_images = ARRAY_SIZE(fw_images),
	.images = fw_images,
};
#endif

int board_early_init_r(void)
{
	/*
	 * Make sure PCI bus is enumerated so that peripherals on the PCI bus
	 * can be discovered by their drivers
	 */
	pci_init();

	return 0;
}

static struct splash_location coreboot_splash_locations[] = {
	{
		.name = "virtio_fs",
		.storage = SPLASH_STORAGE_VIRTIO,
		.flags = SPLASH_STORAGE_RAW,
		.devpart = "0",
	},
};

int splash_screen_prepare(void)
{
	return splash_source_load(coreboot_splash_locations,
				  ARRAY_SIZE(coreboot_splash_locations));
}
