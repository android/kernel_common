// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 MediaTek Inc.
 * Author: James Liao <jamesjj.liao@mediatek.com>
 */

#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "clk-mtk.h"
#include "clk-gate.h"

struct mtk_clk_gate {
	struct clk_hw	hw;
	struct regmap	*regmap;
	struct regmap	*hwv_regmap;
	int		set_ofs;
	int		clr_ofs;
	int		sta_ofs;
	int		hwv_set_ofs;
	int		hwv_clr_ofs;
	int		hwv_sta_ofs;
	u8		bit;
};

static inline struct mtk_clk_gate *to_mtk_clk_gate(struct clk_hw *hw)
{
	return container_of(hw, struct mtk_clk_gate, hw);
}

static u32 mtk_get_clockgating(struct clk_hw *hw)
{
	struct mtk_clk_gate *cg = to_mtk_clk_gate(hw);
	u32 val;

	regmap_read(cg->regmap, cg->sta_ofs, &val);

	return val & BIT(cg->bit);
}

static int mtk_cg_bit_is_cleared(struct clk_hw *hw)
{
	return mtk_get_clockgating(hw) == 0;
}

static int mtk_cg_bit_is_set(struct clk_hw *hw)
{
	return mtk_get_clockgating(hw) != 0;
}

static void mtk_cg_set_bit(struct clk_hw *hw)
{
	struct mtk_clk_gate *cg = to_mtk_clk_gate(hw);

	regmap_write(cg->regmap, cg->set_ofs, BIT(cg->bit));
}

static void mtk_cg_clr_bit(struct clk_hw *hw)
{
	struct mtk_clk_gate *cg = to_mtk_clk_gate(hw);

	regmap_write(cg->regmap, cg->clr_ofs, BIT(cg->bit));
}

static void mtk_cg_set_bit_no_setclr(struct clk_hw *hw)
{
	struct mtk_clk_gate *cg = to_mtk_clk_gate(hw);

	regmap_set_bits(cg->regmap, cg->sta_ofs, BIT(cg->bit));
}

static void mtk_cg_clr_bit_no_setclr(struct clk_hw *hw)
{
	struct mtk_clk_gate *cg = to_mtk_clk_gate(hw);

	regmap_clear_bits(cg->regmap, cg->sta_ofs, BIT(cg->bit));
}

static int mtk_cg_enable(struct clk_hw *hw)
{
	mtk_cg_clr_bit(hw);

	return 0;
}

static void mtk_cg_disable(struct clk_hw *hw)
{
	mtk_cg_set_bit(hw);
}

static int mtk_cg_enable_inv(struct clk_hw *hw)
{
	mtk_cg_set_bit(hw);

	return 0;
}

static void mtk_cg_disable_inv(struct clk_hw *hw)
{
	mtk_cg_clr_bit(hw);
}

static int mtk_cg_is_set_hwv(struct clk_hw *hw)
{
	struct mtk_clk_gate *cg = to_mtk_clk_gate(hw);
	u32 val = 0;

	regmap_read(cg->hwv_regmap, cg->hwv_set_ofs, &val);

	val &= BIT(cg->bit);

	return val != 0;
}

static int mtk_cg_is_done_hwv(struct clk_hw *hw)
{
	struct mtk_clk_gate *cg = to_mtk_clk_gate(hw);
	u32 val = 0;

	regmap_read(cg->hwv_regmap, cg->hwv_sta_ofs, &val);

	val &= BIT(cg->bit);

	return val != 0;
}

static int __cg_enable_hwv(struct clk_hw *hw, bool inv)
{
	struct mtk_clk_gate *cg = to_mtk_clk_gate(hw);
	u32 val = 0, val2 = 0;
	bool is_done = false;
	int i = 0;

	regmap_write(cg->hwv_regmap, cg->hwv_set_ofs,
			BIT(cg->bit));

	while (!mtk_cg_is_set_hwv(hw)) {
		if (i < MTK_WAIT_HWV_PREPARE_CNT)
			udelay(MTK_WAIT_HWV_PREPARE_US);
		else {
			pr_err("%s cg prepare timeout\n", clk_hw_get_name(hw));
			return -EBUSY;
		}

		i++;
	}

	i = 0;

	while (1) {
		if (!is_done)
			regmap_read(cg->hwv_regmap, cg->hwv_sta_ofs, &val);

		if ((val & BIT(cg->bit)) != 0)
			is_done = true;

		if (is_done) {
			regmap_read(cg->regmap, cg->sta_ofs, &val2);
			if ((inv && (val2 & BIT(cg->bit)) != 0) ||
					(!inv && (val2 & BIT(cg->bit)) == 0))
				break;
		}

		if (i < MTK_WAIT_HWV_DONE_CNT)
			udelay(MTK_WAIT_HWV_DONE_US);
		else {
			pr_err("%s cg enable timeout(%x %x)\n", clk_hw_get_name(hw), val, val2);

			if (inv)
				regmap_write(cg->regmap, cg->set_ofs, BIT(cg->bit));
			else
				regmap_write(cg->regmap, cg->clr_ofs, BIT(cg->bit));

			return -EBUSY;
		}

		i++;
	}

	return 0;
}

static int mtk_cg_enable_hwv(struct clk_hw *hw)
{
	return __cg_enable_hwv(hw, false);
}

static int mtk_cg_enable_hwv_inv(struct clk_hw *hw)
{
	return __cg_enable_hwv(hw, true);
}

static void mtk_cg_disable_hwv(struct clk_hw *hw)
{
	struct mtk_clk_gate *cg = to_mtk_clk_gate(hw);
	u32 val;
	int i = 0;

	/* dummy read to clr idle signal of hw voter bus */
	regmap_read(cg->hwv_regmap, cg->hwv_clr_ofs, &val);

	regmap_write(cg->hwv_regmap, cg->hwv_clr_ofs, BIT(cg->bit));

	while (mtk_cg_is_set_hwv(hw)) {
		if (i < MTK_WAIT_HWV_PREPARE_CNT)
			udelay(MTK_WAIT_HWV_PREPARE_US);
		else {
			pr_err("%s cg unprepare timeout\n", clk_hw_get_name(hw));
			return;
		}

		i++;
	}

	i = 0;

	while (!mtk_cg_is_done_hwv(hw)) {
		if (i < MTK_WAIT_HWV_DONE_CNT)
			udelay(MTK_WAIT_HWV_DONE_US);
		else {
			pr_err("%s cg disable timeout\n", clk_hw_get_name(hw));
			return;
		}

		i++;
	}
}

static void mtk_cg_disable_unused_hwv(struct clk_hw *hw)
{
	mtk_cg_enable_hwv(hw);
	mtk_cg_disable_hwv(hw);
}

static void mtk_cg_disable_unused_hwv_inv(struct clk_hw *hw)
{
	mtk_cg_enable_hwv_inv(hw);
	mtk_cg_disable_hwv(hw);
}

static int mtk_cg_enable_no_setclr(struct clk_hw *hw)
{
	mtk_cg_clr_bit_no_setclr(hw);

	return 0;
}

static void mtk_cg_disable_no_setclr(struct clk_hw *hw)
{
	mtk_cg_set_bit_no_setclr(hw);
}

static int mtk_cg_enable_inv_no_setclr(struct clk_hw *hw)
{
	mtk_cg_set_bit_no_setclr(hw);

	return 0;
}

static void mtk_cg_disable_inv_no_setclr(struct clk_hw *hw)
{
	mtk_cg_clr_bit_no_setclr(hw);
}

const struct clk_ops mtk_clk_gate_ops_setclr = {
	.is_enabled	= mtk_cg_bit_is_cleared,
	.enable		= mtk_cg_enable,
	.disable	= mtk_cg_disable,
};
EXPORT_SYMBOL_GPL(mtk_clk_gate_ops_setclr);

const struct clk_ops mtk_clk_gate_ops_setclr_enable = {
	.is_enabled	= mtk_cg_bit_is_cleared,
	.enable		= mtk_cg_enable,
};
EXPORT_SYMBOL_GPL(mtk_clk_gate_ops_setclr_enable);

const struct clk_ops mtk_clk_gate_ops_setclr_inv = {
	.is_enabled	= mtk_cg_bit_is_set,
	.enable		= mtk_cg_enable_inv,
	.disable	= mtk_cg_disable_inv,
};
EXPORT_SYMBOL_GPL(mtk_clk_gate_ops_setclr_inv);

const struct clk_ops mtk_clk_gate_ops_hwv = {
	.is_enabled	= mtk_cg_bit_is_cleared,
	.enable		= mtk_cg_enable_hwv,
	.disable	= mtk_cg_disable_hwv,
	.disable_unused	= mtk_cg_disable_unused_hwv,
};
EXPORT_SYMBOL_GPL(mtk_clk_gate_ops_hwv);

const struct clk_ops mtk_clk_gate_ops_hwv_inv = {
	.is_enabled	= mtk_cg_bit_is_set,
	.enable		= mtk_cg_enable_hwv_inv,
	.disable	= mtk_cg_disable_hwv,
	.disable_unused	= mtk_cg_disable_unused_hwv_inv,
};
EXPORT_SYMBOL_GPL(mtk_clk_gate_ops_hwv_inv);
const struct clk_ops mtk_clk_gate_ops_no_setclr = {
	.is_enabled	= mtk_cg_bit_is_cleared,
	.enable		= mtk_cg_enable_no_setclr,
	.disable	= mtk_cg_disable_no_setclr,
};
EXPORT_SYMBOL_GPL(mtk_clk_gate_ops_no_setclr);

const struct clk_ops mtk_clk_gate_ops_no_setclr_inv = {
	.is_enabled	= mtk_cg_bit_is_set,
	.enable		= mtk_cg_enable_inv_no_setclr,
	.disable	= mtk_cg_disable_inv_no_setclr,
};
EXPORT_SYMBOL_GPL(mtk_clk_gate_ops_no_setclr_inv);

static struct clk_hw *mtk_clk_register_gate(struct device *dev, const char *name,
					 const char *parent_name,
					 struct regmap *regmap, int set_ofs,
					 int clr_ofs, int sta_ofs, u8 bit,
					 const struct clk_ops *ops,
					 unsigned long flags)
{
	struct mtk_clk_gate *cg;
	int ret;
	struct clk_init_data init = {};

	cg = kzalloc(sizeof(*cg), GFP_KERNEL);
	if (!cg)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.flags = flags | CLK_SET_RATE_PARENT;
	init.parent_names = parent_name ? &parent_name : NULL;
	init.num_parents = parent_name ? 1 : 0;
	init.ops = ops;

	cg->regmap = regmap;
	cg->set_ofs = set_ofs;
	cg->clr_ofs = clr_ofs;
	cg->sta_ofs = sta_ofs;
	cg->bit = bit;

	cg->hw.init = &init;

	ret = clk_hw_register(dev, &cg->hw);
	if (ret) {
		kfree(cg);
		return ERR_PTR(ret);
	}

	return &cg->hw;
}

static struct clk_hw *mtk_clk_register_gate_hwv(
		struct device *dev,
		const struct mtk_gate *gate,
		struct regmap *regmap,
		struct regmap *hwv_regmap)
{
	struct mtk_clk_gate *cg;
	int ret;
	struct clk_init_data init = {};

	cg = kzalloc(sizeof(*cg), GFP_KERNEL);
	if (!cg)
		return ERR_PTR(-ENOMEM);

	init.name = gate->name;
	init.flags = gate->flags | CLK_SET_RATE_PARENT;
	init.parent_names = gate->parent_name ? &gate->parent_name : NULL;
	init.num_parents = gate->parent_name ? 1 : 0;
	if (hwv_regmap)
		init.ops = gate->ops;
	else
		init.ops = gate->dma_ops;

	cg->regmap = regmap;
	cg->hwv_regmap = hwv_regmap;
	if (gate->regs) {
		cg->set_ofs = gate->regs->set_ofs;
		cg->clr_ofs = gate->regs->clr_ofs;
		cg->sta_ofs = gate->regs->sta_ofs;
	}
	if (gate->hwv_regs) {
		cg->hwv_set_ofs = gate->hwv_regs->set_ofs;
		cg->hwv_clr_ofs = gate->hwv_regs->clr_ofs;
		cg->hwv_sta_ofs = gate->hwv_regs->sta_ofs;
	}
	cg->bit = gate->shift;

	cg->hw.init = &init;

	ret = clk_hw_register(dev, &cg->hw);
	if (ret) {
		kfree(cg);
		return ERR_PTR(ret);
	}

	return &cg->hw;
}

static void mtk_clk_unregister_gate(struct clk_hw *hw)
{
	struct mtk_clk_gate *cg;
	if (!hw)
		return;

	cg = to_mtk_clk_gate(hw);

	clk_hw_unregister(hw);
	kfree(cg);
}

int mtk_clk_register_gates(struct device *dev, struct device_node *node,
			   const struct mtk_gate *clks, int num,
			   struct clk_hw_onecell_data *clk_data)
{
	int i;
	struct clk_hw *hw;
	struct regmap *regmap;
	struct regmap  *hwv_regmap = NULL;

	if (!clk_data)
		return -ENOMEM;

	regmap = device_node_to_regmap(node);
	if (IS_ERR(regmap)) {
		pr_err("Cannot find regmap for %pOF: %pe\n", node, regmap);
		return PTR_ERR(regmap);
	}

	for (i = 0; i < num; i++) {
		const struct mtk_gate *gate = &clks[i];

		if (!IS_ERR_OR_NULL(clk_data->hws[gate->id])) {
			pr_warn("%pOF: Trying to register duplicate clock ID: %d\n",
				node, gate->id);
			continue;
		}

		if (gate->flags & CLK_USE_HW_VOTER) {
			if (gate->hwv_comp) {
				hwv_regmap = syscon_regmap_lookup_by_phandle(node, gate->hwv_comp);
				if (IS_ERR(hwv_regmap))
					hwv_regmap = NULL;
			}

			hw = mtk_clk_register_gate_hwv(dev, gate, regmap, hwv_regmap);
		} else
			hw = mtk_clk_register_gate(dev, gate->name, gate->parent_name,
					    regmap,
					    gate->regs->set_ofs,
					    gate->regs->clr_ofs,
					    gate->regs->sta_ofs,
					    gate->shift, gate->ops,
					    gate->flags);

		if (IS_ERR(hw)) {
			pr_err("Failed to register clk %s: %pe\n", gate->name,
			       hw);
			goto err;
		}

		clk_data->hws[gate->id] = hw;
	}

	return 0;

err:
	while (--i >= 0) {
		const struct mtk_gate *gate = &clks[i];

		if (IS_ERR_OR_NULL(clk_data->hws[gate->id]))
			continue;

		mtk_clk_unregister_gate(clk_data->hws[gate->id]);
		clk_data->hws[gate->id] = ERR_PTR(-ENOENT);
	}

	return PTR_ERR(hw);
}
EXPORT_SYMBOL_GPL(mtk_clk_register_gates);

void mtk_clk_unregister_gates(const struct mtk_gate *clks, int num,
			      struct clk_hw_onecell_data *clk_data)
{
	int i;

	if (!clk_data)
		return;

	for (i = num; i > 0; i--) {
		const struct mtk_gate *gate = &clks[i - 1];

		if (IS_ERR_OR_NULL(clk_data->hws[gate->id]))
			continue;

		mtk_clk_unregister_gate(clk_data->hws[gate->id]);
		clk_data->hws[gate->id] = ERR_PTR(-ENOENT);
	}
}
EXPORT_SYMBOL_GPL(mtk_clk_unregister_gates);

MODULE_LICENSE("GPL");
