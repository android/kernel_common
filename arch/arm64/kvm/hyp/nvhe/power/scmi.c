// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Linaro Ltd.
 */

#include <linux/bitfield.h>

#include <nvhe/pkvm.h>
#include <nvhe/mm.h>
#include <nvhe/mem_protect.h>
#include <nvhe/trap_handler.h>

/* SCMI protocol */
#define SCMI_PROTOCOL_POWER_DOMAIN	0x11

/*  shmem registers */
#define SCMI_SHM_CHANNEL_STATUS		0x4
#define SCMI_SHM_CHANNEL_FLAGS		0x10
#define SCMI_SHM_LENGTH			0x14
#define SCMI_SHM_MESSAGE_HEADER		0x18
#define SCMI_SHM_MESSAGE_PAYLOAD	0x1c

/*  channel status */
#define SCMI_CHN_FREE			(1U << 0)
#define SCMI_CHN_ERROR			(1U << 1)

/*  channel flags */
#define SCMI_CHN_IRQ			(1U << 0)

/*  message header */
#define SCMI_HDR_TOKEN			GENMASK(27, 18)
#define SCMI_HDR_PROTOCOL_ID		GENMASK(17, 10)
#define SCMI_HDR_MESSAGE_TYPE		GENMASK(9, 8)
#define SCMI_HDR_MESSAGE_ID		GENMASK(7, 0)

/*  power domain */
#define SCMI_PD_STATE_SET		0x4
#define SCMI_PD_STATE_SET_FLAGS		0x0
#define SCMI_PD_STATE_SET_DOMAIN_ID	0x4
#define SCMI_PD_STATE_SET_POWER_STATE	0x8

#define SCMI_PD_STATE_SET_STATUS	0x0

#define SCMI_PD_STATE_SET_FLAGS_ASYNC	(1U << 0)

#define SCMI_PD_POWER_ON		0
#define SCMI_PD_POWER_OFF		(1U << 30)

#define SCMI_SUCCESS			0


static struct {
	u32				smc_id;
	phys_addr_t			shmem_pfn;
	size_t				shmem_size;
	void __iomem			*shmem;
} scmi_channel;

struct scmi_power_domain {
	struct kvm_power_domain			*pd;
	const struct kvm_power_domain_ops	*ops;
};

static struct scmi_power_domain scmi_power_domains[MAX_POWER_DOMAINS];
static int scmi_power_domain_count;

#define SCMI_POLL_TIMEOUT_US	1000000 /* 1s! */

/* Forward the command to EL3, and wait for completion */
static int scmi_run_command(struct kvm_cpu_context *host_ctxt)
{
	u32 reg;
	unsigned long i = 0;

	__kvm_hyp_host_forward_smc(host_ctxt);

	do {
		reg = readl_relaxed(scmi_channel.shmem + SCMI_SHM_CHANNEL_STATUS);
		if (reg & SCMI_CHN_FREE)
			break;

		if (WARN_ON(++i > SCMI_POLL_TIMEOUT_US))
			return -ETIMEDOUT;

		pkvm_udelay(1);
	} while (!(reg & (SCMI_CHN_FREE | SCMI_CHN_ERROR)));

	if (reg & SCMI_CHN_ERROR)
		return -EIO;

	reg = readl_relaxed(scmi_channel.shmem + SCMI_SHM_MESSAGE_PAYLOAD +
			    SCMI_PD_STATE_SET_STATUS);
	if (reg != SCMI_SUCCESS)
		return -EIO;

	return 0;
}

static void __kvm_host_scmi_handler(struct kvm_cpu_context *host_ctxt)
{
	int i;
	u32 reg;
	struct scmi_power_domain *scmi_pd = NULL;

	/*
	 * FIXME: the spec does not really allow for an intermediary filtering
	 * messages on the channel: as soon as the host clears SCMI_CHN_FREE,
	 * the server may process the message. It doesn't have to wait for a
	 * doorbell and could just poll on the shared mem. Unlikely in practice,
	 * but this code is not correct without a spec change requiring the
	 * server to observe an SMC before processing the message.
	 */
	reg = readl_relaxed(scmi_channel.shmem + SCMI_SHM_CHANNEL_STATUS);
	if (reg & (SCMI_CHN_FREE | SCMI_CHN_ERROR))
		return;

	reg = readl_relaxed(scmi_channel.shmem + SCMI_SHM_MESSAGE_HEADER);
	if (FIELD_GET(SCMI_HDR_PROTOCOL_ID, reg) != SCMI_PROTOCOL_POWER_DOMAIN)
		goto out_forward_smc;

	if (FIELD_GET(SCMI_HDR_MESSAGE_ID, reg) != SCMI_PD_STATE_SET)
		goto out_forward_smc;

	reg = readl_relaxed(scmi_channel.shmem + SCMI_SHM_MESSAGE_PAYLOAD +
			    SCMI_PD_STATE_SET_FLAGS);
	if (WARN_ON(reg & SCMI_PD_STATE_SET_FLAGS_ASYNC))
		/* We don't support async requests at the moment */
		return;

	reg = readl_relaxed(scmi_channel.shmem + SCMI_SHM_MESSAGE_PAYLOAD +
			    SCMI_PD_STATE_SET_DOMAIN_ID);

	for (i = 0; i < MAX_POWER_DOMAINS; i++) {
		if (!scmi_power_domains[i].pd)
			break;

		if (reg == scmi_power_domains[i].pd->arm_scmi.domain_id) {
			scmi_pd = &scmi_power_domains[i];
			break;
		}
	}
	if (!scmi_pd)
		goto out_forward_smc;

	reg = readl_relaxed(scmi_channel.shmem + SCMI_SHM_MESSAGE_PAYLOAD +
			    SCMI_PD_STATE_SET_POWER_STATE);
	switch (reg) {
	case SCMI_PD_POWER_ON:
		if (scmi_run_command(host_ctxt))
			break;

		scmi_pd->ops->power_on(scmi_pd->pd);
		break;
	case SCMI_PD_POWER_OFF:
		scmi_pd->ops->power_off(scmi_pd->pd);

		if (scmi_run_command(host_ctxt))
			scmi_pd->ops->power_on(scmi_pd->pd);
		break;
	}
	return;

out_forward_smc:
	__kvm_hyp_host_forward_smc(host_ctxt);
}

bool kvm_host_scmi_handler(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(u64, func_id, host_ctxt, 0);

	if (!scmi_channel.shmem || func_id != scmi_channel.smc_id)
		return false; /* Unhandled */

	/*
	 * Prevent the host from modifying the request while it is in flight.
	 * One page is enough, SCMI messages are smaller than that.
	 *
	 * FIXME: the host is allowed to poll the shmem while the request is in
	 * flight, or read shmem when receiving the SCMI interrupt. Although
	 * it's unlikely with the SMC-based transport, this too requires some
	 * tightening in the spec.
	 */
	if (WARN_ON(__pkvm_host_donate_hyp(scmi_channel.shmem_pfn, 1)))
		return true;

	__kvm_host_scmi_handler(host_ctxt);

	WARN_ON(__pkvm_hyp_donate_host(scmi_channel.shmem_pfn, 1));
	return true; /* Handled */
}

int pkvm_init_scmi_pd(struct kvm_power_domain *pd,
		      const struct kvm_power_domain_ops *ops)
{
	int ret;

	if (!IS_ALIGNED(pd->arm_scmi.shmem_base, PAGE_SIZE) ||
	    pd->arm_scmi.shmem_size < PAGE_SIZE) {
		return -EINVAL;
	}

	if (!scmi_channel.shmem) {
		unsigned long shmem;

		/* FIXME: Do we need to mark those pages shared in the host s2? */
		ret = __pkvm_create_private_mapping(pd->arm_scmi.shmem_base,
						    pd->arm_scmi.shmem_size,
						    PAGE_HYP_DEVICE,
						    &shmem);
		if (ret)
			return ret;

		scmi_channel.smc_id = pd->arm_scmi.smc_id;
		scmi_channel.shmem_pfn = hyp_phys_to_pfn(pd->arm_scmi.shmem_base);
		scmi_channel.shmem = (void *)shmem;

	} else if (scmi_channel.shmem_pfn !=
		   hyp_phys_to_pfn(pd->arm_scmi.shmem_base) ||
		   scmi_channel.smc_id != pd->arm_scmi.smc_id) {
		/* We support a single channel at the moment */
		return -ENXIO;
	}

	if (scmi_power_domain_count == MAX_POWER_DOMAINS)
		return -ENOSPC;

	scmi_power_domains[scmi_power_domain_count].pd = pd;
	scmi_power_domains[scmi_power_domain_count].ops = ops;
	scmi_power_domain_count++;
	return 0;
}
