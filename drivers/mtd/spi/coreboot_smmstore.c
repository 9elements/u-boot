// SPDX-License-Identifier: GPL-2.0+
/*
 * SPI flash device backed by the coreboot SMMSTORE v2 interface
 *
 * coreboot reserves a region of the boot flash and lets the payload read,
 * write and erase it in blocks through SMI calls, so the payload needs no
 * flash driver of its own. Present that region as a SPI flash device so that
 * users such as the UEFI variable store (EFI_VARIABLE_SF_STORE) can keep
 * persistent data there.
 *
 * Copyright (C) 2026 9elements GmbH
 */

#define LOG_CATEGORY UCLASS_SPI_FLASH

#include <dm.h>
#include <errno.h>
#include <limits.h>
#include <log.h>
#include <mapmem.h>
#include <spi_flash.h>
#include <asm/cb_sysinfo.h>
#include <asm/smm.h>
#include <linux/kernel.h>
#include <linux/string.h>

#define SMMSTORE_RET_SUCCESS	0

#define SMMSTORE_CMD_RAW_READ	5
#define SMMSTORE_CMD_RAW_WRITE	6
#define SMMSTORE_CMD_RAW_CLEAR	7

/* Parameters of the raw read and raw write commands */
struct smmstore_params_rw {
	u32 bufsize;
	u32 bufoffset;
	u32 block_id;
} __packed;

/* Parameters of the raw clear command */
struct smmstore_params_clear {
	u32 block_id;
} __packed;

/**
 * struct smmstore_priv - private data for the SMMSTORE flash device
 *
 * @com_buf: Communication buffer; data for block offset N is at com_buf + N
 * @block_size: Size of each block in bytes
 * @apm_cmd: Value to write to the APM command port to call the store
 * @rw: Parameter block for read and write, must be below 4GiB
 * @clear: Parameter block for clear, must be below 4GiB
 */
struct smmstore_priv {
	void *com_buf;
	u32 block_size;
	u8 apm_cmd;
	struct smmstore_params_rw rw;
	struct smmstore_params_clear clear;
};

/*
 * Raise an SMI to run a store command. The command goes in %ah, the APM
 * command byte in %al and the 32-bit address of the parameter block in %ebx.
 * coreboot returns the result in %eax.
 */
static int smmstore_call(struct smmstore_priv *priv, u8 cmd, void *params)
{
	ulong addr = map_to_sysmem(params);
	u32 ret;

	if (addr > U32_MAX)
		return -EFAULT;

	asm volatile("outb %%al, %%dx"
		     : "=a" (ret)
		     : "0" (cmd << 8 | priv->apm_cmd), "b" ((u32)addr),
		       "d" (APM_CNT)
		     : "memory");
	if (ret != SMMSTORE_RET_SUCCESS) {
		log_debug("command %u failed: %u\n", cmd, ret);
		return -EIO;
	}

	return 0;
}

static int smmstore_check_range(struct udevice *dev, u32 offset, size_t len)
{
	struct spi_flash *flash = dev_get_uclass_priv(dev);

	if (offset > flash->size || len > flash->size - offset)
		return -EINVAL;

	return 0;
}

static int smmstore_read(struct udevice *dev, u32 offset, size_t len,
			 void *buf)
{
	struct smmstore_priv *priv = dev_get_priv(dev);
	int ret;

	ret = smmstore_check_range(dev, offset, len);
	if (ret)
		return ret;

	while (len) {
		u32 pos = offset % priv->block_size;
		u32 chunk = min_t(size_t, len, priv->block_size - pos);

		priv->rw.bufsize = chunk;
		priv->rw.bufoffset = pos;
		priv->rw.block_id = offset / priv->block_size;
		ret = smmstore_call(priv, SMMSTORE_CMD_RAW_READ, &priv->rw);
		if (ret)
			return ret;
		memcpy(buf, priv->com_buf + pos, chunk);

		buf += chunk;
		offset += chunk;
		len -= chunk;
	}

	return 0;
}

static int smmstore_write(struct udevice *dev, u32 offset, size_t len,
			  const void *buf)
{
	struct smmstore_priv *priv = dev_get_priv(dev);
	int ret;

	ret = smmstore_check_range(dev, offset, len);
	if (ret)
		return ret;

	while (len) {
		u32 pos = offset % priv->block_size;
		u32 chunk = min_t(size_t, len, priv->block_size - pos);

		memcpy(priv->com_buf + pos, buf, chunk);
		priv->rw.bufsize = chunk;
		priv->rw.bufoffset = pos;
		priv->rw.block_id = offset / priv->block_size;
		ret = smmstore_call(priv, SMMSTORE_CMD_RAW_WRITE, &priv->rw);
		if (ret)
			return ret;

		buf += chunk;
		offset += chunk;
		len -= chunk;
	}

	return 0;
}

static int smmstore_erase(struct udevice *dev, u32 offset, size_t len)
{
	struct smmstore_priv *priv = dev_get_priv(dev);
	int ret;

	ret = smmstore_check_range(dev, offset, len);
	if (ret)
		return ret;
	if (offset % priv->block_size || len % priv->block_size)
		return -EINVAL;

	for (; len; offset += priv->block_size, len -= priv->block_size) {
		priv->clear.block_id = offset / priv->block_size;
		ret = smmstore_call(priv, SMMSTORE_CMD_RAW_CLEAR, &priv->clear);
		if (ret)
			return ret;
	}

	return 0;
}

static int smmstore_probe(struct udevice *dev)
{
	struct spi_flash *flash = dev_get_uclass_priv(dev);
	struct smmstore_priv *priv = dev_get_priv(dev);
	const struct sysinfo_t *sysinfo = cb_get_sysinfo();

	if (!sysinfo || !sysinfo->smmstore.num_blocks)
		return -ENODEV;
	if (!sysinfo->smmstore.block_size ||
	    sysinfo->smmstore.com_buffer_size < sysinfo->smmstore.block_size)
		return log_msg_ret("buf", -EINVAL);

	priv->com_buf = map_sysmem(sysinfo->smmstore.com_buffer,
				   sysinfo->smmstore.com_buffer_size);
	priv->block_size = sysinfo->smmstore.block_size;
	priv->apm_cmd = sysinfo->smmstore.apm_cmd;

	flash->dev = dev;
	flash->name = dev->name;
	flash->size = sysinfo->smmstore.num_blocks * priv->block_size;
	flash->sector_size = priv->block_size;
	flash->erase_size = priv->block_size;
	flash->page_size = priv->block_size;

	return 0;
}

static const struct dm_spi_flash_ops smmstore_ops = {
	.read = smmstore_read,
	.write = smmstore_write,
	.erase = smmstore_erase,
};

static const struct udevice_id smmstore_ids[] = {
	{ .compatible = "coreboot,smmstore" },
	{ }
};

U_BOOT_DRIVER(coreboot_smmstore) = {
	.name		= "coreboot_smmstore",
	.id		= UCLASS_SPI_FLASH,
	.of_match	= smmstore_ids,
	.probe		= smmstore_probe,
	.ops		= &smmstore_ops,
	.priv_auto	= sizeof(struct smmstore_priv),
};
