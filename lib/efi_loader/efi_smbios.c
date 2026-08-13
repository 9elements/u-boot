// SPDX-License-Identifier: GPL-2.0+
/*
 *  EFI application tables support
 *
 *  Copyright (c) 2016 Alexander Graf
 */

#define LOG_CATEGORY LOGC_EFI

#include <efi_loader.h>
#include <log.h>
#include <malloc.h>
#include <mapmem.h>
#include <smbios.h>
#include <linux/sizes.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

const efi_guid_t smbios3_guid = SMBIOS3_TABLE_GUID;

enum {
	TABLE_SIZE	= SZ_4K,
};

/*
 * Some firmware (e.g. coreboot) writes a legacy "_SM_" (2.1) entry point
 * immediately followed by a "_SM3_" (3.0) entry point in the same table,
 * but only ever hands U-Boot the address of the 2.1 entry point. Tagging
 * that address with the SMBIOS3 GUID makes strict SMBIOS3 parsers (e.g.
 * the Linux kernel) reject it, since they require the "_SM3_" anchor at
 * whatever address carries that GUID. Look for a "_SM3_" entry point
 * trailing the 2.1 one and prefer it if found.
 *
 * Return:	pointer to the 3.0 entry point, or NULL if not found
 */
static const struct smbios3_entry *find_trailing_smbios3(const struct smbios_entry *entry)
{
	const u8 *buf = (const u8 *)entry;
	uint offset;

	for (offset = entry->length; offset + sizeof(struct smbios3_entry) <= TABLE_SIZE;
	     offset++) {
		if (!memcmp(buf + offset, "_SM3_", 5))
			return (const struct smbios3_entry *)(buf + offset);
	}

	return NULL;
}

/*
 * Install the SMBIOS table as a configuration table.
 *
 * Return:	status code
 */
efi_status_t efi_smbios_register(void)
{
	ulong addr;
	efi_status_t ret;
	void *buf;
	const void *table;
	const efi_guid_t *guid;

	addr = gd_smbios_start();
	if (!addr) {
		log_err("No SMBIOS tables to install\n");
		return EFI_NOT_FOUND;
	}

	/* Mark space used for tables */
	ret = efi_add_memory_map(addr, TABLE_SIZE, EFI_RUNTIME_SERVICES_DATA);
	if (ret)
		return ret;

	buf = map_sysmem(addr, 0);

	if (!memcmp(buf, "_SM3_", 5)) {
		guid = &smbios3_guid;
		table = buf;
	} else if (!memcmp(buf, "_SM_", 4)) {
		const struct smbios3_entry *entry3 = find_trailing_smbios3(buf);

		if (entry3) {
			guid = &smbios3_guid;
			table = entry3;
		} else {
			guid = &smbios_guid;
			table = buf;
		}
	} else {
		log_err("Invalid SMBIOS anchor at %lx\n", addr);
		unmap_sysmem(buf);
		return EFI_NOT_FOUND;
	}

	log_debug("EFI using SMBIOS tables at %p\n", table);

	/* Install SMBIOS information as configuration table */
	ret = efi_install_configuration_table(guid, (void *)table);
	unmap_sysmem(buf);

	return ret;
}

static int install_smbios_table(void)
{
	ulong addr;
	void *buf;

	if (!IS_ENABLED(CONFIG_GENERATE_SMBIOS_TABLE) ||
	    IS_ENABLED(CONFIG_X86) ||
	    IS_ENABLED(CONFIG_QFW_SMBIOS))
		return 0;

	/* Align the table to a 4KB boundary to keep EFI happy */
	buf = memalign(SZ_4K, TABLE_SIZE);
	if (!buf)
		return log_msg_ret("mem", -ENOMEM);

	addr = map_to_sysmem(buf);
	if (!write_smbios_table(addr)) {
		log_err("Failed to write SMBIOS table\n");
		return log_msg_ret("smbios", -EINVAL);
	}

	/* Make a note of where we put it */
	log_debug("SMBIOS tables written to %lx\n", addr);
	gd->arch.smbios_start = addr;

	return 0;
}
EVENT_SPY_SIMPLE(EVT_LAST_STAGE_INIT, install_smbios_table);
