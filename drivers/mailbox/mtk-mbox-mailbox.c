// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 MediaTek Inc.
 */

#include <linux/kernel.h>
#include <linux/of_device.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_fdt.h>
#ifdef CONFIG_OF_RESERVED_MEM
#include <linux/of_reserved_mem.h>
#endif

#include <asm/arch_timer.h>
#define FIXED_MBOX_SIZE		128
#define T_SEND_OFFSET_H		(FIXED_MBOX_SIZE - 16)
#define T_SEND_OFFSET_L		(FIXED_MBOX_SIZE - 12)
#define T_IRQ_OFFSET_H		(FIXED_MBOX_SIZE - 8)
#define T_IRQ_OFFSET_L		(FIXED_MBOX_SIZE - 4)

/* for secure mode */
#include <linux/arm-smccc.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>

enum mtk_tinysys_sspm_kernel_op {
	MTK_TINYSYS_SSPM_KERNEL_OP_MBOX_CLEAR = 0,
	MTK_TINYSYS_SSPM_KERNEL_OP_MD2SPM_IPC_CLEAR = 1,
	MTK_TINYSYS_SSPM_KERNEL_OP_NUM,
};

static unsigned int secure_mbox_clr_support;
static unsigned int secure_sspm_md2spm_clr_support;

#define INTR_SET_OFS	0x0
#define INTR_CLR_OFS	0x4
#define MBOX_CHANS	2

struct mhu_link {
	unsigned int irq; /* GIC irq */
	void __iomem *tx_reg;
	void __iomem *rx_reg;
	void __iomem *shmem;
	resource_size_t shmem_size;
	unsigned int mbox_irq; /* for secure mode */
};

struct mtk_mbu {
	void __iomem *base;
	struct mhu_link mlink[MBOX_CHANS];
	struct mbox_chan chan[MBOX_CHANS];
	struct mbox_controller mbox;
};

/* for secure mode */
static inline uint64_t sspm_do_mbox_clear(unsigned int mbox_irq)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_TINYSYS_SSPM_CONTROL,
		      MTK_TINYSYS_SSPM_KERNEL_OP_MBOX_CLEAR,
		      mbox_irq, 0, 0, 0, 0, 0, &res);

	return res.a0;
}

/* Clear SSPM2SPM_IPC */
static inline uint64_t sspm_do_md2spm_clear(void)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_TINYSYS_SSPM_CONTROL,
		      MTK_TINYSYS_SSPM_KERNEL_OP_MD2SPM_IPC_CLEAR,
		      0, 0, 0, 0, 0, 0, &res);

	return res.a0;
}

static irqreturn_t tinysys_mbox_rx_interrupt(int irq, void *p)
{
	struct mbox_chan *chan = p;
	struct mhu_link *mlink = chan->con_priv;
	u32 val;
	u64 tv;

	tv = __arch_counter_get_cntvct();
	writel_relaxed((u32)((tv & 0xFFFFFFFF00000000LL) >> 32), mlink->shmem + T_IRQ_OFFSET_H);
	writel_relaxed((u32)(tv & 0xFFFFFFFFLL), mlink->shmem + T_IRQ_OFFSET_L);

	val = readl_relaxed(mlink->rx_reg + INTR_CLR_OFS);
	dev_dbg(chan->mbox->dev,
		"[scmi] %s chan:%p txdone_method:%x\n",
		__func__, chan, chan->txdone_method);
	if (!val)
		return IRQ_NONE;

	/* for secure mode */
	if (secure_mbox_clr_support)
		sspm_do_mbox_clear(mlink->mbox_irq);
	else
		writel_relaxed(1, mlink->rx_reg + INTR_CLR_OFS);

	/* clear irq before free channel*/
	mbox_chan_received_data(chan, (void *)&val);

	/* Clear SSPM2SPM_IPC */
	if (secure_sspm_md2spm_clr_support)
		sspm_do_md2spm_clear();

	return IRQ_HANDLED;
}

static bool tinysys_mbox_last_tx_done(struct mbox_chan *chan)
{
	struct mhu_link *mlink = chan->con_priv;
	u32 val = readl_relaxed(mlink->tx_reg + INTR_SET_OFS);

	if (val == 0)
		dev_dbg(chan->mbox->dev, "[scmi] last_tx_done\n");
	return (val == 0);
}

static int tinysys_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct mhu_link *mlink = chan->con_priv;
	u64 tv;
	u32 *arg = data;

	dev_dbg(chan->mbox->dev, "[scmi] send_data %x\n", *arg);
	/* Make sure other CPUs can write in order */
	smp_mb();
	tv = __arch_counter_get_cntvct();
	writel_relaxed((u32)((tv & 0xFFFFFFFF00000000LL) >> 32),
			mlink->shmem + T_SEND_OFFSET_H);
	writel_relaxed((u32)(tv & 0xFFFFFFFFLL), mlink->shmem + T_SEND_OFFSET_L);
	writel_relaxed(1, mlink->tx_reg + INTR_SET_OFS);

	return 0;
}

static int tinysys_mbox_startup(struct mbox_chan *chan)
{
	struct mhu_link *mlink = chan->con_priv;
	int ret;

	ret = request_threaded_irq(mlink->irq, NULL,
				   tinysys_mbox_rx_interrupt,
				   IRQF_NO_SUSPEND | IRQF_TRIGGER_NONE | IRQF_ONESHOT,
				   "mtk_tinysys_mbox", chan);
	if (ret) {
		dev_err(chan->mbox->dev,
			"Unable to acquire IRQ %d\n", mlink->irq);
		return ret;
	}
	return 0;
}

static void tinysys_mbox_shutdown(struct mbox_chan *chan)
{
	struct mhu_link *mlink = chan->con_priv;

	if (mlink->irq)
		free_irq(mlink->irq, chan);
}

static const struct mbox_chan_ops tinysys_mbox_chan_ops = {
	.send_data = tinysys_mbox_send_data,
	.startup = tinysys_mbox_startup,
	.shutdown = tinysys_mbox_shutdown,
	.last_tx_done = tinysys_mbox_last_tx_done,
};

static int tinysys_mbox_probe(struct platform_device *pdev)
{
	int err, i;
	int secure_ret; /* for secure mode */
	struct mtk_mbu *mbu;
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct tinysys_mbox_plat *plat_data;
	struct device_node *shmem;
	struct resource shmem_res;
	resource_size_t size;

	/* Allocate memory for device */
	mbu = devm_kzalloc(dev, sizeof(*mbu), GFP_KERNEL);
	if (!mbu)
		return -ENOMEM;

	for (i = 0; i < MBOX_CHANS; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		mbu->base = devm_ioremap_resource(dev, res);

		if (IS_ERR(mbu->base)) {
			dev_err(dev, "failed to ioremap mbu");
			return PTR_ERR(mbu->base);
		}
		shmem = of_parse_phandle(dev->of_node, "shmem", i);
		err  = of_address_to_resource(shmem, 0, &shmem_res);
		of_node_put(shmem);
		if (err) {
			dev_err(dev, "failed to get scmi profile memory\n");
			return err;
		}
		size = resource_size(&shmem_res);
		mbu->mlink[i].shmem = devm_ioremap(dev, shmem_res.start, size);
		mbu->mlink[i].shmem_size = size;
		mbu->chan[i].con_priv = &mbu->mlink[i];

		mbu->mlink[i].irq = platform_get_irq(pdev, i);
		if (!mbu->mlink[i].irq)
			dev_err(dev, "failed to get irq");

		mbu->mlink[i].mbox_irq = i; /* for secure mode*/
		mbu->mlink[i].rx_reg = mbu->base;
		mbu->mlink[i].tx_reg = mbu->base;
	}

	/* for secure mode*/
	secure_ret = of_property_read_u32(pdev->dev.of_node,
		"secure-sspm-mbox-clr", &secure_mbox_clr_support);

	if ((!secure_ret) && secure_mbox_clr_support) {
		pr_notice("[scmi] secure-sspm-mbox-clr support, secure_mbox_clr_support: %d\n",
			secure_mbox_clr_support);
	} else {
		pr_notice("[scmi] secure-sspm-mbox-clr not support\n");
	}

	/* notify spm */
	secure_ret = of_property_read_u32(pdev->dev.of_node,
		"secure-sspm-md2spm-clr", &secure_sspm_md2spm_clr_support);

	if ((!secure_ret) && secure_sspm_md2spm_clr_support) {
		pr_notice("[scmi] secure-sspm-md2spm-clr support, secure_sspm_md2spm_clr_support: %d\n",
			secure_sspm_md2spm_clr_support);
	} else {
		pr_notice("[scmi] secure-sspm_md2spm-clr not support\n");
	}

	plat_data = (struct tinysys_mbox_plat *)of_device_get_match_data(dev);
	if (!plat_data) {
		dev_err(dev, "failed to get match data\n");
		return -EINVAL;
	}

	mbu->mbox.dev = dev;
	mbu->mbox.chans = &mbu->chan[0];
	mbu->mbox.num_chans = MBOX_CHANS;
	mbu->mbox.ops = &tinysys_mbox_chan_ops;
	mbu->mbox.txdone_irq = false;
	mbu->mbox.txdone_poll = true;
	mbu->mbox.txpoll_period = 1;

	platform_set_drvdata(pdev, mbu);

	err = devm_mbox_controller_register(dev, &mbu->mbox);
	if (err) {
		dev_err(dev, "Failed to register mailboxes %d\n", err);
		return err;
	}

	dev_dbg(dev, "MTK MBOX Mailbox registered\n");

	return 0;
}

struct tinysys_mbox_plat {
	u32 thread_nr;
	u32 bfs;
};

static const struct tinysys_mbox_plat tinysys_mbox_plat_v1 = {.thread_nr = 1, .bfs = 64};

static const struct of_device_id tinysys_mbox_of_ids[] = {
	{.compatible = "mediatek,tinysys_mbox", .data = (void *)&tinysys_mbox_plat_v1},
	{}
};

static int tinysys_mbox_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver mtk_tinysys_mbox_driver = {
	.probe = tinysys_mbox_probe,
	.remove = tinysys_mbox_remove,
	.driver = {
		.name = "mtk_tinysys_mbox",
		.of_match_table = tinysys_mbox_of_ids,
	}
};

module_platform_driver(mtk_tinysys_mbox_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek Mailbox driver");

