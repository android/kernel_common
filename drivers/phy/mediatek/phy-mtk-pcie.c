// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 MediaTek Inc.
 * Author: Jianjun Wang <jianjun.wang@mediatek.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "phy-mtk-io.h"

/* PHY sif registers */
#define PEXTP_DIG_GLB_00		0x0
#define PRB_SEL_TO_101			0x101
#define PEXTP_DIG_GLB_04		0x4
#define PEXTP_DIG_GLB_08		0x8
#define CKGEN_PRB_SEL_TO_A000A		0xa000a
#define PEXTP_DIG_GLB_10		0x10
#define PEXTP_DIG_GLB_20		0x20
#define RG_XTP_BYPASS_PIPE_RST		BIT(4)
#define RG_XTP_BYPASS_PIPE_RST_RC	BIT(17)
#define PEXTP_DIG_GLB_28		0x28
#define RG_XTP_PCIE_MODE		BIT(3)
#define RG_XTP_PHY_CLKREQ_N_IN		GENMASK(13, 12)
#define PEXTP_DIG_GLB_30		0x30
#define RG_XTP_CKBG_STAL_STB_T_SEL	GENMASK(25, 16)
#define CKBG_STAL_STB_T_SEL_TO_0	0x0
#define PEXTP_DIG_GLB_38		0x38
#define RG_XTP_TPLL_SET_STB_T_SEL	GENMASK(7, 2)
#define TPLL_SET_STB_T_SEL_TO_3F	0x3f
#define RG_XTP_TPLL_PWE_ON_STB_T_SEL	GENMASK(9, 8)
#define TPLL_PWE_ON_STB_T_SEL_TO_3	0x3
#define PEXTP_DIG_GLB_50		0x50
#define RG_XTP_CKM_EN_L1S0		BIT(13)
#define RG_XTP_CKM_EN_L1S1		BIT(14)
#define PEXTP_DIG_PROBE_OUT		0xd0
#define PEXTP_DIG_GLB_70		0x70
#define RG_XTP_PIPE_UPDT		BIT(4)
#define RG_XTP_PIPE_TX_SWING		BIT(22)
#define PEXTP_DIG_GLB_A4		0xa4
#define RG_XTP_FRC_TX_SWING		BIT(1)
#define PEXTP_DIG_GLB_D0		0xd0
#define PEXTP_DIG_GLB_F4		0xf4
#define RG_XTP_TPLL_ISO_EN_STB_T_SEL	GENMASK(13, 12)
#define TPLL_ISO_EN_STB_T_SEL_TO_3	0x3

#define PEXTP_DIG_TPLL0_78		0x1078
#define RG_XTP_VCO_CFIX_EN_GEN1		GENMASK(21, 20)
#define RG_XTP_VCO_CFIX_EN_GEN2		GENMASK(23, 22)
#define HIGH_VCO_FREQ			0x0

/* PHY ANA GLB registers */
#define PEXTP_DIG_LN_TRX_70		0x3070
#define RG_XTP_LN_FRC_RX_AEQ_DFETP5	BIT(21)
#define RG_XTP_LN_FRC_RX_AEQ_DFETP4	BIT(29)

#define PEXTP_DIG_LN_TRX_74		0x3074
#define RG_XTP_LN_FRC_RX_AEQ_DFETP3	BIT(6)
#define RG_XTP_LN_FRC_RX_AEQ_DFETP2	BIT(14)
#define RG_XTP_LN_FRC_RX_AEQ_DFETP1	BIT(23)

#define PEXTP_DIG_LN_TRX_E8		0x30e8
#define RG_XTP_LN_RX_LF_CTLE_CSEL_GEN4	GENMASK(14, 12)
#define CTLE_CSEL_GEN4_TO_1		0x1

#define PEXTP_DIG_LN_RX_F0		0x50f0
#define RG_XTP_LN_RX_GEN1_CTLE1_CSEL	GENMASK(3, 0)
#define GEN1_CTLE1_CSEL_TO_D		0xd
#define RG_XTP_LN_RX_GEN2_CTLE1_CSEL	GENMASK(7, 4)
#define GEN2_CTLE1_CSEL_TO_D		0xd
#define RG_XTP_LN_RX_GEN3_CTLE1_CSEL	GENMASK(11, 8)
#define GEN3_CTLE1_CSEL_TO_D		0xd

#define PEXTP_DIG_LN_RX2_04			0x6004
#define RG_XTP_LN_RX_AEQ_EGEQ_RATIO_GEN3	GENMASK(21, 16)
#define RG_XTP_LN_RX_AEQ_EGEQ_RATIO_GEN4	GENMASK(29, 24)
#define AEQ_EGEQ_RATIO_GEN3_TO_22		0x16
#define AEQ_EGEQ_RATIO_GEN4_TO_22		0x16

#define PEXTP_DIG_LN_RX2_94		0x6094
#define RG_XTP_LN_RX_CDR_DLY_OFF_GEN3	BIT(6)
#define RG_XTP_LN_RX_CDR_DLY_OFF_GEN4	BIT(7)

#define PEXTP_DIG_LN_RX2_A4		0x60a4
#define RG_XTP_LN_RX_AEQ_OFORCE_GEN3	GENMASK(7, 1)
#define AEQ_OFORCE_GEN3_TO_7F		0x7f
#define RG_XTP_LN_RX_AEQ_OFORCE_GEN4	GENMASK(19, 13)
#define AEQ_OFORCE_GEN4_TO_7F		0x7f

#define PEXTP_ANA_GLB_00_REG		0x9000
/* Internal Resistor Selection of TX Bias Current */
#define EFUSE_GLB_INTR_SEL		GENMASK(28, 24)

#define PEXTP_ANA_GLB_10_REG		0x9010
#define GLB_TPLL0_RST_DLY		GENMASK(5, 4)
#define RESET_COUNTER_SELECT_2		0x2

#define PEXTP_ANA_GLB_14_REG		0x9014
#define GLB_TPLL0_DEBUG_SEL		GENMASK(13, 11)

#define PEXTP_ANA_GLB_6			(PEXTP_ANA_GLB_00_REG + 0x18)
#define PEXTP_ANA_GLB_9			(PEXTP_ANA_GLB_00_REG + 0x24)

#define PEXTP_ANA_GLB_2C		0x902c
#define RG_XTP_GLB_TPLL1_RESERVE_0	GENMASK(7, 0)
#define TPLL1_P_PATH_GAIN_TO_05		0xf1

#define PEXTP_ANA_GLB_50_REG		0x9050
#define PEXTP_ANA_GLB_54_REG		0x9054

#define PEXTP_ANA_GLB_60		0x9060
#define RG_XTP_GLB_BIAS_INTR_CTRL	GENMASK(5, 0)

#define PEXTP_ANA_GLB_C0		0x90c0
#define RG_XTP_GLB_BIAS_V2V_VTRIM	GENMASK(9, 6)


#define PEXTP_ANA_LN0_TRX_REG		0xa000
#define RG_XTP_LN_TX_RESERVE		GENMASK(31, 16)
#define LN_TX_RESERVE_TO_8		0x8

#define PEXTP_ANA_LN_TRX_C		0xa00c
#define RG_XTP_LN_TX_RSWN_IMPSEL	GENMASK(20, 16)

#define PEXTP_ANA_LN_TRX_34		0xA034
#define RG_XTP_LN_RX_FE			BIT(15)

#define PEXTP_ANA_LN_TRX_6C		0xA06C
#define RG_XTP_LN_RX_AEQ_CTLE_ERR_TYPE	GENMASK(14, 13)
#define AEQ_CTLE_ERR_TYPE_H15		0x0
#define AEQ_CTLE_ERR_TYPE_H15_H25	0x1

#define PEXTP_ANA_LN_TRX_A0		0xa0a0
#define RG_XTP_LN_TX_IMPSEL_PMOS	GENMASK(4, 0)
#define TX_IMPSEL_PMOS_TO_A		0xa
#define RG_XTP_LN_TX_IMPSEL_NMOS	GENMASK(11, 7)
#define TX_IMPSEL_NMOS_TO_9		0x9
#define RG_XTP_LN_RX_IMPSEL		GENMASK(15, 12)

#define PEXTP_ANA_LN_TRX_A8		0xa0a8
#define RG_XTP_LN_RX_LEQ_RL_CTLE_CAL	GENMASK(6, 2)
#define RG_XTP_LN_RX_LEQ_RL_VGA_CAL	GENMASK(11, 7)
#define RG_XTP_LN_RX_LEQ_RL_DFE_CAL	GENMASK(23, 19)

#define PEXTP_DIG_LN_TX_RSWN_4		0xb004
#define PEXTP_DIG_LN_TX_RSWN_8		0xb008
#define PEXTP_DIG_LN_TX_RSWN_C		0xb00c
#define PEXTP_DIG_LN_TX_RSWN_10		0xb010
#define PEXTP_DIG_LN_TX_RSWN_14		0xb014
#define PEXTP_DIG_LN_TX_RSWN_18		0xb018
#define RG_XTP_LN_TX_MGX_PX_CM1		GENMASK(5, 0)
#define MGX_PX_CM1_TO_1			0x1
#define MGX_PX_CM1_TO_2			0x2
#define RG_XTP_LN_TX_MGX_PX_C0		GENMASK(13, 8)
#define MGX_PX_C0_TO_A			0xa
#define MGX_PX_C0_TO_B			0xb
#define MGX_PX_C0_TO_C			0xc
#define RG_XTP_LN_TX_MGX_PX_CP1		GENMASK(21, 16)
#define MGX_PX_CP1_TO_1			0x1
#define MGX_PX_CP1_TO_2			0x2


#define PEXTP_ANA_TX_REG		0x04
/* TX PMOS impedance selection */
#define EFUSE_LN_TX_PMOS_SEL		GENMASK(5, 2)
/* TX NMOS impedance selection */
#define EFUSE_LN_TX_NMOS_SEL		GENMASK(11, 8)

#define PEXTP_ANA_RX_REG		0x3c
/* RX impedance selection */
#define EFUSE_LN_RX_SEL			GENMASK(3, 0)

#define PEXTP_ANA_LANE_OFFSET		0x100

/* PHY ckm regsiters */
#define XTP_CKM_DA_REG_38		0x38
#define RG_CKM_BIAS_WAIT_PRD		GENMASK(21, 16)
#define CKM_BIAS_WAIT_PRD_TO_4US	0x4
#define XTP_CKM_DA_REG_3C		0x3C
#define RG_CKM_PADCK_REQ		GENMASK(13, 12)
#define RG_CKM_PROBE_SEL		GENMASK(19, 17)
#define XTP_CKM_DA_REG_44		0x44
#define XTP_CKM_DA_REG_D4		0xD4
#define RG_CKM_CKTX_IMPSEL_PMOS		GENMASK(19, 16)
#define RG_CKM_CKTX_IMPSEL_NMOS		GENMASK(23, 20)
#define RG_CKM_CKTX_IMPSEL_SW		GENMASK(27, 24)

/* EFUSE */
#define SPHY3_EFUSE_MAX_LANE		2

#define EFUSE_GLB_BIAS_INTR_CTRL	GENMASK(5, 0)
#define EFUSE_GLB_BIAS_V2V_VTRIM	GENMASK(9, 6)
#define EFUSE_CKM_CKTX_IMPSEL_PMOS	GENMASK(13, 10)
#define EFUSE_CKM_CKTX_IMPSEL_NMOS	GENMASK(17, 14)
#define EFUSE_CKM_CKTX_IMPSEL_RMID	GENMASK(21, 18)
#define EFUSE_LN0_TX_RSWN_IMPSEL	GENMASK(26, 22)
#define EFUSE_LN1_TX_RSWN_IMPSEL	GENMASK(31, 27)

#define EFUSE_LN_RX_LEQ_RL_CTLE_CAL	GENMASK(4, 0)
#define EFUSE_LN_RX_LEQ_RL_VGA_CAL	GENMASK(9, 5)
#define EFUSE_LN_RX_LEQ_RL_DEF_CAL	GENMASK(14, 10)
#define EFUSE_LN_RX_IMPSEl		GENMASK(18, 15)
#define EFUSE_LN_TX_IMPSEL_PMOS		GENMASK(24, 20)
#define EFUSE_LN_TX_IMPSEL_NMOS		GENMASK(29, 25)

/**
 * struct mtk_pcie_lane_efuse - eFuse data for each lane
 * @tx_pmos: TX PMOS impedance selection data
 * @tx_nmos: TX NMOS impedance selection data
 * @rx_data: RX impedance selection data
 * @lane_efuse_supported: software eFuse data is supported for this lane
 */
struct mtk_pcie_lane_efuse {
	u32 tx_pmos;
	u32 tx_nmos;
	u32 rx_data;
	bool lane_efuse_supported;
};

/**
 * struct mtk_pcie_phy_data - phy data for each SoC
 * @num_lanes: supported lane numbers
 * @sw_efuse_supported: support software to load eFuse data
 */
struct mtk_pcie_phy_data {
	int num_lanes;
	bool sw_efuse_supported;
	int (*phy_init)(struct phy *phy);
};

/**
 * struct mtk_pcie_phy - PCIe phy driver main structure
 * @dev: pointer to device
 * @phy: pointer to generic phy
 * @sif_base: IO mapped register base address of system interface
 * @data: pointer to SoC dependent data
 * @num_lanes: supported lane numbers
 * @num_clks: PCIe PHY clocks count
 * @clks: PCIe PHY clocks
 * @sw_efuse_en: software eFuse enable status
 * @efuse_glb_intr: internal resistor selection of TX bias current data
 * @efuse: pointer to eFuse data for each lane
 */
struct mtk_pcie_phy {
	struct device *dev;
	struct phy *phy;
	void __iomem *sif_base;
	void __iomem *ckm_base;
	const struct mtk_pcie_phy_data *data;

	int num_lanes;
	int num_clks;
	struct clk_bulk_data *clks;
	bool sw_efuse_en;
	u32 efuse_glb_intr;
	u32 *efuse_info;
	size_t efuse_info_len;
	struct mtk_pcie_lane_efuse *efuse;
};

static void mtk_pcie_efuse_set_lane(struct mtk_pcie_phy *pcie_phy,
				    unsigned int lane)
{
	struct mtk_pcie_lane_efuse *data = &pcie_phy->efuse[lane];
	void __iomem *addr;

	if (!data->lane_efuse_supported)
		return;

	addr = pcie_phy->sif_base + PEXTP_ANA_LN0_TRX_REG +
	       lane * PEXTP_ANA_LANE_OFFSET;

	mtk_phy_update_field(addr + PEXTP_ANA_TX_REG, EFUSE_LN_TX_PMOS_SEL,
			     data->tx_pmos);

	mtk_phy_update_field(addr + PEXTP_ANA_TX_REG, EFUSE_LN_TX_NMOS_SEL,
			     data->tx_nmos);

	mtk_phy_update_field(addr + PEXTP_ANA_RX_REG, EFUSE_LN_RX_SEL,
			     data->rx_data);
}

/**
 * mtk_pcie_phy_init() - Initialize the phy
 * @phy: the phy to be initialized
 *
 * Initialize the phy by setting the efuse data.
 * The hardware settings will be reset during suspend, it should be
 * reinitialized when the consumer calls phy_init() again on resume.
 */
static int mtk_pcie_phy_init(struct phy *phy)
{
	struct mtk_pcie_phy *pcie_phy = phy_get_drvdata(phy);
	int i, ret;

	if (pcie_phy->data->phy_init) {
		ret = pcie_phy->data->phy_init(phy);
		if (ret)
			return ret;
	}

	if (pcie_phy->num_clks > 0) {
		ret = clk_bulk_prepare_enable(pcie_phy->num_clks, pcie_phy->clks);
		if (ret) {
			dev_info(pcie_phy->dev, "failed to enable clocks\n");
			return ret;
		}
	}

	if (!pcie_phy->sw_efuse_en)
		return 0;

	/* Set global data */
	mtk_phy_update_field(pcie_phy->sif_base + PEXTP_ANA_GLB_00_REG,
			     EFUSE_GLB_INTR_SEL, pcie_phy->efuse_glb_intr);

	for (i = 0; i < pcie_phy->num_lanes; i++)
		mtk_pcie_efuse_set_lane(pcie_phy, i);

	return 0;
}

static int mtk_pcie_phy_exit(struct phy *phy)
{
	struct mtk_pcie_phy *pcie_phy = phy_get_drvdata(phy);

	if (pcie_phy->num_clks > 0)
		clk_bulk_disable_unprepare(pcie_phy->num_clks, pcie_phy->clks);

	return 0;
}

static const struct phy_ops mtk_pcie_phy_ops = {
	.init	= mtk_pcie_phy_init,
	.exit	= mtk_pcie_phy_exit,
	.owner	= THIS_MODULE,
};

static int mtk_pcie_efuse_read_for_lane(struct mtk_pcie_phy *pcie_phy,
					unsigned int lane)
{
	struct mtk_pcie_lane_efuse *efuse = &pcie_phy->efuse[lane];
	struct device *dev = pcie_phy->dev;
	char efuse_id[16];
	int ret;

	snprintf(efuse_id, sizeof(efuse_id), "tx_ln%d_pmos", lane);
	ret = nvmem_cell_read_variable_le_u32(dev, efuse_id, &efuse->tx_pmos);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read %s\n", efuse_id);

	snprintf(efuse_id, sizeof(efuse_id), "tx_ln%d_nmos", lane);
	ret = nvmem_cell_read_variable_le_u32(dev, efuse_id, &efuse->tx_nmos);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read %s\n", efuse_id);

	snprintf(efuse_id, sizeof(efuse_id), "rx_ln%d", lane);
	ret = nvmem_cell_read_variable_le_u32(dev, efuse_id, &efuse->rx_data);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read %s\n", efuse_id);

	if (!(efuse->tx_pmos || efuse->tx_nmos || efuse->rx_data))
		return dev_err_probe(dev, -EINVAL,
				     "No eFuse data found for lane%d, but dts enable it\n",
				     lane);

	efuse->lane_efuse_supported = true;

	return 0;
}

static int mtk_pcie_read_efuse(struct mtk_pcie_phy *pcie_phy)
{
	struct device *dev = pcie_phy->dev;
	bool nvmem_enabled;
	int ret, i;

	/* nvmem data is optional */
	nvmem_enabled = device_property_present(dev, "nvmem-cells");
	if (!nvmem_enabled)
		return 0;

	ret = nvmem_cell_read_variable_le_u32(dev, "glb_intr",
					      &pcie_phy->efuse_glb_intr);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read glb_intr\n");

	pcie_phy->sw_efuse_en = true;

	pcie_phy->efuse = devm_kzalloc(dev, pcie_phy->num_lanes *
				       sizeof(*pcie_phy->efuse), GFP_KERNEL);
	if (!pcie_phy->efuse)
		return -ENOMEM;

	for (i = 0; i < pcie_phy->num_lanes; i++) {
		ret = mtk_pcie_efuse_read_for_lane(pcie_phy, i);
		if (ret)
			return ret;
	}

	return 0;
}

static int mtk_pcie_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;
	struct mtk_pcie_phy *pcie_phy;
	int ret;

	pcie_phy = devm_kzalloc(dev, sizeof(*pcie_phy), GFP_KERNEL);
	if (!pcie_phy)
		return -ENOMEM;

	pcie_phy->sif_base = devm_platform_ioremap_resource_byname(pdev, "sif");
	if (IS_ERR(pcie_phy->sif_base))
		return dev_err_probe(dev, PTR_ERR(pcie_phy->sif_base),
				     "Failed to map phy-sif base\n");

	pcie_phy->ckm_base = devm_platform_ioremap_resource_byname(pdev, "ckm");
	if (IS_ERR(pcie_phy->ckm_base)) {
		dev_info(dev, "Failed to get ckm resource\n");
		pcie_phy->ckm_base = NULL;
	}

	pcie_phy->phy = devm_phy_create(dev, dev->of_node, &mtk_pcie_phy_ops);
	if (IS_ERR(pcie_phy->phy))
		return dev_err_probe(dev, PTR_ERR(pcie_phy->phy),
				     "Failed to create PCIe phy\n");

	pcie_phy->dev = dev;

	/* Some PHY may not have clks, only print warning in that case */
	pcie_phy->num_clks = devm_clk_bulk_get_all(dev, &pcie_phy->clks);
	if (pcie_phy->num_clks < 0)
		dev_info(dev, "Failed to get clocks, pcie-phy may not works as expects\n");

	pcie_phy->data = of_device_get_match_data(dev);
	if (!pcie_phy->data)
		return dev_err_probe(dev, -EINVAL, "Failed to get phy data\n");

	ret = of_property_read_u32(dev->of_node, "num-lanes", &pcie_phy->num_lanes);
	if (ret) {
		if (pcie_phy->data->num_lanes) {
			pcie_phy->num_lanes = pcie_phy->data->num_lanes;
		} else {
			dev_info(dev, "Failed to get num-lanes, set as 1\n");
			pcie_phy->num_lanes = 1;
		}
	}

	if (pcie_phy->data->sw_efuse_supported) {
		/*
		 * Failed to read the efuse data is not a fatal problem,
		 * ignore the failure and keep going.
		 */
		ret = mtk_pcie_read_efuse(pcie_phy);
		if (ret == -EPROBE_DEFER || ret == -ENOMEM)
			return ret;
	}

	phy_set_drvdata(pcie_phy->phy, pcie_phy);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(provider))
		return dev_err_probe(dev, PTR_ERR(provider),
				     "PCIe phy probe failed\n");

	return 0;
}

static int mtk_pcie_sphy3_calibrate(struct phy *phy)
{
	struct mtk_pcie_phy *pcie_phy = phy_get_drvdata(phy);
	struct device *dev = pcie_phy->dev;
	u32 i;

	/* Efuse info is null or chip without calibrated data */
	if (!pcie_phy->efuse_info || !pcie_phy->efuse_info_len || !pcie_phy->efuse_info[0])
		goto no_efuse_info;

	/* Current SPHY3 efuse architecture only support 1 or 2 lane */
	if (pcie_phy->num_lanes > SPHY3_EFUSE_MAX_LANE) {
		dev_info(dev, "The number of lanes %d out of range\n", pcie_phy->num_lanes);
		goto no_efuse_info;
	}

	if ((pcie_phy->num_lanes + 1) * sizeof(u32) > pcie_phy->efuse_info_len) {
		dev_info(dev, "efuse info length = %zu error\n", pcie_phy->efuse_info_len);
		goto no_efuse_info;
	}

	mtk_phy_update_field(pcie_phy->sif_base + PEXTP_ANA_GLB_60,
			     RG_XTP_GLB_BIAS_INTR_CTRL,
			     FIELD_GET(EFUSE_GLB_BIAS_INTR_CTRL, pcie_phy->efuse_info[0]));

	mtk_phy_update_field(pcie_phy->sif_base + PEXTP_ANA_GLB_C0,
			     RG_XTP_GLB_BIAS_V2V_VTRIM,
			     FIELD_GET(EFUSE_GLB_BIAS_V2V_VTRIM, pcie_phy->efuse_info[0]));

	mtk_phy_update_field(pcie_phy->ckm_base + XTP_CKM_DA_REG_D4,
			     RG_CKM_CKTX_IMPSEL_PMOS,
			     FIELD_GET(EFUSE_CKM_CKTX_IMPSEL_PMOS, pcie_phy->efuse_info[0]));

	mtk_phy_update_field(pcie_phy->ckm_base + XTP_CKM_DA_REG_D4,
			     RG_CKM_CKTX_IMPSEL_NMOS,
			     FIELD_GET(EFUSE_CKM_CKTX_IMPSEL_NMOS, pcie_phy->efuse_info[0]));

	mtk_phy_update_field(pcie_phy->ckm_base + XTP_CKM_DA_REG_D4,
			     RG_CKM_CKTX_IMPSEL_SW,
			     FIELD_GET(EFUSE_CKM_CKTX_IMPSEL_RMID, pcie_phy->efuse_info[0]));

	for (i = 0; i < pcie_phy->num_lanes; i++) {
		if (i)
			mtk_phy_update_field(pcie_phy->sif_base +
					     PEXTP_ANA_LN_TRX_C +
					     i * PEXTP_ANA_LANE_OFFSET,
					     RG_XTP_LN_TX_RSWN_IMPSEL,
					     FIELD_GET(EFUSE_LN1_TX_RSWN_IMPSEL, pcie_phy->efuse_info[0]));
		else
			mtk_phy_update_field(pcie_phy->sif_base +
					     PEXTP_ANA_LN_TRX_C +
					     i * PEXTP_ANA_LANE_OFFSET,
					     RG_XTP_LN_TX_RSWN_IMPSEL,
					     FIELD_GET(EFUSE_LN0_TX_RSWN_IMPSEL, pcie_phy->efuse_info[0]));

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_ANA_LN_TRX_A8 +
				     i * PEXTP_ANA_LANE_OFFSET,
				     RG_XTP_LN_RX_LEQ_RL_CTLE_CAL,
				     FIELD_GET(EFUSE_LN_RX_LEQ_RL_CTLE_CAL, pcie_phy->efuse_info[i+1]));

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_ANA_LN_TRX_A8 +
				     i * PEXTP_ANA_LANE_OFFSET,
				     RG_XTP_LN_RX_LEQ_RL_VGA_CAL,
				     FIELD_GET(EFUSE_LN_RX_LEQ_RL_VGA_CAL, pcie_phy->efuse_info[i+1]));

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_ANA_LN_TRX_A8 +
				     i * PEXTP_ANA_LANE_OFFSET,
				     RG_XTP_LN_RX_LEQ_RL_DFE_CAL,
				     FIELD_GET(EFUSE_LN_RX_LEQ_RL_DEF_CAL, pcie_phy->efuse_info[i+1]));

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_ANA_LN_TRX_A0 +
				     i * PEXTP_ANA_LANE_OFFSET,
				     RG_XTP_LN_RX_IMPSEL,
				     FIELD_GET(EFUSE_LN_RX_IMPSEl, pcie_phy->efuse_info[i+1]));

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_ANA_LN_TRX_A0 +
				     i * PEXTP_ANA_LANE_OFFSET,
				     RG_XTP_LN_TX_IMPSEL_PMOS,
				     FIELD_GET(EFUSE_LN_TX_IMPSEL_PMOS, pcie_phy->efuse_info[i+1]));

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_ANA_LN_TRX_A0 +
				     i * PEXTP_ANA_LANE_OFFSET,
				     RG_XTP_LN_TX_IMPSEL_NMOS,
				     FIELD_GET(EFUSE_LN_TX_IMPSEL_NMOS, pcie_phy->efuse_info[i+1]));
	}

	dev_info(dev, "Calibration successful\n");

	return 0;

no_efuse_info:
	dev_info(dev, "No calibration info\n");

	/* To prevent potential EM problem, apply this setting
	 * if no Efuse calibration
	 */
	for (i = 0; i < pcie_phy->num_lanes; i++) {
		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_ANA_LN_TRX_A0 +
				     i * PEXTP_ANA_LANE_OFFSET,
				     RG_XTP_LN_TX_IMPSEL_PMOS,
				     TX_IMPSEL_PMOS_TO_A);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_ANA_LN_TRX_A0 +
				     i * PEXTP_ANA_LANE_OFFSET,
				     RG_XTP_LN_TX_IMPSEL_NMOS,
				     TX_IMPSEL_NMOS_TO_9);
	}

	return 0;
}

static int mtk_pcie_phy_init_8196(struct phy *phy)
{
	struct mtk_pcie_phy *pcie_phy = phy_get_drvdata(phy);
	struct device *dev = pcie_phy->dev;
	u32 i;

	if (!pcie_phy->ckm_base) {
		dev_info(dev, "phy-ckm base not found, please check\n");
		return -EINVAL;
	}

	/* RC mode need adjust PHY sequence to fix L1.2 issue */
	mtk_phy_update_field(pcie_phy->ckm_base + XTP_CKM_DA_REG_38, RG_CKM_BIAS_WAIT_PRD,
			     CKM_BIAS_WAIT_PRD_TO_4US);

	mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_GLB_38, RG_XTP_TPLL_SET_STB_T_SEL,
			     TPLL_SET_STB_T_SEL_TO_3F);

	mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_GLB_38, RG_XTP_TPLL_PWE_ON_STB_T_SEL,
			     TPLL_PWE_ON_STB_T_SEL_TO_3);

	mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_GLB_F4, RG_XTP_TPLL_ISO_EN_STB_T_SEL,
			     TPLL_ISO_EN_STB_T_SEL_TO_3);

	mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_GLB_30, RG_XTP_CKBG_STAL_STB_T_SEL,
			     CKBG_STAL_STB_T_SEL_TO_0);

	mtk_phy_clear_bits(pcie_phy->sif_base + PEXTP_DIG_GLB_50, RG_XTP_CKM_EN_L1S1);

	/* not bypass pipe reset, pipe reset will reset TPLL */
	mtk_phy_clear_bits(pcie_phy->sif_base + PEXTP_DIG_GLB_20, RG_XTP_BYPASS_PIPE_RST_RC);

	dev_info(dev, "CKM_38=%#x, GLB_20=%#x, GLB_30=%#x, GLB_38=%#x, GLB_F4=%#x\n",
		 readl_relaxed(pcie_phy->ckm_base + XTP_CKM_DA_REG_38),
		 readl_relaxed(pcie_phy->sif_base + PEXTP_DIG_GLB_20),
		 readl_relaxed(pcie_phy->sif_base + PEXTP_DIG_GLB_30),
		 readl_relaxed(pcie_phy->sif_base + PEXTP_DIG_GLB_38),
		 readl_relaxed(pcie_phy->sif_base + PEXTP_DIG_GLB_F4));

	mtk_phy_update_field(pcie_phy->sif_base + PEXTP_ANA_GLB_2C, RG_XTP_GLB_TPLL1_RESERVE_0,
			     TPLL1_P_PATH_GAIN_TO_05);

	for (i = 0; i < pcie_phy->num_lanes; i++) {
		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_ANA_LN_TRX_6C +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_RX_AEQ_CTLE_ERR_TYPE,
				     AEQ_CTLE_ERR_TYPE_H15_H25);

		mtk_phy_set_bits(pcie_phy->sif_base + PEXTP_ANA_LN_TRX_34 +
				 i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_RX_FE);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_TRX_E8 +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_RX_LF_CTLE_CSEL_GEN4,
				     CTLE_CSEL_GEN4_TO_1);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_RX_F0 +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_RX_GEN1_CTLE1_CSEL,
				     GEN1_CTLE1_CSEL_TO_D);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_RX_F0 +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_RX_GEN2_CTLE1_CSEL,
				     GEN2_CTLE1_CSEL_TO_D);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_RX_F0 +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_RX_GEN3_CTLE1_CSEL,
				     GEN3_CTLE1_CSEL_TO_D);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_RX2_04 +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_RX_AEQ_EGEQ_RATIO_GEN3,
				     AEQ_EGEQ_RATIO_GEN3_TO_22);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_RX2_04 +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_RX_AEQ_EGEQ_RATIO_GEN4,
				     AEQ_EGEQ_RATIO_GEN4_TO_22);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_TX_RSWN_4 +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_TX_MGX_PX_C0,
				     MGX_PX_C0_TO_A);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_TX_RSWN_4 +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_TX_MGX_PX_CP1,
				     MGX_PX_CP1_TO_2);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_TX_RSWN_8 +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_TX_MGX_PX_C0,
				     MGX_PX_C0_TO_B);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_TX_RSWN_8 +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_TX_MGX_PX_CP1,
				     MGX_PX_CP1_TO_1);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_TX_RSWN_C +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_TX_MGX_PX_C0,
				     MGX_PX_C0_TO_C);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_TX_RSWN_10 +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_TX_MGX_PX_C0,
				     MGX_PX_C0_TO_B);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_TX_RSWN_10 +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_TX_MGX_PX_CM1,
				     MGX_PX_CM1_TO_1);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_TX_RSWN_14 +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_TX_MGX_PX_C0,
				     MGX_PX_C0_TO_B);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_TX_RSWN_14 +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_TX_MGX_PX_CM1,
				     MGX_PX_CM1_TO_1);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_TX_RSWN_18 +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_TX_MGX_PX_C0,
				     MGX_PX_C0_TO_A);

		mtk_phy_update_field(pcie_phy->sif_base + PEXTP_DIG_LN_TX_RSWN_18 +
				     i * PEXTP_ANA_LANE_OFFSET, RG_XTP_LN_TX_MGX_PX_CM1,
				     MGX_PX_CM1_TO_2);
	}

	return mtk_pcie_sphy3_calibrate(phy);
}

static const struct mtk_pcie_phy_data mt8195_data = {
	.num_lanes = 2,
	.sw_efuse_supported = true,
};

static const struct mtk_pcie_phy_data mt8196_data = {
	.sw_efuse_supported = false,
	.phy_init = mtk_pcie_phy_init_8196,
};

static const struct of_device_id mtk_pcie_phy_of_match[] = {
	{ .compatible = "mediatek,mt8195-pcie-phy", .data = &mt8195_data },
	{ .compatible = "mediatek,mt8196-pcie-phy", .data = &mt8196_data },
	{ },
};
MODULE_DEVICE_TABLE(of, mtk_pcie_phy_of_match);

static struct platform_driver mtk_pcie_phy_driver = {
	.probe	= mtk_pcie_phy_probe,
	.driver	= {
		.name = "mtk-pcie-phy",
		.of_match_table = mtk_pcie_phy_of_match,
	},
};
module_platform_driver(mtk_pcie_phy_driver);

MODULE_DESCRIPTION("MediaTek PCIe PHY driver");
MODULE_AUTHOR("Jianjun Wang <jianjun.wang@mediatek.com>");
MODULE_LICENSE("GPL");
