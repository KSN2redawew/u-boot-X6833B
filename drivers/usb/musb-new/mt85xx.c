// SPDX-License-Identifier: GPL-2.0
/*
 * Mediatek "glue layer"
 *
 * Copyright (C) 2019-2021 by Mediatek
 * Based on the AllWinner SUNXI "glue layer" code.
 * Copyright (C) 2015 Hans de Goede <hdegoede@redhat.com>
 * Copyright (C) 2013 Jussi Kivilinna <jussi.kivilinna@iki.fi>
 *
 * This file is part of the Inventra Controller Driver for Linux.
 */
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <dm/lists.h>
#include <dm/root.h>
#include <generic-phy.h>
#include <linux/delay.h>
#include <linux/printk.h>
#include <linux/usb/musb.h>
#include <usb.h>
#include "linux-compat.h"
#include "musb_core.h"
#include "musb_uboot.h"

#define DBG_I(fmt, ...) \
	pr_info(fmt, ##__VA_ARGS__)

struct mtk_musb_config {
	struct musb_hdrc_config *config;
};

struct mtk_musb_glue {
	struct musb_host_data mdata;
	struct clk usbpllclk;
	struct clk usbmcuclk;
	struct clk usbclk;
	struct mtk_musb_config *cfg;
	struct device dev;
};

#define to_mtk_musb_glue(d)	container_of(d, struct mtk_musb_glue, dev)

static void mt85xx_phy_force_usb(void *com_base, ulong com);

/******************************************************************************
 * phy setup via generic PHY framework (t-phy on MT6789, 0x11f40000)
 ******************************************************************************/
static int mt_usb_phy_poweron(struct udevice *dev)
{
	struct phy phy;
	int ret;

	ret = generic_phy_get_by_index(dev, 0, &phy);
	if (ret) {
		printf("MUSB-PHY: get failed: %d\n", ret);
		return ret;
	}

	ret = generic_phy_set_mode(&phy, PHY_MODE_USB_DEVICE, 0);
	if (ret) {
		printf("MUSB-PHY: set_mode failed: %d\n", ret);
		return ret;
	}

	ret = generic_phy_power_on(&phy);
	if (ret) {
		printf("MUSB-PHY: power_on failed: %d\n", ret);
		return ret;
	}

	{
		/* read back PHY force/pull-up bank (v2: com=base+0x300, DTM1=+0x6c, ACR6=+0x18) */
		ulong pbase = (ulong)dev_read_addr_ptr(phy.dev);
		ulong com   = pbase ? pbase + 0x300 : 0;
		u32 dtm1 = com ? readl((void *)(com + 0x6c)) : 0xdeadbeef;
		u32 acr6 = com ? readl((void *)(com + 0x18)) : 0xdeadbeef;

		printf("MUSB-PHY: ok DTM1=%08x ACR6=%08x (com=%p)\n",
		       dtm1, acr6, (void *)com);

		if (com) {
			u32 d0 = readl((void *)(com + 0x68));

			printf("MUSB-PHY: DTM0=%08x (suspendm=%d force_suspendm=%d)\n",
			       d0, (d0 >> 3) & 1, (d0 >> 18) & 1);
			if (d0 & (BIT(18) | BIT(3))) {
				clrbits_le32((void *)(com + 0x68), BIT(18) | BIT(3));
				printf("MUSB-PHY: cleared SUSPENDM\n");
			}
		}

		mt85xx_phy_force_usb((void *)com, com);
	}

	return 0;
}

/******************************************************************************
 * Raw t-phy register force (absolute 32-bit writes + read-back verify)
 ******************************************************************************/
static void mt85xx_phy_force_usb(void *com_base, ulong com)
{
	u32 v, r;
	u32 d0, d1, a6;

	(void)com;

	/* DTM0 @ +0x68: XCVR=01 (full-speed), TERMSEL, no pulldowns, no force */
	v = 0x00000014;
	writel(v, com_base + 0x68);
	r = readl(com_base + 0x68);
	printf("MUSB-PHY: W[68]=%08x -> %08x\n", v, r);

	/* DTM1 @ +0x6c: pull-up + FORCE bits for device mode */
	v = 0x0000372f;
	writel(v, com_base + 0x6c);
	r = readl(com_base + 0x6c);
	printf("MUSB-PHY: W[6c]=%08x -> %08x\n", v, r);

	/* ACR6 @ +0x18: VBUSCMP_EN (bit20) + DISCTH/SQTH - restore VBUS detect */
	v = 0x00100042;
	writel(v, com_base + 0x18);
	r = readl(com_base + 0x18);
	printf("MUSB-PHY: W[18]=%08x -> %08x\n", v, r);

	printf("MUSB-PHY: forced USB mode, pll settled\n");

	d0 = readl(com_base + 0x68);
	d1 = readl(com_base + 0x6c);
	a6 = readl(com_base + 0x18);
	printf("MUSB-PHY: after DTM0=%08x DTM1=%08x ACR6=%08x\n", d0, d1, a6);
}

/******************************************************************************
 * MUSB Glue code
 ******************************************************************************/

static irqreturn_t mtk_musb_interrupt(int irq, void *__hci)
{
	struct musb		*musb = __hci;
	irqreturn_t		retval = IRQ_NONE;

	/* read and flush interrupts */
	musb->int_usb = musb_readb(musb->mregs, MUSB_INTRUSB);
//	last_int_usb = musb->int_usb;
	if (musb->int_usb)
		musb_writeb(musb->mregs, MUSB_INTRUSB, musb->int_usb);
	musb->int_tx = musb_readw(musb->mregs, MUSB_INTRTX);
	if (musb->int_tx)
		musb_writew(musb->mregs, MUSB_INTRTX, musb->int_tx);
	musb->int_rx = musb_readw(musb->mregs, MUSB_INTRRX);
	if (musb->int_rx)
		musb_writew(musb->mregs, MUSB_INTRRX, musb->int_rx);

	if (musb->int_usb || musb->int_tx || musb->int_rx)
		retval |= musb_interrupt(musb);

	return retval;
}

/* musb_core does not call enable / disable in a balanced manner <sigh> */
static bool enabled;

static int mtk_musb_enable(struct musb *musb)
{
	struct mtk_musb_glue *glue = to_mtk_musb_glue(musb->controller);

	DBG_I("%s():\n", __func__);

	musb_ep_select(musb->mregs, 0);
	musb_writeb(musb->mregs, MUSB_FADDR, 0);

	if (enabled)
		return 0;

	enabled = true;

	return 0;
}

static void mtk_musb_disable(struct musb *musb)
{
	struct mtk_musb_glue *glue = to_mtk_musb_glue(musb->controller);

	DBG_I("%s():\n", __func__);

	if (!enabled)
		return;

	enabled = false;
}

static int mtk_musb_init(struct musb *musb)
{
	struct mtk_musb_glue *glue = to_mtk_musb_glue(musb->controller);
	int ret;

	DBG_I("%s():\n", __func__);

	ret = clk_enable(&glue->usbpllclk);
	if (ret) {
		dev_err(musb->controller, "failed to enable usbpll clock\n");
		return ret;
	}
	ret = clk_enable(&glue->usbmcuclk);
	if (ret) {
		dev_err(musb->controller, "failed to enable usbmcu clock\n");
		return ret;
	}
	ret = clk_enable(&glue->usbclk);
	if (ret) {
		dev_err(musb->controller, "failed to enable usb clock\n");
		return ret;
	}

	musb->isr = mtk_musb_interrupt;

	return 0;
}

static int mtk_musb_exit(struct musb *musb)
{
	struct mtk_musb_glue *glue = to_mtk_musb_glue(musb->controller);

	clk_disable(&glue->usbclk);
	clk_disable(&glue->usbmcuclk);
	clk_disable(&glue->usbpllclk);

	return 0;
}

static const struct musb_platform_ops mtk_musb_ops = {
	.init		= mtk_musb_init,
	.exit		= mtk_musb_exit,
	.enable		= mtk_musb_enable,
	.disable	= mtk_musb_disable,
};

/* MTK OTG supports up to 7 endpoints */
#define MTK_MUSB_MAX_EP_NUM		8
#define MTK_MUSB_RAM_BITS		16

static struct musb_fifo_cfg mtk_musb_mode_cfg[] = {
	MUSB_EP_FIFO_SINGLE(1, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(1, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(2, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(2, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(3, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(3, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(4, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(4, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(5, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(5, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(6, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(6, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(7, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(7, FIFO_RX, 512),
};

static struct musb_hdrc_config musb_config = {
	.fifo_cfg       = mtk_musb_mode_cfg,
	.fifo_cfg_size  = ARRAY_SIZE(mtk_musb_mode_cfg),
	.multipoint	= true,
	.dyn_fifo	= true,
	.num_eps	= MTK_MUSB_MAX_EP_NUM,
	.ram_bits	= MTK_MUSB_RAM_BITS,
};

static int musb_usb_probe(struct udevice *dev)
{
	struct mtk_musb_glue *glue = dev_get_priv(dev);
	struct musb_host_data *host = &glue->mdata;
	struct musb_hdrc_platform_data pdata;
	void *base = dev_read_addr_ptr(dev);
	int ret;

	DBG_I("%s():\n", __func__);

#ifdef CONFIG_USB_MUSB_HOST
	struct usb_bus_priv *priv = dev_get_uclass_priv(dev);
#endif

	if (!base)
		return -EINVAL;

	printf("MUSB-PROBE: base=%p\n", base);

	glue->cfg = (struct mtk_musb_config *)dev_get_driver_data(dev);
	if (!glue->cfg) {
		printf("MUSB-PROBE: no driver data\n");
		return -EINVAL;
	}

	ret = clk_get_by_name(dev, "usbpll", &glue->usbpllclk);
	if (ret) {
		dev_err(dev, "failed to get usbpll clock\n");
		return ret;
	}
	ret = clk_get_by_name(dev, "usbmcu", &glue->usbmcuclk);
	if (ret) {
		dev_err(dev, "failed to get usbmcu clock\n");
		return ret;
	}
	ret = clk_get_by_name(dev, "usb", &glue->usbclk);
	if (ret) {
		dev_err(dev, "failed to get usb clock\n");
		return ret;
	}
	printf("MUSB-PROBE: clks ok\n");

	memset(&pdata, 0, sizeof(pdata));
	pdata.power = (u8)400;
	pdata.platform_ops = &mtk_musb_ops;
	pdata.config = glue->cfg->config;

#ifdef CONFIG_USB_MUSB_HOST
	priv->desc_before_addr = true;

	pdata.mode = MUSB_HOST;
	host->host = musb_init_controller(&pdata, &glue->dev, base);
	if (!host->host)
		return -EIO;

	ret = musb_lowlevel_init(host);
	if (!ret)
		printf("MTK MUSB OTG (Host)\n");
#else
	pdata.mode = MUSB_PERIPHERAL;
	printf("MUSB-PROBE: musb_register(mode=%d)...\n", pdata.mode);
	host->host = musb_register(&pdata, &glue->dev, base);
	if (!host->host) {
		printf("MUSB-PROBE: musb_register failed\n");
		return -EIO;
	}

	printf("MTK MUSB OTG (Peripheral)\n");
#endif

	ret = mt_usb_phy_poweron(dev);

	return ret;
}

static int musb_usb_remove(struct udevice *dev)
{
	struct mtk_musb_glue *glue = dev_get_priv(dev);
	struct musb_host_data *host = &glue->mdata;

	musb_stop(host->host);
	free(host->host);
	host->host = NULL;

	return 0;
}

static const struct mtk_musb_config mt8518_cfg = {
	.config = &musb_config,
};

#if defined(CONFIG_USB_MUSB_GADGET) && !CONFIG_IS_ENABLED(DM_USB_HOST)
static const struct usb_gadget_generic_ops mtk_musb_gadget_ops = {
	.handle_interrupts = musb_gadget_handle_interrupts,
};
#endif

static const struct udevice_id mtk_musb_ids[] = {
	{ .compatible = "mediatek,mt8518-musb",
	  .data = (ulong)&mt8518_cfg },
	{ }
};

U_BOOT_DRIVER(mtk_musb) = {
	.name		= "mtk_musb",
#ifdef CONFIG_USB_MUSB_HOST
	.id		= UCLASS_USB,
#else
	.id		= UCLASS_USB_GADGET_GENERIC,
#endif
	.of_match	= mtk_musb_ids,
	.probe		= musb_usb_probe,
	.remove		= musb_usb_remove,
#ifdef CONFIG_USB_MUSB_HOST
	.ops		= &musb_usb_ops,
#else
	.ops		= &mtk_musb_gadget_ops,
#endif
	.plat_auto	= sizeof(struct usb_plat),
	.priv_auto	= sizeof(struct mtk_musb_glue),
};
