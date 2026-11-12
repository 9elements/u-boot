// SPDX-License-Identifier: GPL-2.0+
/*
 * AMD FCH SPI100 controller, used for the boot SPI flash.
 *
 * Ported from coreboot's soc/amd/common/block/spi/fch_spi_ctrl.c. The
 * controller sends one command at a time: the opcode goes in a register, and
 * the address, dummy and data bytes go through a 71-byte FIFO, shared by the
 * bytes sent and the bytes received.
 *
 * The PSP uses the same controller. Around each command the driver sets the
 * BIOS lock, which tells coreboot's SMM handler to leave the flash alone, as
 * coreboot's ramstage does. SoCs with the SPI mutex (CONFIG_AMD_FCH_SPI_MUTEX)
 * also take the host BIOS mutex first.
 */

#define LOG_CATEGORY	UCLASS_SPI

#include <dm.h>
#include <errno.h>
#include <log.h>
#include <spi.h>
#include <spi-mem.h>
#include <asm/io.h>
#include <asm/pci.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <time.h>

/* LPC bridge (0:14.3) register with the SPI controller's base address */
#define LPC_BDF				PCI_BDF(0, 0x14, 3)
#define LPC_SPI_BASE			0xa0
#define   SPI_BASE_ALIGN		0x100

#define SPI_CNTRL0			0x00
#define   SPI_BUSY			BIT(31)
#define SPI_ALT_CS			0x1d
#define   SPI_ALT_CS_MASK		0x03
#define SPI_CMD_CODE			0x45
#define SPI_CMD_TRIGGER			0x47
#define   SPI_CMD_TRIGGER_EXECUTE	BIT(7)
#define SPI_TX_BYTE_COUNT		0x48
#define SPI_RX_BYTE_COUNT		0x4b
#define SPI_STATUS			0x4c
#define SPI_FIFO			0x80
#define SPI_FIFO_DEPTH			(0xc6 - SPI_FIFO + 1)
#define SPI_MISC_CNTRL			0xfc
#define   SPI_MUTEX_PSP_OWNS		BIT(10)
#define   SPI_MUTEX_HFP_OWNS		BIT(9)
#define   SPI_MUTEX_HBIOS_OWNS		BIT(5)
#define   SPI_SEMAPHORE_BIOS_LOCKED	BIT(3)

#define SPI_READY_TIMEOUT_MS		500
#define SPI_MUTEX_TIMEOUT_MS		1000

struct amd_fch_spi_priv {
	void __iomem *base;
	u8 default_cs;
	uint claimed;	/* nested claims: spi-nor claims around each op too */
};

static int amd_fch_spi_wait_ready(struct amd_fch_spi_priv *priv)
{
	ulong start = get_timer(0);

	while (readl(priv->base + SPI_STATUS) & SPI_BUSY) {
		if (get_timer(start) > SPI_READY_TIMEOUT_MS)
			return -ETIMEDOUT;
	}

	return 0;
}

static int amd_fch_spi_mutex_claim(struct amd_fch_spi_priv *priv)
{
	ulong start = get_timer(0);
	u16 misc;

	/* Wait until neither the PSP nor the host FP holds the mutex */
	for (;;) {
		misc = readw(priv->base + SPI_MISC_CNTRL);
		if (!(misc & (SPI_MUTEX_PSP_OWNS | SPI_MUTEX_HFP_OWNS))) {
			writew(misc | SPI_MUTEX_HBIOS_OWNS,
			       priv->base + SPI_MISC_CNTRL);
			misc = readw(priv->base + SPI_MISC_CNTRL);
			if ((misc & SPI_MUTEX_HBIOS_OWNS) &&
			    !(misc & (SPI_MUTEX_PSP_OWNS | SPI_MUTEX_HFP_OWNS)))
				return 0;
		}
		if (get_timer(start) > SPI_MUTEX_TIMEOUT_MS) {
			log_err("SPI mutex held by %s (misc %04x)\n",
				misc & SPI_MUTEX_PSP_OWNS ? "PSP" : "host FP",
				misc);
			return -EBUSY;
		}
		udelay(10);
	}
}

static int amd_fch_spi_claim_bus(struct udevice *dev)
{
	struct amd_fch_spi_priv *priv = dev_get_priv(dev->parent);
	u8 misc, cs;
	int ret;

	if (priv->claimed++)
		return 0;

	if (IS_ENABLED(CONFIG_AMD_FCH_SPI_MUTEX)) {
		ret = amd_fch_spi_mutex_claim(priv);
		if (ret)
			goto err_count;
	}

	/* Keep the PSP's SMM flash requests away while U-Boot uses the flash */
	misc = readb(priv->base + SPI_MISC_CNTRL);
	if (misc & SPI_SEMAPHORE_BIOS_LOCKED) {
		log_err("SPI controller is locked by someone else\n");
		ret = -EBUSY;
		goto err;
	}
	writeb(misc | SPI_SEMAPHORE_BIOS_LOCKED, priv->base + SPI_MISC_CNTRL);

	cs = readb(priv->base + SPI_ALT_CS);
	if ((cs & SPI_ALT_CS_MASK) != priv->default_cs)
		writeb((cs & ~SPI_ALT_CS_MASK) | priv->default_cs,
		       priv->base + SPI_ALT_CS);

	return 0;

err:
	if (IS_ENABLED(CONFIG_AMD_FCH_SPI_MUTEX))
		writew(readw(priv->base + SPI_MISC_CNTRL) & ~SPI_MUTEX_HBIOS_OWNS,
		       priv->base + SPI_MISC_CNTRL);
err_count:
	priv->claimed--;
	return ret;
}

static int amd_fch_spi_release_bus(struct udevice *dev)
{
	struct amd_fch_spi_priv *priv = dev_get_priv(dev->parent);

	if (!priv->claimed || --priv->claimed)
		return 0;

	writeb(readb(priv->base + SPI_MISC_CNTRL) & ~SPI_SEMAPHORE_BIOS_LOCKED,
	       priv->base + SPI_MISC_CNTRL);
	if (IS_ENABLED(CONFIG_AMD_FCH_SPI_MUTEX))
		writew(readw(priv->base + SPI_MISC_CNTRL) & ~SPI_MUTEX_HBIOS_OWNS,
		       priv->base + SPI_MISC_CNTRL);

	return 0;
}

static int amd_fch_spi_exec_op(struct spi_slave *slave,
			       const struct spi_mem_op *op)
{
	struct amd_fch_spi_priv *priv = dev_get_priv(slave->dev->parent);
	u8 tx[SPI_FIFO_DEPTH];
	uint tx_len = 0, rx_len = 0, i;
	int ret;

	for (i = 0; i < op->addr.nbytes; i++)
		tx[tx_len++] = op->addr.val >> (8 * (op->addr.nbytes - 1 - i));
	for (i = 0; i < op->dummy.nbytes; i++)
		tx[tx_len++] = 0xff;
	if (op->data.dir == SPI_MEM_DATA_OUT) {
		if (tx_len + op->data.nbytes > SPI_FIFO_DEPTH)
			return -EINVAL;
		memcpy(tx + tx_len, op->data.buf.out, op->data.nbytes);
		tx_len += op->data.nbytes;
	} else if (op->data.dir == SPI_MEM_DATA_IN) {
		rx_len = op->data.nbytes;
	}
	if (tx_len + rx_len > SPI_FIFO_DEPTH)
		return -EINVAL;

	ret = amd_fch_spi_wait_ready(priv);
	if (ret)
		return ret;

	writeb(op->cmd.opcode, priv->base + SPI_CMD_CODE);
	writeb(tx_len, priv->base + SPI_TX_BYTE_COUNT);
	writeb(rx_len, priv->base + SPI_RX_BYTE_COUNT);
	for (i = 0; i < tx_len; i++)
		writeb(tx[i], priv->base + SPI_FIFO + i);

	writeb(SPI_CMD_TRIGGER_EXECUTE, priv->base + SPI_CMD_TRIGGER);
	ret = amd_fch_spi_wait_ready(priv);
	if (ret) {
		log_err("command %02x timed out\n", op->cmd.opcode);
		return ret;
	}

	for (i = 0; i < rx_len; i++)
		((u8 *)op->data.buf.in)[i] =
			readb(priv->base + SPI_FIFO + tx_len + i);

	return 0;
}

static int amd_fch_spi_adjust_op_size(struct spi_slave *slave,
				      struct spi_mem_op *op)
{
	uint room = SPI_FIFO_DEPTH - op->addr.nbytes - op->dummy.nbytes;

	/* Keep page programs within the FIFO and away from page boundaries */
	if (op->data.dir == SPI_MEM_DATA_OUT)
		room = min(room, 64U);
	op->data.nbytes = min(op->data.nbytes, room);

	return 0;
}

static bool amd_fch_spi_supports_op(struct spi_slave *slave,
				    const struct spi_mem_op *op)
{
	/* Single-wire commands with a one-byte opcode only */
	if (op->cmd.nbytes != 1 || op->cmd.buswidth > 1 ||
	    op->addr.buswidth > 1 || op->dummy.buswidth > 1 ||
	    op->data.buswidth > 1 || op->cmd.dtr || op->addr.dtr ||
	    op->dummy.dtr || op->data.dtr)
		return false;

	return op->addr.nbytes + op->dummy.nbytes < SPI_FIFO_DEPTH;
}

static int amd_fch_spi_set_speed(struct udevice *bus, uint speed)
{
	/* coreboot has set the speeds up already */
	return 0;
}

static int amd_fch_spi_set_mode(struct udevice *bus, uint mode)
{
	return 0;
}

static int amd_fch_spi_probe(struct udevice *dev)
{
	struct amd_fch_spi_priv *priv = dev_get_priv(dev);
	ulong vendor, base;
	int ret;

	ret = pci_x86_read_config(LPC_BDF, PCI_VENDOR_ID, &vendor, PCI_SIZE_16);
	if (ret || vendor != PCI_VENDOR_ID_AMD)
		return -ENODEV;

	ret = pci_x86_read_config(LPC_BDF, LPC_SPI_BASE, &base, PCI_SIZE_32);
	if (ret)
		return ret;
	base &= ~(SPI_BASE_ALIGN - 1);
	if (!base)
		return -ENODEV;

	priv->base = (void __iomem *)base;
	priv->default_cs = readb(priv->base + SPI_ALT_CS) & SPI_ALT_CS_MASK;
	log_debug("SPI controller at %lx, CS%d\n", base, priv->default_cs);

	return 0;
}

static const struct spi_controller_mem_ops amd_fch_spi_mem_ops = {
	.adjust_op_size	= amd_fch_spi_adjust_op_size,
	.supports_op	= amd_fch_spi_supports_op,
	.exec_op	= amd_fch_spi_exec_op,
};

static const struct dm_spi_ops amd_fch_spi_ops = {
	.claim_bus	= amd_fch_spi_claim_bus,
	.release_bus	= amd_fch_spi_release_bus,
	.set_speed	= amd_fch_spi_set_speed,
	.set_mode	= amd_fch_spi_set_mode,
	.mem_ops	= &amd_fch_spi_mem_ops,
};

static const struct udevice_id amd_fch_spi_ids[] = {
	{ .compatible = "amd,fch-spi" },
	{ }
};

U_BOOT_DRIVER(amd_fch_spi) = {
	.name		= "amd_fch_spi",
	.id		= UCLASS_SPI,
	.of_match	= amd_fch_spi_ids,
	.ops		= &amd_fch_spi_ops,
	.priv_auto	= sizeof(struct amd_fch_spi_priv),
	.probe		= amd_fch_spi_probe,
};
