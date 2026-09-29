// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal MediaTek MT6789 clock driver for U-Boot.
 * Only the clocks needed by SSUSB (mtu3) are described:
 *   - topckgen  : CLK_TOP_USB_TOP_SEL  mux+gate @0x90 (CLK_CFG_9) bits[25:24]/bit31
 *   - infracfg  : CLK_IFRAO_SSUSB      gate (SETCLR) set 0xA4 clr 0xA8 sta 0xAC bit1
 *   - apmixedsys: placeholder driver (used as clock-parent fallback)
 *
 * The mux select bits are left untouched - the preloader already programs
 * them for the USB download path.
 */
#include <dm.h>
#include <dt-bindings/clock/mediatek,mt6789-clk.h>
#include "clk-mtk.h"

static const struct mtk_parent usb_parents[] = {
	TOP_PARENT(CLK_TOP_F26M),
	TOP_PARENT(CLK_TOP_UNIVPLL_D5_D4),
	TOP_PARENT(CLK_TOP_UNIVPLL_D6_D4),
	TOP_PARENT(CLK_TOP_UNIVPLL_D5_D2)
};

static const struct mtk_composite mt6789_top_muxes[] = {
	MUX_GATE(CLK_TOP_USB_TOP_SEL, usb_parents, 0x90, 24, 2, 31),
};

static const struct mtk_gate_regs infra_ao2_cg_regs = {
	.set_ofs = 0xA4,
	.clr_ofs = 0xA8,
	.sta_ofs = 0xAC,
};

static const struct mtk_gate mt6789_infra_gates[] = {
	{
		.id = CLK_IFRAO_SSUSB,
		.parent = CLK_TOP_USB_TOP_SEL,
		.regs = &infra_ao2_cg_regs,
		.shift = 1,
		.flags = CLK_GATE_SETCLR | CLK_PARENT_TOPCKGEN,
	},
};

static const struct mtk_clk_tree mt6789_clk_tree = {
	.muxes = mt6789_top_muxes,
	.num_muxes = ARRAY_SIZE(mt6789_top_muxes),
	.muxes_offs = CLK_TOP_USB_TOP_SEL,
	.gates = mt6789_infra_gates,
	.num_gates = ARRAY_SIZE(mt6789_infra_gates),
	.gates_offs = CLK_IFRAO_SSUSB,
};

static int mt6789_apmixedsys_probe(struct udevice *dev)
{
	return mtk_common_clk_init(dev, &mt6789_clk_tree);
}

static int mt6789_topckgen_probe(struct udevice *dev)
{
	return mtk_common_clk_init(dev, &mt6789_clk_tree);
}

static int mt6789_infracfg_probe(struct udevice *dev)
{
	return mtk_common_clk_gate_init(dev, &mt6789_clk_tree,
					mt6789_infra_gates,
					ARRAY_SIZE(mt6789_infra_gates),
					CLK_IFRAO_SSUSB);
}

static const struct udevice_id mt6789_apmixed_compat[] = {
	{ .compatible = "mediatek,mt6789-apmixedsys" },
	{ }
};

static const struct udevice_id mt6789_topckgen_compat[] = {
	{ .compatible = "mediatek,mt6789-topckgen" },
	{ }
};

static const struct udevice_id mt6789_infracfg_compat[] = {
	{ .compatible = "mediatek,mt6789-infracfg_ao" },
	{ }
};

U_BOOT_DRIVER(mtk_clk_apmixedsys) = {
	.name = "mtk_clk_apmixedsys",
	.id = UCLASS_CLK,
	.of_match = mt6789_apmixed_compat,
	.probe = mt6789_apmixedsys_probe,
	.priv_auto = sizeof(struct mtk_clk_priv),
	.ops = &mtk_clk_apmixedsys_ops,
	.flags = DM_FLAG_PRE_RELOC,
};

U_BOOT_DRIVER(mtk_clk_topckgen) = {
	.name = "mtk_clk_topckgen",
	.id = UCLASS_CLK,
	.of_match = mt6789_topckgen_compat,
	.probe = mt6789_topckgen_probe,
	.priv_auto = sizeof(struct mtk_clk_priv),
	.ops = &mtk_clk_topckgen_ops,
	.flags = DM_FLAG_PRE_RELOC,
};

U_BOOT_DRIVER(mtk_clk_infracfg) = {
	.name = "mtk_clk_infracfg",
	.id = UCLASS_CLK,
	.of_match = mt6789_infracfg_compat,
	.bind = dm_scan_fdt_dev,
	.probe = mt6789_infracfg_probe,
	.priv_auto = sizeof(struct mtk_cg_priv),
	.ops = &mtk_clk_gate_ops,
	.flags = DM_FLAG_PRE_RELOC,
};