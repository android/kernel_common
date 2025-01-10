// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 MediaTek Inc.
 * Author: James Liao <jamesjj.liao@mediatek.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset-controller.h>
#include <linux/soc/mediatek/mtk-mmsys.h>

#include "mtk-mmsys.h"
#include "mt8167-mmsys.h"
#include "mt8173-mmsys.h"
#include "mt8183-mmsys.h"
#include "mt8186-mmsys.h"
#include "mt8188-mmsys.h"
#include "mt8192-mmsys.h"
#include "mt8195-mmsys.h"
#include "mt8196-mmsys.h"
#include "mt8365-mmsys.h"

#define MMSYS_SW_RESET_PER_REG 32

static const struct mtk_mmsys_driver_data mt2701_mmsys_driver_data = {
	.clk_driver = "clk-mt2701-mm",
	.routes = mmsys_default_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_default_routing_table),
};

static const struct mtk_mmsys_driver_data mt2712_mmsys_driver_data = {
	.clk_driver = "clk-mt2712-mm",
	.routes = mmsys_default_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_default_routing_table),
};

static const struct mtk_mmsys_driver_data mt6779_mmsys_driver_data = {
	.clk_driver = "clk-mt6779-mm",
};

static const struct mtk_mmsys_driver_data mt6795_mmsys_driver_data = {
	.clk_driver = "clk-mt6795-mm",
	.routes = mt8173_mmsys_routing_table,
	.num_routes = ARRAY_SIZE(mt8173_mmsys_routing_table),
	.sw0_rst_offset = MT8183_MMSYS_SW0_RST_B,
	.num_resets = 64,
};

static const struct mtk_mmsys_driver_data mt6797_mmsys_driver_data = {
	.clk_driver = "clk-mt6797-mm",
};

static const struct mtk_mmsys_driver_data mt8167_mmsys_driver_data = {
	.clk_driver = "clk-mt8167-mm",
	.routes = mt8167_mmsys_routing_table,
	.num_routes = ARRAY_SIZE(mt8167_mmsys_routing_table),
};

static const struct mtk_mmsys_driver_data mt8173_mmsys_driver_data = {
	.clk_driver = "clk-mt8173-mm",
	.routes = mt8173_mmsys_routing_table,
	.num_routes = ARRAY_SIZE(mt8173_mmsys_routing_table),
	.sw0_rst_offset = MT8183_MMSYS_SW0_RST_B,
	.num_resets = 64,
};

static const struct mtk_mmsys_driver_data mt8183_mmsys_driver_data = {
	.clk_driver = "clk-mt8183-mm",
	.routes = mmsys_mt8183_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8183_routing_table),
	.sw0_rst_offset = MT8183_MMSYS_SW0_RST_B,
	.num_resets = 32,
	.mdp_isp_ctrl[ISP_CTRL_CAM1] = mmsys_mt8183_mdp_isp_ctrl_table1,
	.mdp_isp_ctrl[ISP_CTRL_CAM2] = mmsys_mt8183_mdp_isp_ctrl_table2,
};

static const struct mtk_mmsys_driver_data mt8186_mmsys_driver_data = {
	.clk_driver = "clk-mt8186-mm",
	.routes = mmsys_mt8186_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8186_routing_table),
	.sw0_rst_offset = MT8186_MMSYS_SW0_RST_B,
	.num_resets = 32,
};

static const struct mtk_mmsys_driver_data mt8188_vdosys0_driver_data = {
	.clk_driver = "clk-mt8188-vdo0",
	.routes = mmsys_mt8188_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8188_routing_table),
	.sw0_rst_offset = MT8188_VDO0_SW0_RST_B,
	.rst_tb = mmsys_mt8188_vdo0_rst_tb,
	.num_resets = ARRAY_SIZE(mmsys_mt8188_vdo0_rst_tb),
};

static const struct mtk_mmsys_driver_data mt8188_vdosys1_driver_data = {
	.clk_driver = "clk-mt8188-vdo1",
	.routes = mmsys_mt8188_vdo1_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8188_vdo1_routing_table),
	.sw0_rst_offset = MT8188_VDO1_SW0_RST_B,
	.rst_tb = mmsys_mt8188_vdo1_rst_tb,
	.num_resets = ARRAY_SIZE(mmsys_mt8188_vdo1_rst_tb),
	.vsync_len = 1,
};

static const struct mtk_mmsys_driver_data mt8188_vppsys0_driver_data = {
	.clk_driver = "clk-mt8188-vpp0",
	.is_vppsys = true,
};

static const struct mtk_mmsys_driver_data mt8188_vppsys1_driver_data = {
	.clk_driver = "clk-mt8188-vpp1",
	.is_vppsys = true,
};

static const struct mtk_mmsys_driver_data mt8192_mmsys_driver_data = {
	.clk_driver = "clk-mt8192-mm",
	.routes = mmsys_mt8192_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8192_routing_table),
	.sw0_rst_offset = MT8186_MMSYS_SW0_RST_B,
	.num_resets = 32,
};

static const struct mtk_mmsys_driver_data mt8195_vdosys0_driver_data = {
	.clk_driver = "clk-mt8195-vdo0",
	.routes = mmsys_mt8195_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8195_routing_table),
};

static const struct mtk_mmsys_driver_data mt8195_vdosys1_driver_data = {
	.clk_driver = "clk-mt8195-vdo1",
	.routes = mmsys_mt8195_vdo1_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8195_vdo1_routing_table),
	.sw0_rst_offset = MT8195_VDO1_SW0_RST_B,
	.num_resets = 64,
};

static const struct mtk_mmsys_driver_data mt8195_vppsys0_driver_data = {
	.clk_driver = "clk-mt8195-vpp0",
	.is_vppsys = true,
};

static const struct mtk_mmsys_driver_data mt8195_vppsys1_driver_data = {
	.clk_driver = "clk-mt8195-vpp1",
	.is_vppsys = true,
};

static const struct mtk_mmsys_driver_data mt8196_dispsys0_driver_data = {
	.clk_driver = "clk-mt8196-disp0",
	.routes = mmsys_mt8196_disp0_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8196_disp0_routing_table),
	.async_info = mmsys_mt8196_disp0_async_comp_table,
	.num_async_info = ARRAY_SIZE(mmsys_mt8196_disp0_async_comp_table),
	.def_config = mmsys_mt8196_disp0_default_table,
	.num_def_config = ARRAY_SIZE(mmsys_mt8196_disp0_default_table),
	.num_top_clk = 1,
};

static const struct mtk_mmsys_driver_data mt8196_dispsys1_driver_data = {
	.clk_driver = "clk-mt8196-disp1",
	.routes = mmsys_mt8196_disp1_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8196_disp1_routing_table),
	.async_info = mmsys_mt8196_disp1_async_comp_table,
	.num_async_info = ARRAY_SIZE(mmsys_mt8196_disp1_async_comp_table),
	.def_config = mmsys_mt8196_disp1_default_table,
	.num_def_config = ARRAY_SIZE(mmsys_mt8196_disp1_default_table),
	.num_top_clk = 1,
};

static const struct mtk_mmsys_driver_data mt8196_ovlsys0_driver_data = {
	.clk_driver = "clk-mt8196-ovl0",
	.routes = mmsys_mt8196_ovl0_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8196_ovl0_routing_table),
	.async_info = mmsys_mt8196_ovl0_async_comp_table,
	.num_async_info = ARRAY_SIZE(mmsys_mt8196_ovl0_async_comp_table),
	.def_config = mmsys_mt8196_ovl0_default_table,
	.num_def_config = ARRAY_SIZE(mmsys_mt8196_ovl0_default_table),
};

static const struct mtk_mmsys_driver_data mt8196_ovlsys1_driver_data = {
	.clk_driver = "clk-mt8196-ovl1",
	.routes = mmsys_mt8196_ovl1_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8196_ovl1_routing_table),
	.async_info = mmsys_mt8196_ovl1_async_comp_table,
	.num_async_info = ARRAY_SIZE(mmsys_mt8196_ovl1_async_comp_table),
	.def_config = mmsys_mt8196_ovl0_default_table,
	.num_def_config = ARRAY_SIZE(mmsys_mt8196_ovl0_default_table),
};

static const struct mtk_mmsys_driver_data mt8196_vdisp_ao_driver_data = {
	.clk_driver = "clk-mt8196-vdisp_ao",
	.def_config = mmsys_mt8196_vdisp_ao_default_table,
	.num_def_config = ARRAY_SIZE(mmsys_mt8196_vdisp_ao_default_table),
};

static const struct mtk_mmsys_driver_data mt8365_mmsys_driver_data = {
	.clk_driver = "clk-mt8365-mm",
	.routes = mt8365_mmsys_routing_table,
	.num_routes = ARRAY_SIZE(mt8365_mmsys_routing_table),
};

struct mtk_mmsys {
	void __iomem *regs;
	const struct mtk_mmsys_driver_data *data;
	struct platform_device *clks_pdev;
	struct platform_device *drm_pdev;
	spinlock_t lock; /* protects mmsys_sw_rst_b reg */
	struct reset_controller_dev rcdev;
	struct cmdq_client_reg cmdq_base;
	struct clk **async_clk;
	int num_async_clk;
	struct clk **top_clk;
};

static void mtk_mmsys_update_bits(struct mtk_mmsys *mmsys, u32 offset, u32 mask, u32 val,
				  struct cmdq_pkt *cmdq_pkt)
{
	int ret;
	u32 tmp;

	if (mmsys->cmdq_base.size && cmdq_pkt) {
		offset += mmsys->cmdq_base.offset;
		if (mmsys->cmdq_base.subsys != CMDQ_SUBSYS_INVALID) {
			ret = cmdq_pkt_write_mask(cmdq_pkt, mmsys->cmdq_base.subsys,
						  offset, val, mask);
		} else {
			/* only MMIO access, no need to check mminfro_offset */
			ret = cmdq_pkt_assign(cmdq_pkt, CMDQ_THR_SPR_IDX0,
					      CMDQ_ADDR_HIGH(mmsys->cmdq_base.pa_base));
			ret |= cmdq_pkt_write_s_mask_value(cmdq_pkt, CMDQ_THR_SPR_IDX0,
							   CMDQ_ADDR_LOW(offset), val, mask);
		}
		if (ret)
			pr_debug("CMDQ unavailable: using CPU write\n");
		else
			return;
	}
	tmp = readl_relaxed(mmsys->regs + offset);
	tmp = (tmp & ~mask) | (val & mask);
	writel_relaxed(tmp, mmsys->regs + offset);
}

int mtk_mmsys_top_clk_enable(struct device *dev)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);
	int ret, i;

	if (!mmsys->data->num_top_clk)
		return 0;

	for (i = 0; i < mmsys->data->num_top_clk; i++)
		ret = clk_prepare_enable(mmsys->top_clk[i]);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_mmsys_top_clk_enable);

void mtk_mmsys_top_clk_disable(struct device *dev)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < mmsys->data->num_top_clk; i++)
		clk_disable_unprepare(mmsys->top_clk[i]);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_top_clk_disable);

int mtk_mmsys_ddp_clk_enable(struct device *dev, enum mtk_ddp_comp_id comp_id)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);
	const struct mtk_mmsys_async_info *async = mmsys->data->async_info;

	int i;

	if (!mmsys->data->num_async_info)
		return 0;

	for (i = 0; i < mmsys->data->num_async_info; i++)
		if (comp_id == async[i].comp_id)
			return clk_prepare_enable(mmsys->async_clk[async[i].index]);
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_mmsys_ddp_clk_enable);

void mtk_mmsys_ddp_clk_disable(struct device *dev, enum mtk_ddp_comp_id comp_id)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);
	const struct mtk_mmsys_async_info *async = mmsys->data->async_info;
	int i;

	if (!mmsys->data->num_async_info)
		return;

	for (i = 0; i < mmsys->data->num_async_info; i++)
		if (comp_id == async[i].comp_id)
			clk_disable_unprepare(mmsys->async_clk[async[i].index]);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_ddp_clk_disable);

void mtk_mmsys_ddp_config(struct device *dev, enum mtk_ddp_comp_id comp_id,
			  int width, int height, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);
	const struct mtk_mmsys_async_info *async = mmsys->data->async_info;
	int i;

	if (!mmsys->data->num_async_info)
		return;

	for (i = 0; i < mmsys->data->num_async_info; i++)
		if (comp_id == async[i].comp_id)
			break;

	if (i == mmsys->data->num_async_info)
		return;

	mtk_mmsys_update_bits(mmsys, async[i].offset, async[i].mask,
			      height << 16 | width, cmdq_pkt);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_ddp_config);

void mtk_mmsys_default_config(struct device *dev)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);
	const struct mtk_mmsys_default *def_config = mmsys->data->def_config;
	int i;

	if (!mmsys->data->num_def_config)
		return;

	for (i = 0; i < mmsys->data->num_def_config; i++)
		mtk_mmsys_update_bits(mmsys, def_config[i].offset, def_config[i].mask,
				      def_config[i].val, NULL);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_default_config);

void mtk_mmsys_ddp_connect(struct device *dev,
			   enum mtk_ddp_comp_id cur,
			   enum mtk_ddp_comp_id next)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);
	const struct mtk_mmsys_routes *routes = mmsys->data->routes;
	int i;

	for (i = 0; i < mmsys->data->num_routes; i++)
		if (cur == routes[i].from_comp && next == routes[i].to_comp)
			mtk_mmsys_update_bits(mmsys, routes[i].addr, routes[i].mask,
					      routes[i].val, NULL);

	if (mmsys->data->vsync_len)
		mtk_mmsys_update_bits(mmsys, MT8188_VDO1_MIXER_VSYNC_LEN, GENMASK(31, 0),
				      mmsys->data->vsync_len, NULL);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_ddp_connect);

void mtk_mmsys_ddp_disconnect(struct device *dev,
			      enum mtk_ddp_comp_id cur,
			      enum mtk_ddp_comp_id next)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);
	const struct mtk_mmsys_routes *routes = mmsys->data->routes;
	int i;

	for (i = 0; i < mmsys->data->num_routes; i++)
		if (cur == routes[i].from_comp && next == routes[i].to_comp)
			mtk_mmsys_update_bits(mmsys, routes[i].addr, routes[i].mask, 0, NULL);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_ddp_disconnect);

void mtk_mmsys_merge_async_config(struct device *dev, int idx, int width, int height,
				  struct cmdq_pkt *cmdq_pkt)
{
	mtk_mmsys_update_bits(dev_get_drvdata(dev), MT8195_VDO1_MERGE0_ASYNC_CFG_WD + 0x10 * idx,
			      ~0, height << 16 | width, cmdq_pkt);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_merge_async_config);

void mtk_mmsys_hdr_config(struct device *dev, int be_width, int be_height,
			  struct cmdq_pkt *cmdq_pkt)
{
	mtk_mmsys_update_bits(dev_get_drvdata(dev), MT8195_VDO1_HDRBE_ASYNC_CFG_WD, ~0,
			      be_height << 16 | be_width, cmdq_pkt);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_hdr_config);

void mtk_mmsys_mixer_in_config(struct device *dev, int idx, bool alpha_sel, u16 alpha,
			       u8 mode, u32 biwidth, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);

	mtk_mmsys_update_bits(mmsys, MT8195_VDO1_MIXER_IN1_ALPHA + (idx - 1) * 4, ~0,
			      alpha << 16 | alpha, cmdq_pkt);
	mtk_mmsys_update_bits(mmsys, MT8195_VDO1_HDR_TOP_CFG, BIT(15 + idx), 0, cmdq_pkt);
	mtk_mmsys_update_bits(mmsys, MT8195_VDO1_HDR_TOP_CFG, BIT(19 + idx),
			      alpha_sel << (19 + idx), cmdq_pkt);
	mtk_mmsys_update_bits(mmsys, MT8195_VDO1_MIXER_IN1_PAD + (idx - 1) * 4,
			      GENMASK(31, 16) | GENMASK(1, 0), biwidth << 16 | mode, cmdq_pkt);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_mixer_in_config);

void mtk_mmsys_mixer_in_channel_swap(struct device *dev, int idx, bool channel_swap,
				     struct cmdq_pkt *cmdq_pkt)
{
	mtk_mmsys_update_bits(dev_get_drvdata(dev), MT8195_VDO1_MIXER_IN1_PAD + (idx - 1) * 4,
			      BIT(4), channel_swap << 4, cmdq_pkt);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_mixer_in_channel_swap);

void mtk_mmsys_ddp_dpi_fmt_config(struct device *dev, u32 val)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);

	switch (val) {
	case MTK_DPI_RGB888_SDR_CON:
		mtk_mmsys_update_bits(mmsys, MT8186_MMSYS_DPI_OUTPUT_FORMAT,
				      MT8186_DPI_FORMAT_MASK, MT8186_DPI_RGB888_SDR_CON, NULL);
		break;
	case MTK_DPI_RGB565_SDR_CON:
		mtk_mmsys_update_bits(mmsys, MT8186_MMSYS_DPI_OUTPUT_FORMAT,
				      MT8186_DPI_FORMAT_MASK, MT8186_DPI_RGB565_SDR_CON, NULL);
		break;
	case MTK_DPI_RGB565_DDR_CON:
		mtk_mmsys_update_bits(mmsys, MT8186_MMSYS_DPI_OUTPUT_FORMAT,
				      MT8186_DPI_FORMAT_MASK, MT8186_DPI_RGB565_DDR_CON, NULL);
		break;
	case MTK_DPI_RGB888_DDR_CON:
	default:
		mtk_mmsys_update_bits(mmsys, MT8186_MMSYS_DPI_OUTPUT_FORMAT,
				      MT8186_DPI_FORMAT_MASK, MT8186_DPI_RGB888_DDR_CON, NULL);
		break;
	}
}
EXPORT_SYMBOL_GPL(mtk_mmsys_ddp_dpi_fmt_config);

void mtk_mmsys_vpp_rsz_merge_config(struct device *dev, u32 id, bool enable,
				    struct cmdq_pkt *cmdq_pkt)
{
	u32 reg;

	switch (id) {
	case 2:
		reg = MT8195_SVPP2_BUF_BF_RSZ_SWITCH;
		break;
	case 3:
		reg = MT8195_SVPP3_BUF_BF_RSZ_SWITCH;
		break;
	default:
		dev_err(dev, "Invalid id %d\n", id);
		return;
	}

	mtk_mmsys_update_bits(dev_get_drvdata(dev), reg, ~0, enable, cmdq_pkt);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_vpp_rsz_merge_config);

void mtk_mmsys_vpp_rsz_dcm_config(struct device *dev, bool enable,
				  struct cmdq_pkt *cmdq_pkt)
{
	u32 client;

	client = MT8195_SVPP1_MDP_RSZ;
	mtk_mmsys_update_bits(dev_get_drvdata(dev),
			      MT8195_VPP1_HW_DCM_1ST_DIS0, client,
			      ((enable) ? client : 0), cmdq_pkt);
	mtk_mmsys_update_bits(dev_get_drvdata(dev),
			      MT8195_VPP1_HW_DCM_2ND_DIS0, client,
			      ((enable) ? client : 0), cmdq_pkt);

	client = MT8195_SVPP2_MDP_RSZ | MT8195_SVPP3_MDP_RSZ;
	mtk_mmsys_update_bits(dev_get_drvdata(dev),
			      MT8195_VPP1_HW_DCM_1ST_DIS1, client,
			      ((enable) ? client : 0), cmdq_pkt);
	mtk_mmsys_update_bits(dev_get_drvdata(dev),
			      MT8195_VPP1_HW_DCM_2ND_DIS1, client,
			      ((enable) ? client : 0), cmdq_pkt);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_vpp_rsz_dcm_config);

void mtk_mmsys_mdp_isp_ctrl(struct device *dev, struct cmdq_pkt *cmdq_pkt,
			    enum mtk_isp_ctrl idx)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);
	const unsigned int *isp_ctrl;
	u32 ofst;

	if (idx >= ISP_CTRL_MAX) {
		dev_err(dev, "Invalid idx %d\n", idx);
		return;
	}

	isp_ctrl = mmsys->data->mdp_isp_ctrl[idx];

	/* Reset MDP_DL_ASYNC_TX */
	ofst = isp_ctrl[ISP_REG_MMSYS_SW0_RST_B];
	mtk_mmsys_update_bits(mmsys, ofst, isp_ctrl[ISP_BIT_MDP_DL_ASYNC_TX],
			      0x0, cmdq_pkt);
	mtk_mmsys_update_bits(mmsys, ofst, isp_ctrl[ISP_BIT_MDP_DL_ASYNC_TX],
			      isp_ctrl[ISP_BIT_MDP_DL_ASYNC_TX], cmdq_pkt);

	/* Reset MDP_DL_ASYNC_RX */
	ofst = isp_ctrl[ISP_REG_MMSYS_SW1_RST_B];
	mtk_mmsys_update_bits(mmsys, ofst, isp_ctrl[ISP_BIT_MDP_DL_ASYNC_RX],
			      0x0, cmdq_pkt);
	mtk_mmsys_update_bits(mmsys, ofst, isp_ctrl[ISP_BIT_MDP_DL_ASYNC_RX],
			      isp_ctrl[ISP_BIT_MDP_DL_ASYNC_RX], cmdq_pkt);

	/* Enable sof mode */
	ofst = isp_ctrl[ISP_REG_ISP_RELAY_CFG_WD];
	mtk_mmsys_update_bits(mmsys, ofst, isp_ctrl[ISP_BIT_NO_SOF_MODE],
			      0x0, cmdq_pkt);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_mdp_isp_ctrl);

void mtk_mmsys_mdp_camin_ctrl(struct device *dev, struct cmdq_pkt *cmdq_pkt,
			      enum mtk_isp_ctrl idx, u32 camin_w, u32 camin_h)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);
	const unsigned int *isp_ctrl;
	u32 ofst;

	if (idx >= ISP_CTRL_MAX) {
		dev_err(dev, "Invalid idx %d\n", idx);
		return;
	}

	isp_ctrl = mmsys->data->mdp_isp_ctrl[idx];

	/* Config for direct link */

	ofst = isp_ctrl[ISP_REG_MDP_ASYNC_CFG_WD];
	mtk_mmsys_update_bits(mmsys, ofst, 0x3FFF3FFF,
			      (camin_h << 16) + camin_w, cmdq_pkt);

	ofst = isp_ctrl[ISP_REG_ISP_RELAY_CFG_WD];
	mtk_mmsys_update_bits(mmsys, ofst, 0x3FFF3FFF,
			      (camin_h << 16) + camin_w, cmdq_pkt);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_mdp_camin_ctrl);

static int mtk_mmsys_reset_update(struct reset_controller_dev *rcdev, unsigned long id,
				  bool assert)
{
	struct mtk_mmsys *mmsys = container_of(rcdev, struct mtk_mmsys, rcdev);
	unsigned long flags;
	u32 offset;
	u32 reg;

	if (mmsys->data->rst_tb) {
		if (id >= mmsys->data->num_resets) {
			dev_err(rcdev->dev, "Invalid reset ID: %lu (>=%u)\n",
				id, mmsys->data->num_resets);
			return -EINVAL;
		}
		id = mmsys->data->rst_tb[id];
	}

	offset = (id / MMSYS_SW_RESET_PER_REG) * sizeof(u32);
	id = id % MMSYS_SW_RESET_PER_REG;
	reg = mmsys->data->sw0_rst_offset + offset;

	spin_lock_irqsave(&mmsys->lock, flags);

	if (assert)
		mtk_mmsys_update_bits(mmsys, reg, BIT(id), 0, NULL);
	else
		mtk_mmsys_update_bits(mmsys, reg, BIT(id), BIT(id), NULL);

	spin_unlock_irqrestore(&mmsys->lock, flags);

	return 0;
}

static int mtk_mmsys_reset_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	return mtk_mmsys_reset_update(rcdev, id, true);
}

static int mtk_mmsys_reset_deassert(struct reset_controller_dev *rcdev, unsigned long id)
{
	return mtk_mmsys_reset_update(rcdev, id, false);
}

static int mtk_mmsys_reset(struct reset_controller_dev *rcdev, unsigned long id)
{
	int ret;

	ret = mtk_mmsys_reset_assert(rcdev, id);
	if (ret)
		return ret;

	usleep_range(1000, 1100);

	return mtk_mmsys_reset_deassert(rcdev, id);
}

static const struct reset_control_ops mtk_mmsys_reset_ops = {
	.assert = mtk_mmsys_reset_assert,
	.deassert = mtk_mmsys_reset_deassert,
	.reset = mtk_mmsys_reset,
};

static int mtk_mmsys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct platform_device *clks;
	struct platform_device *drm;
	struct mtk_mmsys *mmsys;
	int ret, i;

	mmsys = devm_kzalloc(dev, sizeof(*mmsys), GFP_KERNEL);
	if (!mmsys)
		return -ENOMEM;

	mmsys->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mmsys->regs)) {
		ret = PTR_ERR(mmsys->regs);
		dev_err(dev, "Failed to ioremap mmsys registers: %d\n", ret);
		return ret;
	}

	mmsys->data = of_device_get_match_data(&pdev->dev);

	if (mmsys->data->num_resets > 0) {
		spin_lock_init(&mmsys->lock);

		mmsys->rcdev.owner = THIS_MODULE;
		mmsys->rcdev.nr_resets = mmsys->data->num_resets;
		mmsys->rcdev.ops = &mtk_mmsys_reset_ops;
		mmsys->rcdev.of_node = pdev->dev.of_node;
		ret = devm_reset_controller_register(&pdev->dev, &mmsys->rcdev);
		if (ret) {
			dev_err(&pdev->dev, "Couldn't register mmsys reset controller: %d\n", ret);
			return ret;
		}
	}

	/* CMDQ is optional */
	ret = cmdq_dev_get_client_reg(dev, &mmsys->cmdq_base, 0);
	if (ret)
		dev_dbg(dev, "No mediatek,gce-client-reg!\n");

	platform_set_drvdata(pdev, mmsys);

	clks = platform_device_register_data(&pdev->dev, mmsys->data->clk_driver,
					     PLATFORM_DEVID_AUTO, NULL, 0);
	if (IS_ERR(clks))
		return PTR_ERR(clks);
	mmsys->clks_pdev = clks;

	if (mmsys->data->num_top_clk) {
		struct device_node *node;

		node = of_get_child_by_name(dev->of_node, "top");
		if (!node) {
			dev_err(&pdev->dev, "Couldn't find top node\n");
			return -EINVAL;
		}

		mmsys->top_clk = devm_kmalloc_array(dev, mmsys->data->num_top_clk,
						    sizeof(*mmsys->top_clk), GFP_KERNEL);
		if (!mmsys->top_clk)
			return -ENOMEM;

		for (i = 0; i < mmsys->data->num_top_clk; i++) {
			mmsys->top_clk[i] = of_clk_get(node, i);
			if (IS_ERR(mmsys->top_clk[i]))
				return PTR_ERR(mmsys->top_clk[i]);
		}
	}

	if (mmsys->data->num_async_info) {
		struct device_node *node;

		node = of_get_child_by_name(dev->of_node, "async");
		if (!node) {
			dev_err(&pdev->dev, "Couldn't find async node\n");
			return -EINVAL;
		}

		mmsys->async_clk = devm_kmalloc_array(dev, mmsys->data->num_async_info,
						      sizeof(*mmsys->async_clk), GFP_KERNEL);
		if (!mmsys->async_clk)
			return -ENOMEM;
		mmsys->num_async_clk = mmsys->data->num_async_info;

		for (i = 0; i < mmsys->num_async_clk; i++) {
			mmsys->async_clk[i] = of_clk_get(node, i);
			if (IS_ERR(mmsys->async_clk[i]))
				return PTR_ERR(mmsys->async_clk[i]);
		}
	}

	if (mmsys->data->is_vppsys)
		goto out_probe_done;

	drm = platform_device_register_data(&pdev->dev, "mediatek-drm",
					    PLATFORM_DEVID_AUTO, NULL, 0);
	if (IS_ERR(drm)) {
		platform_device_unregister(clks);
		return PTR_ERR(drm);
	}
	mmsys->drm_pdev = drm;

	if (of_property_present(dev->of_node, "power-domains"))
		pm_runtime_enable(dev);

out_probe_done:
	return 0;
}

static int mtk_mmsys_remove(struct platform_device *pdev)
{
	struct mtk_mmsys *mmsys = platform_get_drvdata(pdev);

	platform_device_unregister(mmsys->drm_pdev);
	platform_device_unregister(mmsys->clks_pdev);

	if (of_property_present(pdev->dev.of_node, "power-domains"))
		pm_runtime_disable(&pdev->dev);

	return 0;
}

static const struct of_device_id of_match_mtk_mmsys[] = {
	{ .compatible = "mediatek,mt2701-mmsys", .data = &mt2701_mmsys_driver_data },
	{ .compatible = "mediatek,mt2712-mmsys", .data = &mt2712_mmsys_driver_data },
	{ .compatible = "mediatek,mt6779-mmsys", .data = &mt6779_mmsys_driver_data },
	{ .compatible = "mediatek,mt6795-mmsys", .data = &mt6795_mmsys_driver_data },
	{ .compatible = "mediatek,mt6797-mmsys", .data = &mt6797_mmsys_driver_data },
	{ .compatible = "mediatek,mt8167-mmsys", .data = &mt8167_mmsys_driver_data },
	{ .compatible = "mediatek,mt8173-mmsys", .data = &mt8173_mmsys_driver_data },
	{ .compatible = "mediatek,mt8183-mmsys", .data = &mt8183_mmsys_driver_data },
	{ .compatible = "mediatek,mt8186-mmsys", .data = &mt8186_mmsys_driver_data },
	{ .compatible = "mediatek,mt8188-vdosys0", .data = &mt8188_vdosys0_driver_data },
	{ .compatible = "mediatek,mt8188-vdosys1", .data = &mt8188_vdosys1_driver_data },
	{ .compatible = "mediatek,mt8188-vppsys0", .data = &mt8188_vppsys0_driver_data },
	{ .compatible = "mediatek,mt8188-vppsys1", .data = &mt8188_vppsys1_driver_data },
	{ .compatible = "mediatek,mt8192-mmsys", .data = &mt8192_mmsys_driver_data },
	/* "mediatek,mt8195-mmsys" compatible is deprecated */
	{ .compatible = "mediatek,mt8195-mmsys", .data = &mt8195_vdosys0_driver_data },
	{ .compatible = "mediatek,mt8195-vdosys0", .data = &mt8195_vdosys0_driver_data },
	{ .compatible = "mediatek,mt8195-vdosys1", .data = &mt8195_vdosys1_driver_data },
	{ .compatible = "mediatek,mt8195-vppsys0", .data = &mt8195_vppsys0_driver_data },
	{ .compatible = "mediatek,mt8195-vppsys1", .data = &mt8195_vppsys1_driver_data },
	{ .compatible = "mediatek,mt8196-dispsys0", .data = &mt8196_dispsys0_driver_data },
	{ .compatible = "mediatek,mt8196-dispsys1", .data = &mt8196_dispsys1_driver_data },
	{ .compatible = "mediatek,mt8196-ovlsys0", .data = &mt8196_ovlsys0_driver_data },
	{ .compatible = "mediatek,mt8196-ovlsys1", .data = &mt8196_ovlsys1_driver_data },
	{ .compatible = "mediatek,mt8196-vdisp-ao", .data = &mt8196_vdisp_ao_driver_data },
	{ .compatible = "mediatek,mt8365-mmsys", .data = &mt8365_mmsys_driver_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_mtk_mmsys);

static struct platform_driver mtk_mmsys_drv = {
	.driver = {
		.name = "mtk-mmsys",
		.of_match_table = of_match_mtk_mmsys,
	},
	.probe = mtk_mmsys_probe,
	.remove = mtk_mmsys_remove,
};
module_platform_driver(mtk_mmsys_drv);

MODULE_AUTHOR("Yongqiang Niu <yongqiang.niu@mediatek.com>");
MODULE_DESCRIPTION("MediaTek SoC MMSYS driver");
MODULE_LICENSE("GPL");
