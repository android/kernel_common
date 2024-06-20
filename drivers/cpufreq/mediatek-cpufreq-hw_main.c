// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/bitfield.h>
#include <linux/cpufreq.h>
#include <linux/energy_model.h>
#include <linux/init.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/kdebug.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/panic_notifier.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/sched/clock.h>
#include <linux/topology.h>

#include "mediatek-cpufreq-hw_fdvfs.h"

#define LUT_MAX_ENTRIES			32U
#define LUT_FREQ			GENMASK(11, 0)
#define LUT_ROW_SIZE			0x4
#define CPUFREQ_HW_STATUS		BIT(0)
#define SVS_HW_STATUS			BIT(1)
#define POLL_USEC			1000
#define TIMEOUT_USEC			300000
#define REG_FREQ_SCALING		0x4cc/* from csram_base */
#define REG_STOP_CPUDVFS_LOG		0x128 /* from csram_base */
#define REG_QOS_OFF			0x20
#define CHECK_VAL			0X55AA55AA
#define MAX_CONTROL_GROUPS		8
#define MAX_PERF_DOMAINS		3
#define PER_CORE_OFF			0x1380
#define PER_CORE_SIZE			0x20
#define CSRAM_BASE_ADD			0x10

enum {
	REG_FREQ_LUT_TABLE,
	REG_FREQ_ENABLE,
	REG_FREQ_PERF_STATE,
	REG_FREQ_HW_STATE,
	REG_EM_POWER_TBL,
	REG_FREQ_LATENCY,

	REG_ARRAY_SIZE,
};

struct cpufreq_mtk {
	struct cpufreq_frequency_table *table;
	void __iomem *reg_bases[REG_ARRAY_SIZE];
	int nr_opp;
	unsigned int last_index;
	cpumask_t related_cpus;
	long sb_ch;
};

static const u16 cpufreq_mtk_offsets[REG_ARRAY_SIZE] = {
	[REG_FREQ_LUT_TABLE]	= 0x0,
	[REG_FREQ_ENABLE]	= 0x84,
	[REG_FREQ_PERF_STATE]	= 0x88,
	[REG_FREQ_HW_STATE]	= 0x8C,
	[REG_EM_POWER_TBL]	= 0x90,
	[REG_FREQ_LATENCY]	= 0x114,
};

static struct cpufreq_mtk *mtk_freq_domain_map[NR_CPUS];
static bool freq_scaling_disabled = true;
static bool fdvfs_enabled;
static bool per_core_enabled;
static void __iomem *qos_base;
static void __iomem *per_core_base;
static int cpu_control_group_map[MAX_CONTROL_GROUPS];
static int control_group_master[MAX_CONTROL_GROUPS];
static int perf_domain_master[MAX_PERF_DOMAINS];

static int look_up_cpu(struct device *cpu_dev)
{
	unsigned int cpu = 0;

	for_each_possible_cpu(cpu) {
		if (cpu_dev == get_cpu_device(cpu))
			return cpu;
	}

	return 0; /* fallback to cpu0 */
}


static int __maybe_unused
mtk_cpufreq_get_cpu_power(struct device *cpu_dev, unsigned long *uW,
		unsigned long *KHz)
{
	int cpu = look_up_cpu(cpu_dev);
	struct cpufreq_mtk *c = mtk_freq_domain_map[cpu];
	int i;

	for (i = 0; i < c->nr_opp; i++) {
		if (c->table[i].frequency < *KHz)
			break;
	}
	i--;

	*KHz = c->table[i].frequency;
	*uW = readl_relaxed(c->reg_bases[REG_EM_POWER_TBL] +
			    i * LUT_ROW_SIZE);

	return 0;
}

static int stop_dvfs_log(struct notifier_block *self, unsigned long cmd, void *ptr)
{
	static atomic_t first_exception = ATOMIC_INIT(0);
	struct cpufreq_mtk *c = mtk_freq_domain_map[0];
	void __iomem *csram_base_add_0x10;
	void __iomem *stop_dvfs_log;

	if(c) {
		if(atomic_cmpxchg(&first_exception, 0, 1) != 0)
			return NOTIFY_DONE;
		csram_base_add_0x10 = c->reg_bases[REG_FREQ_LUT_TABLE];
		stop_dvfs_log = csram_base_add_0x10 + REG_STOP_CPUDVFS_LOG - CSRAM_BASE_ADD;
		writel_relaxed(0x1, stop_dvfs_log);
	}
	return NOTIFY_DONE;
}

static struct notifier_block die_blk = {
	.notifier_call = stop_dvfs_log,
	.priority = INT_MAX,
};

static struct notifier_block panic_blk = {
	.notifier_call = stop_dvfs_log,
	.priority = INT_MAX,
};
static int mtk_cpufreq_hw_target_index(struct cpufreq_policy *policy,
				       unsigned int index)
{
	struct cpufreq_mtk *c = policy->driver_data;

	if (!freq_scaling_disabled) {
		int target_freq = policy->freq_table[index].frequency;

		if (fdvfs_enabled)
			cpufreq_fdvfs_switch(target_freq, policy);
		else
			writel_relaxed(target_freq, c->reg_bases[REG_FREQ_PERF_STATE]);
	} else
		writel_relaxed(index, c->reg_bases[REG_FREQ_PERF_STATE]);

	return 0;
}

static unsigned int mtk_cpufreq_hw_get(unsigned int cpu)
{
	struct cpufreq_mtk *c;
	unsigned int index, freq, nr_opp;

	c = mtk_freq_domain_map[cpu];
	nr_opp = c->nr_opp;

	if (per_core_enabled)
		index = readl_relaxed(per_core_base + CPU_OFS * cpu);
	else
		freq = readl_relaxed(c->reg_bases[REG_FREQ_PERF_STATE]);

	if (per_core_enabled && index <= LUT_MAX_ENTRIES - 1) {
		index = min(index, LUT_MAX_ENTRIES - 1);
		return c->table[index].frequency;
	} else if (c->table[0].frequency >= freq && freq >= c->table[nr_opp - 1].frequency) {
		return freq;
	}

	return 0;
}

static unsigned int mtk_cpufreq_hw_fast_switch(struct cpufreq_policy *policy,
					       unsigned int target_freq)
{
	struct cpufreq_mtk *c = policy->driver_data;
	unsigned int index;
#if IS_ENABLED(CONFIG_MTK_IRQ_MONITOR_DEBUG)
	u64 ts[2];

	ts[0] = sched_clock();
#endif

	if (policy->cached_target_freq == target_freq)
		index = policy->cached_resolved_idx;
	else
		index = cpufreq_table_find_index_dl(policy, target_freq, false);

	if (fdvfs_enabled) {
		if(c->sb_ch && c->sb_ch != -1)
			cpufreq_fdvfs_cci_switch(c->sb_ch);
		cpufreq_fdvfs_switch(target_freq, policy);
	} else {
		if(qos_base && c->sb_ch) {
			c->sb_ch == -1 ?
			writel_relaxed(0, qos_base + REG_QOS_OFF):
			writel_relaxed(c->sb_ch, qos_base + REG_QOS_OFF);
		}
		if (!freq_scaling_disabled) // Frequency scaling enabled
			writel_relaxed(target_freq, c->reg_bases[REG_FREQ_PERF_STATE]);
		else
			writel_relaxed(index, c->reg_bases[REG_FREQ_PERF_STATE]);
	}

	if (c->last_index == index)
		return 0;
	c->last_index = index;

#if IS_ENABLED(CONFIG_MTK_IRQ_MONITOR_DEBUG)
	ts[1] = sched_clock();

	if ((ts[1] - ts[0] > 100000ULL) && in_hardirq()) {
		printk_deferred("%s duration %llu, ts[0]=%llu, ts[1]=%llu\n",
			__func__, ts[1] - ts[0], ts[0], ts[1]);
	}
#endif

	return policy->freq_table[index].frequency;
}

static int mtk_cpufreq_hw_cpu_init(struct cpufreq_policy *policy)
{
	struct cpufreq_mtk *c;
	struct pm_qos_request *qos_request;
	struct device *dev = get_cpu_device(policy->cpu);
	int sig, pwr_hw = CPUFREQ_HW_STATUS | SVS_HW_STATUS;
	int ret = 0;
	unsigned int latency;

	qos_request = kzalloc(sizeof(*qos_request), GFP_KERNEL);
	if (!qos_request)
		return -ENOMEM;

	c = mtk_freq_domain_map[policy->cpu];
	if (!c) {
		dev_err(dev, "No scaling support for CPU%d\n", policy->cpu);
		ret = -EOPNOTSUPP;
		goto free_qos_request;
	}

	cpumask_copy(policy->cpus, &c->related_cpus);

	policy->freq_table = c->table;
	policy->driver_data = c;

	latency = readl_relaxed(c->reg_bases[REG_FREQ_LATENCY]);
	if (!latency)
		latency = CPUFREQ_ETERNAL;

	/* us convert to ns */
	policy->cpuinfo.transition_latency = latency * POLL_USEC;

	policy->fast_switch_possible = true;

	/* Let CPUs leave idle-off state for SVS CPU initializing */
	cpu_latency_qos_add_request(qos_request, PM_QOS_DEFAULT_VALUE);

	/* HW should be in enabled state to proceed now */
	writel_relaxed(0x1, c->reg_bases[REG_FREQ_ENABLE]);

	if (readl_poll_timeout(c->reg_bases[REG_FREQ_HW_STATE], sig,
			       (sig & pwr_hw) == pwr_hw, POLL_USEC,
			       TIMEOUT_USEC)) {
		if (!(sig & CPUFREQ_HW_STATUS)) {
			dev_err(dev, "cpufreq hardware of CPU%d is not enabled\n", policy->cpu);
			cpu_latency_qos_remove_request(qos_request);
			ret = -EOPNOTSUPP;
			goto remove_qos_request;
		}

		dev_notice(dev, "SVS of CPU%d is not enabled\n", policy->cpu);
	}

remove_qos_request:
	cpu_latency_qos_remove_request(qos_request);
free_qos_request:
	kfree(qos_request);

	return ret;
}

static int mtk_cpufreq_hw_cpu_exit(struct cpufreq_policy *policy)
{
	struct cpufreq_mtk *c;

	c = mtk_freq_domain_map[policy->cpu];
	if (!c) {
		dev_err(get_cpu_device(policy->cpu), "No scaling support for CPU%d\n", policy->cpu);
		return -EOPNOTSUPP;
	}

	/* HW should be in paused state now */
	writel_relaxed(0x0, c->reg_bases[REG_FREQ_ENABLE]);

	return 0;
}

static void mtk_cpufreq_register_em(struct cpufreq_policy *policy)
{
	struct em_data_callback em_cb = EM_DATA_CB(mtk_cpufreq_get_cpu_power);
	struct cpufreq_mtk *c = policy->driver_data;

	if (!c->nr_opp) {
		dev_err(get_cpu_device(policy->cpu),
			 "%s: %d: CPU%d: Get opp failed\n", __func__, __LINE__,
			 policy->cpu);
		return;
	}

	em_dev_register_perf_domain(get_cpu_device(policy->cpu), c->nr_opp, &em_cb, policy->cpus,
			true);
}

static struct cpufreq_driver cpufreq_mtk_hw_driver = {
	.flags		= CPUFREQ_NEED_INITIAL_FREQ_CHECK |
			  CPUFREQ_HAVE_GOVERNOR_PER_POLICY |
			  CPUFREQ_IS_COOLING_DEV,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= mtk_cpufreq_hw_target_index,
	.get		= mtk_cpufreq_hw_get,
	.init		= mtk_cpufreq_hw_cpu_init,
	.exit		= mtk_cpufreq_hw_cpu_exit,
	.register_em	= mtk_cpufreq_register_em,
	.fast_switch	= mtk_cpufreq_hw_fast_switch,
	.name		= "mtk-cpufreq-hw",
	.attr		= cpufreq_generic_attr,
};

static int mtk_cpu_create_freq_table(struct platform_device *pdev,
				     struct cpufreq_mtk *c)
{
	struct device *dev = &pdev->dev;
	void __iomem *base_table;
	u32 data, i, freq, prev_freq = 0;

	c->table = devm_kcalloc(dev, LUT_MAX_ENTRIES + 1,
				sizeof(*c->table), GFP_KERNEL);
	if (!c->table)
		return -ENOMEM;

	base_table = c->reg_bases[REG_FREQ_LUT_TABLE];

	for (i = 0; i < LUT_MAX_ENTRIES; i++) {
		data = readl_relaxed(base_table + (i * LUT_ROW_SIZE));
		freq = FIELD_GET(LUT_FREQ, data) * POLL_USEC;

		if (freq == prev_freq)
			break;

		c->table[i].frequency = freq;

		dev_dbg(dev, "index=%d freq=%d\n",
			i, c->table[i].frequency);

		prev_freq = freq;
	}

	c->table[i].frequency = CPUFREQ_TABLE_END;
	c->nr_opp = i;

	return 0;
}

static int mtk_get_related_cpus(int master_index, struct cpufreq_mtk *c)
{
	struct device_node *cpu_np;
	struct of_phandle_args args;
	int control_index;
	int cpu, ret;

	for_each_possible_cpu(cpu) {
		if(!per_core_enabled) {
			cpu_np = of_cpu_device_node_get(cpu);
			if (!cpu_np)
				continue;

			ret = of_parse_phandle_with_args(cpu_np, "performance-domains",
							"#performance-domain-cells", 0,
							&args);
			of_node_put(cpu_np);
			if (ret < 0)
				continue;

			control_index = args.args[0];
		} else
			control_index = cpu_control_group_map[cpu];

		if (control_index == master_index) {
			cpumask_set_cpu(cpu, &c->related_cpus);
			mtk_freq_domain_map[cpu] = c;
		}
	}

	return 0;
}

static int mtk_cpu_resources_init(struct platform_device *pdev,
				  unsigned int cpu, int index,
				  const u16 *offsets)
{
	struct cpufreq_mtk *c;
	struct device *dev = &pdev->dev;
	int control_index;
	int ret, i;
	void __iomem *base;

	if (mtk_freq_domain_map[cpu])
		return 0;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, index);
	if (IS_ERR(base))
		return PTR_ERR(base);

	for (i = REG_FREQ_LUT_TABLE; i < REG_ARRAY_SIZE; i++)
		c->reg_bases[i] = base + offsets[i];

	control_index = !per_core_enabled ? index : cpu_control_group_map[cpu];
	ret = mtk_get_related_cpus(control_index, c);
	if (ret) {
		dev_err(dev, "Domain-%d failed to get related CPUs\n", control_index);
		return ret;
	}

	ret = mtk_cpu_create_freq_table(pdev, c);
	if (ret) {
		dev_err(dev, "Domain-%d failed to create freq table\n", index);
		return ret;
	}

	return 0;
}

static int mtk_per_core_cpu_resources_init(struct platform_device *pdev,
					unsigned int cpu, int index,
					const u16 *offsets)
{
	struct cpufreq_mtk *c;
	struct cpufreq_mtk *master_c;
	struct device *dev = &pdev->dev;
	int ret, i, master;
	int control_group = cpu_control_group_map[cpu];

	if (mtk_freq_domain_map[cpu]) {
		dev_info(dev, "already has per-core entry: index-%d cpu-%d\n", index, cpu);
		return 0;
	}

	if (cpu == perf_domain_master[index])
		return mtk_cpu_resources_init(pdev, cpu, index, offsets);
	else if (cpu == control_group_master[control_group]) {
		master = perf_domain_master[index];
		if (master == -1) {
			dev_err(dev, "no master for control group %d, index %d\n", control_group, index);
			return -EINVAL;
		}
		master_c = mtk_freq_domain_map[master];

		c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
		if (!c)
			return -ENOMEM;

		/* init all other member variables from master_c */
		c->table = master_c->table;
		c->nr_opp = master_c->nr_opp;

		for (i = REG_FREQ_LUT_TABLE; i < REG_ARRAY_SIZE; i++)
			c->reg_bases[i] = master_c->reg_bases[i];

		ret = mtk_get_related_cpus(control_group, c);
		if (ret) {
			dev_err(dev, "Domain-%d failed to get related CPUs\n", control_group);
			return ret;
		}
	} else {
		/* shall not go in here */
		dev_err(dev, "wrong per-core entry, index-%d cpu-%d\n", index, cpu);
		return -EINVAL;
	}

	return 0;
}

static bool check_per_core_enabled(struct device *dev)
{
	static int control_group_num;
	struct device_node *cpu_np;
	u32 control_group;
	int cpu_cluster_id;
	int i, cpu, ret;

	if (!fdvfs_enabled) {
		dev_err(dev, "%s: per-core requires fdvfs support\n", __func__);
		return false;
	}

	control_group_num = 0;
	for (i = 0; i < MAX_CONTROL_GROUPS; i++)
		control_group_master[i] = -1;

	for (i = 0; i < MAX_PERF_DOMAINS; i++)
		perf_domain_master[i] = -1;

	for_each_possible_cpu(cpu) {
		cpu_np = of_cpu_device_node_get(cpu);
		if (!cpu_np) {
			dev_err(dev, "%s: failed to get cpu device\n", __func__);
			return false;
		}

		ret = of_property_read_u32(cpu_np, "control-group", &control_group);
		of_node_put(cpu_np);
		if (ret < 0) {
			dev_err(dev, "%s: failed to get control-group\n", __func__);
			return false;
		}

		if (control_group >= MAX_CONTROL_GROUPS) {
			dev_err(dev, "%s: control-group incorrect\n", __func__);
			return false;
		}

		cpu_control_group_map[cpu] = control_group;

		if (control_group_master[control_group] == -1)
			control_group_master[control_group] = cpu;

		if (control_group + 1 > control_group_num)
			control_group_num = control_group + 1;

		cpu_cluster_id = topology_cluster_id(cpu);
		if (perf_domain_master[cpu_cluster_id] == -1)
			perf_domain_master[cpu_cluster_id] = cpu;
	}

	dev_dbg(dev, "%s: per-core enabled\n", __func__);
	return true;
}

static int mtk_cpufreq_hw_driver_probe(struct platform_device *pdev)
{
	struct device_node *cpu_np;
	struct device_node *hvfs_node, *csram_sys_node;
	struct of_phandle_args args;
	struct resource *csram_res;
	struct platform_device *pdev_c;
	static void __iomem *csram_base;
	const u16 *offsets;
	unsigned int cpu;
	int ret;

	offsets = of_device_get_match_data(&pdev->dev);
	if (!offsets)
		return -EINVAL;

	hvfs_node = of_find_compatible_node(NULL, NULL, "mediatek,cpufreq-hybrid");
	if (!hvfs_node) {
		dev_err(&pdev->dev, "failed to find node @ %s\n", __func__);
		return -ENODEV;
	}

	pdev_c = of_find_device_by_node(hvfs_node);
	if (!pdev_c) {
		dev_err(&pdev->dev, "failed to find pdev @ %s\n", __func__);
		return -EINVAL;
	}

	csram_res = platform_get_resource(pdev_c, IORESOURCE_MEM, 1);
	if (!csram_res) {
		dev_err(&pdev->dev, "failed to get mem resource @ %s\n", __func__);
		return -ENODEV;
	}

	if (!request_mem_region(csram_res->start, resource_size(csram_res),
			csram_res->name)) {
		dev_err(&pdev->dev, "failed to request resource %pR\n", csram_res);
		return -EBUSY;
	}

	csram_base = ioremap(csram_res->start, resource_size(csram_res));
	if (!csram_base) {
		dev_err(&pdev->dev, "failed to map csram_base @ %s\n", __func__);
		ret = -ENOMEM;
		goto release_region;
	}

	csram_sys_node = of_parse_phandle(hvfs_node, "csram-sys-base", 0);
	if (csram_sys_node != NULL) {
		static void __iomem *csram_sys_base;

		csram_sys_base = of_iomap(csram_sys_node, 0);
		if (readl_relaxed(csram_sys_base) != CHECK_VAL ||
		    !readl_relaxed(csram_sys_base + CSRAM_BASE_ADD +
				   cpufreq_mtk_offsets[REG_FREQ_HW_STATE])) {
			dev_err(&pdev->dev, "%s: cpufreq hardware not enable\n", __func__);
			ret = -EIO;
			iounmap(csram_base);
			goto release_region;
		}
		dev_dbg(&pdev->dev, "%s: cpufreq hardware enable\n", __func__);
	}

	if (readl_relaxed(csram_base + REG_FREQ_SCALING)){
		freq_scaling_disabled = false;
		iounmap(csram_base);
		release_mem_region(csram_res->start, resource_size(csram_res));
	}
	fdvfs_enabled = check_fdvfs_support(&pdev->dev) == 1 ? true : false;
	per_core_enabled = check_per_core_enabled(&pdev->dev);

	if (per_core_enabled) {
		per_core_base = ioremap(csram_res->start + PER_CORE_OFF, PER_CORE_SIZE);
		if (!per_core_base) {
			dev_err(&pdev->dev, "failed to map per_core_base @ %s\n", __func__);
			return -ENOMEM;
		}
	}

	for_each_possible_cpu(cpu) {
		cpu_np = of_cpu_device_node_get(cpu);
		if (!cpu_np) {
			dev_err(&pdev->dev, "Failed to get cpu %d device\n",
				cpu);
			return -ENODEV;
		}

		ret = of_parse_phandle_with_args(cpu_np, "performance-domains",
						 "#performance-domain-cells", 0,
						 &args);
		if (ret < 0)
			return ret;

		if (per_core_enabled) {
			ret = mtk_per_core_cpu_resources_init(pdev, cpu, args.args[0], offsets);
			if (ret) {
				dev_err(&pdev->dev, "per-core CPUFreq resource init failed\n");
				return ret;
			}
		} else {
			/* Get the bases of cpufreq for domains */
			ret = mtk_cpu_resources_init(pdev, cpu, args.args[0], offsets);
			if (ret) {
				dev_err(&pdev->dev, "CPUFreq resource init failed\n");
				return ret;
			}
		}
	}

	ret = cpufreq_register_driver(&cpufreq_mtk_hw_driver);
	if (ret) {
		dev_err(&pdev->dev, "CPUFreq HW driver failed to register\n");
		return ret;
	}
	register_die_notifier(&die_blk);
	atomic_notifier_chain_register(&panic_notifier_list, &panic_blk);

	return 0;

release_region:
	release_mem_region(csram_res->start, resource_size(csram_res));
	return ret;
}

static int mtk_cpufreq_hw_driver_remove(struct platform_device *pdev)
{
	cpufreq_unregister_driver(&cpufreq_mtk_hw_driver);
	return 0;
}

static const struct of_device_id mtk_cpufreq_hw_match[] = {
	{ .compatible = "mediatek,cpufreq-hw-v1", .data = &cpufreq_mtk_offsets },
	{}
};

static struct platform_driver mtk_cpufreq_hw_driver = {
	.probe = mtk_cpufreq_hw_driver_probe,
	.remove = mtk_cpufreq_hw_driver_remove,
	.driver = {
		.name = "mtk-cpufreq-hw-v1",
		.of_match_table = mtk_cpufreq_hw_match,
	},
};
module_platform_driver(mtk_cpufreq_hw_driver);

MODULE_DESCRIPTION("Mediatek cpufreq-hw driver");
MODULE_LICENSE("GPL");
