// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2018 MediaTek Inc.

#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/mailbox_controller.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

#define CMDQ_WRITE_ENABLE_MASK	BIT(0)
#define CMDQ_POLL_ENABLE_MASK	BIT(0)
/* dedicate the last GPR_R15 to assign the register address to be poll */
#define CMDQ_POLL_ADDR_GPR	(15)
#define CMDQ_EOC_IRQ_EN		BIT(0)
#define CMDQ_IMMEDIATE_VALUE	0
#define CMDQ_REG_TYPE		1
#define CMDQ_JUMP_RELATIVE	0
#define CMDQ_JUMP_ABSOLUTE	1

struct cmdq_instruction {
	union {
		u32 value;
		u32 mask;
		struct {
			u16 arg_c;
			u16 src_reg;
		};
	};
	union {
		u16 offset;
		u16 event;
		u16 reg_dst;
	};
	union {
		u8 subsys;
		struct {
			u8 sop:5;
			u8 arg_c_t:1;
			u8 src_t:1;
			u8 dst_t:1;
		};
	};
	u8 op;
};

static inline u8 cmdq_operand_get_type(struct cmdq_operand *op)
{
	return op->reg ? CMDQ_REG_TYPE : CMDQ_IMMEDIATE_VALUE;
}

static inline u16 cmdq_operand_get_idx_value(struct cmdq_operand *op)
{
	return op->reg ? op->idx : op->value;
}

int cmdq_dev_get_client_reg(struct device *dev,
			    struct cmdq_client_reg *client_reg, int idx)
{
	struct of_phandle_args spec;
	struct resource res;
	int err;

	if (!client_reg)
		return -ENOENT;

	if (of_address_to_resource(dev->of_node, 0, &res) != 0) {
		dev_err(dev, "Missing reg in %s node\n", dev->of_node->full_name);
		return -EINVAL;
	}
	client_reg->pa_base = res.start;

	err = of_parse_phandle_with_fixed_args(dev->of_node,
					       "mediatek,gce-client-reg",
					       3, idx, &spec);
	if (err < 0) {
		dev_dbg(dev,
			"error %d can't parse gce-client-reg property (%d)",
			err, idx);

		/* make subsys invalid */
		client_reg->subsys = CMDQ_SUBSYS_INVALID;

		return 0;
	}

	client_reg->subsys = (u8)spec.args[0];
	client_reg->offset = (u16)spec.args[1];
	client_reg->size = (u16)spec.args[2];
	of_node_put(spec.np);

	return 0;
}
EXPORT_SYMBOL(cmdq_dev_get_client_reg);

struct cmdq_client *cmdq_mbox_create(struct device *dev, int index)
{
	struct cmdq_client *client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return (struct cmdq_client *)-ENOMEM;

	client->client.dev = dev;
	client->client.tx_block = false;
	client->client.knows_txdone = true;
	client->chan = mbox_request_channel(&client->client, index);

	if (IS_ERR(client->chan)) {
		long err;

		dev_err(dev, "failed to request channel\n");
		err = PTR_ERR(client->chan);
		kfree(client);

		return ERR_PTR(err);
	}

	return client;
}
EXPORT_SYMBOL(cmdq_mbox_create);

void cmdq_mbox_destroy(struct cmdq_client *client)
{
	mbox_free_channel(client->chan);
	kfree(client);
}
EXPORT_SYMBOL(cmdq_mbox_destroy);

int cmdq_pkt_create(struct cmdq_client *client, struct cmdq_pkt *pkt, size_t size)
{
	struct device *dev;
	dma_addr_t dma_addr;

	pkt->va_base = kzalloc(size, GFP_KERNEL);
	if (!pkt->va_base)
		return -ENOMEM;

	pkt->buf_size = size;

	/* need to use pkt->cl->chan later to call mbox APIs when generating instruction */
	pkt->cl = (void *)client;

	dev = client->chan->mbox->dev;
	dma_addr = dma_map_single(dev, pkt->va_base, pkt->buf_size,
				  DMA_TO_DEVICE);
	if (dma_mapping_error(dev, dma_addr)) {
		dev_err(dev, "dma map failed, size=%u\n", (u32)(u64)size);
		kfree(pkt->va_base);
		return -ENOMEM;
	}

	pkt->pa_base = dma_addr;

	return 0;
}
EXPORT_SYMBOL(cmdq_pkt_create);

void cmdq_pkt_destroy(struct cmdq_client *client, struct cmdq_pkt *pkt)
{
	dma_unmap_single(client->chan->mbox->dev, pkt->pa_base, pkt->buf_size,
			 DMA_TO_DEVICE);
	kfree(pkt->va_base);
}
EXPORT_SYMBOL(cmdq_pkt_destroy);

static int cmdq_pkt_append_command(struct cmdq_pkt *pkt,
				   struct cmdq_instruction inst)
{
	struct cmdq_instruction *cmd_ptr;

	if (unlikely(pkt->cmd_buf_size + CMDQ_INST_SIZE > pkt->buf_size)) {
		/*
		 * In the case of allocated buffer size (pkt->buf_size) is used
		 * up, the real required size (pkt->cmdq_buf_size) is still
		 * increased, so that the user knows how much memory should be
		 * ultimately allocated after appending all commands and
		 * flushing the command packet. Therefor, the user can call
		 * cmdq_pkt_create() again with the real required buffer size.
		 */
		pkt->cmd_buf_size += CMDQ_INST_SIZE;
		WARN_ONCE(1, "%s: buffer size %u is too small !\n",
			__func__, (u32)pkt->buf_size);
		return -ENOMEM;
	}

	cmd_ptr = pkt->va_base + pkt->cmd_buf_size;
	*cmd_ptr = inst;
	pkt->cmd_buf_size += CMDQ_INST_SIZE;

	return 0;
}

static int cmdq_pkt_mask(struct cmdq_pkt *pkt, u32 mask)
{
	struct cmdq_instruction inst = {
		.op = CMDQ_CODE_MASK,
		.mask = ~mask
	};
	return cmdq_pkt_append_command(pkt, inst);
}

int cmdq_pkt_write(struct cmdq_pkt *pkt, u8 subsys, u16 offset, u32 value)
{
	struct cmdq_instruction inst = {
		.op = CMDQ_CODE_WRITE,
		.value = value,
		.offset = offset,
		.subsys = subsys
	};
	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_write);

int cmdq_pkt_write_mask(struct cmdq_pkt *pkt, u8 subsys,
			u16 offset, u32 value, u32 mask)
{
	u16 offset_mask = offset;
	int err;

	if (mask != GENMASK(31, 0)) {
		err = cmdq_pkt_mask(pkt, mask);
		if (err < 0)
			return err;

		offset_mask |= CMDQ_WRITE_ENABLE_MASK;
	}
	return cmdq_pkt_write(pkt, subsys, offset_mask, value);
}
EXPORT_SYMBOL(cmdq_pkt_write_mask);

int cmdq_pkt_read_s(struct cmdq_pkt *pkt, u16 high_addr_reg_idx, u16 addr_low,
		    u16 reg_idx)
{
	struct cmdq_instruction inst = {
		.op = CMDQ_CODE_READ_S,
		.dst_t = CMDQ_REG_TYPE,
		.sop = high_addr_reg_idx,
		.reg_dst = reg_idx,
		.src_reg = addr_low
	};
	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_read_s);

int cmdq_pkt_write_s(struct cmdq_pkt *pkt, u16 high_addr_reg_idx,
		     u16 addr_low, u16 src_reg_idx)
{
	struct cmdq_instruction inst = {
		.op = CMDQ_CODE_WRITE_S,
		.src_t = CMDQ_REG_TYPE,
		.sop = high_addr_reg_idx,
		.offset = addr_low,
		.src_reg = src_reg_idx
	};
	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_write_s);

int cmdq_pkt_write_s_mask(struct cmdq_pkt *pkt, u16 high_addr_reg_idx,
			  u16 addr_low, u16 src_reg_idx, u32 mask)
{
	struct cmdq_instruction inst = {
		.op = CMDQ_CODE_WRITE_S_MASK,
		.src_t = CMDQ_REG_TYPE,
		.sop = high_addr_reg_idx,
		.offset = addr_low,
		.src_reg = src_reg_idx,
	};
	int err;

	err = cmdq_pkt_mask(pkt, mask);
	if (err < 0)
		return err;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_write_s_mask);

int cmdq_pkt_write_s_value(struct cmdq_pkt *pkt, u8 high_addr_reg_idx,
			   u16 addr_low, u32 value)
{
	struct cmdq_instruction inst = {
		.op = CMDQ_CODE_WRITE_S,
		.sop = high_addr_reg_idx,
		.offset = addr_low,
		.value = value
	};
	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_write_s_value);

int cmdq_pkt_write_s_mask_value(struct cmdq_pkt *pkt, u8 high_addr_reg_idx,
				u16 addr_low, u32 value, u32 mask)
{
	struct cmdq_instruction inst = {
		.op = CMDQ_CODE_WRITE_S_MASK,
		.sop = high_addr_reg_idx,
		.offset = addr_low,
		.value = value
	};
	int err;

	err = cmdq_pkt_mask(pkt, mask);
	if (err < 0)
		return err;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_write_s_mask_value);

int cmdq_pkt_mem_move(struct cmdq_pkt *pkt, dma_addr_t src_addr, dma_addr_t dst_addr)
{
	struct cmdq_client *cl = (struct cmdq_client *)pkt->cl;
	const u16 high_addr_reg_idx  = CMDQ_THR_SPR_IDX0;
	const u16 value_reg_idx = CMDQ_THR_SPR_IDX1;
	int ret;

	if (!cl) {
		pr_err("%s %d: pkt->cl is NULL!\n", __func__, __LINE__);
		return -EINVAL;
	}

	if (cmdq_addr_need_offset(cl->chan, src_addr))
		src_addr += cmdq_get_offset_pa(cl->chan);

	if (cmdq_addr_need_offset(cl->chan, dst_addr))
		dst_addr += cmdq_get_offset_pa(cl->chan);

	/* read the value of src_addr into high_addr_reg_idx */
	ret = cmdq_pkt_assign(pkt, high_addr_reg_idx, CMDQ_ADDR_HIGH(src_addr));
	if (ret < 0)
		return ret;
	ret = cmdq_pkt_read_s(pkt, high_addr_reg_idx, CMDQ_ADDR_LOW(src_addr), value_reg_idx);
	if (ret < 0)
		return ret;

	/* write the value of value_reg_idx into dst_addr */
	ret = cmdq_pkt_assign(pkt, high_addr_reg_idx, CMDQ_ADDR_HIGH(dst_addr));
	if (ret < 0)
		return ret;
	ret = cmdq_pkt_write_s(pkt, high_addr_reg_idx, CMDQ_ADDR_LOW(dst_addr), value_reg_idx);
	if (ret < 0)
		return ret;

	return 0;
}
EXPORT_SYMBOL(cmdq_pkt_mem_move);

int cmdq_pkt_wfe(struct cmdq_pkt *pkt, u16 event, bool clear)
{
	u32 clear_option = clear ? CMDQ_WFE_UPDATE : 0;
	struct cmdq_instruction inst = {
		.op = CMDQ_CODE_WFE,
		.value = CMDQ_WFE_OPTION | clear_option,
		.event = event
	};

	if (event >= CMDQ_MAX_EVENT)
		return -EINVAL;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_wfe);

int cmdq_pkt_acquire_event(struct cmdq_pkt *pkt, u16 event)
{
	struct cmdq_instruction inst = {
		.op = CMDQ_CODE_WFE,
		.value = CMDQ_WFE_UPDATE | CMDQ_WFE_UPDATE_VALUE | CMDQ_WFE_WAIT,
		.event = event
	};

	if (event >= CMDQ_MAX_EVENT)
		return -EINVAL;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_acquire_event);

int cmdq_pkt_clear_event(struct cmdq_pkt *pkt, u16 event)
{
	struct cmdq_instruction inst = {
		.op = CMDQ_CODE_WFE,
		.value = CMDQ_WFE_UPDATE,
		.event = event
	};

	if (event >= CMDQ_MAX_EVENT)
		return -EINVAL;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_clear_event);

int cmdq_pkt_set_event(struct cmdq_pkt *pkt, u16 event)
{
	struct cmdq_instruction inst = {
		.op = CMDQ_CODE_WFE,
		.value = CMDQ_WFE_UPDATE | CMDQ_WFE_UPDATE_VALUE,
		.event = event
	};

	if (event >= CMDQ_MAX_EVENT)
		return -EINVAL;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_set_event);

int cmdq_pkt_poll(struct cmdq_pkt *pkt, u8 subsys,
		  u16 offset, u32 value)
{
	struct cmdq_instruction inst = {
		.op = CMDQ_CODE_POLL,
		.value = value,
		.offset = offset,
		.subsys = subsys
	};
	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_poll);

int cmdq_pkt_poll_mask(struct cmdq_pkt *pkt, u8 subsys,
		       u16 offset, u32 value, u32 mask)
{
	int err;

	err = cmdq_pkt_mask(pkt, mask);
	if (err < 0)
		return err;

	offset = offset | CMDQ_POLL_ENABLE_MASK;
	return cmdq_pkt_poll(pkt, subsys, offset, value);
}
EXPORT_SYMBOL(cmdq_pkt_poll_mask);

int cmdq_pkt_poll_addr(struct cmdq_pkt *pkt, dma_addr_t addr, u32 value, u32 mask)
{
	struct cmdq_client *cl = (struct cmdq_client *)pkt->cl;
	struct cmdq_instruction inst = { {0} };
	u8 use_mask = 0;
	int ret;

	if (!cl) {
		pr_err("%s %d: pkt->cl is NULL!\n", __func__, __LINE__);
		return -EINVAL;
	}

	if (cmdq_addr_need_offset(cl->chan, addr))
		addr += cmdq_get_offset_pa(cl->chan);

	/*
	 * Append an MASK instruction to set the mask for following POLL instruction
	 * which enables use_mask bit.
	 */
	if (mask != GENMASK(31, 0)) {
		ret = cmdq_pkt_mask(pkt, mask);
		if (ret < 0)
			return ret;
		use_mask = CMDQ_POLL_ENABLE_MASK;
	}

	/*
	 * POLL is an legacy operation in GCE and it does not support SPR and CMDQ_CODE_LOGIC,
	 * so it can not use cmdq_pkt_assign to keep polling register address to SPR.
	 * If user wants to poll a register address which doesn't have a subsys id,
	 * user needs to use GPR and CMDQ_CODE_MASK to move polling register address to GPR.
	 */
	inst.op = CMDQ_CODE_MASK;
	inst.dst_t = CMDQ_REG_TYPE;
	inst.sop = CMDQ_POLL_ADDR_GPR;
	inst.value = addr;
	ret = cmdq_pkt_append_command(pkt, inst);
	if (ret < 0)
		return ret;

	/* Append POLL instruction to poll the register address assign to GPR previously. */
	inst.op = CMDQ_CODE_POLL;
	inst.dst_t = CMDQ_REG_TYPE;
	inst.sop = CMDQ_POLL_ADDR_GPR;
	inst.offset = use_mask;
	inst.value = value;
	ret = cmdq_pkt_append_command(pkt, inst);
	if (ret < 0)
		return ret;

	return 0;
}
EXPORT_SYMBOL(cmdq_pkt_poll_addr);

int cmdq_pkt_logic_command(struct cmdq_pkt *pkt, u16 result_reg_idx,
			   struct cmdq_operand *left_operand,
			   enum cmdq_logic_op s_op,
			   struct cmdq_operand *right_operand)
{
	struct cmdq_instruction inst;

	if (!left_operand || !right_operand || s_op >= CMDQ_LOGIC_MAX)
		return -EINVAL;

	inst.value = 0;
	inst.op = CMDQ_CODE_LOGIC;
	inst.dst_t = CMDQ_REG_TYPE;
	inst.src_t = cmdq_operand_get_type(left_operand);
	inst.arg_c_t = cmdq_operand_get_type(right_operand);
	inst.sop = s_op;
	inst.reg_dst = result_reg_idx;
	inst.src_reg = cmdq_operand_get_idx_value(left_operand);
	inst.arg_c = cmdq_operand_get_idx_value(right_operand);

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_logic_command);

int cmdq_pkt_assign(struct cmdq_pkt *pkt, u16 reg_idx, u32 value)
{
	struct cmdq_instruction inst = {
		.op = CMDQ_CODE_LOGIC,
		.dst_t = CMDQ_REG_TYPE,
		.reg_dst = reg_idx,
		.value = value
	};
	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_assign);

int cmdq_pkt_jump_abs(struct cmdq_pkt *pkt, dma_addr_t addr, u8 shift_pa)
{
	struct cmdq_client *cl = (struct cmdq_client *)pkt->cl;
	struct cmdq_instruction inst = {
		.op = CMDQ_CODE_JUMP,
		.offset = CMDQ_JUMP_ABSOLUTE
	};

	if (!cl) {
		pr_err("%s %d: pkt->cl is NULL!\n", __func__, __LINE__);
		return -EINVAL;
	}

	if (cmdq_addr_need_offset(cl->chan, addr))
		addr += cmdq_get_offset_pa(cl->chan);

	inst.value = addr >> shift_pa;
	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_jump_abs);

int cmdq_pkt_jump_rel(struct cmdq_pkt *pkt, s32 offset, u8 shift_pa)
{
	struct cmdq_instruction inst = {
		.op = CMDQ_CODE_JUMP,
		.value = (u32)offset >> shift_pa
	};
	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_jump_rel);

int cmdq_pkt_eoc(struct cmdq_pkt *pkt)
{
	struct cmdq_instruction inst = {
		.op = CMDQ_CODE_EOC,
		.value = CMDQ_EOC_IRQ_EN
	};
	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_eoc);

int cmdq_pkt_finalize(struct cmdq_pkt *pkt)
{
	struct cmdq_instruction inst = { {0} };
	int err;

	/* insert EOC and generate IRQ for each command iteration */
	err = cmdq_pkt_eoc(pkt);
	if (err < 0)
		return err;

	/* JUMP to end */
	inst.op = CMDQ_CODE_JUMP;
	inst.value = CMDQ_JUMP_PASS >>
		cmdq_get_shift_pa(((struct cmdq_client *)pkt->cl)->chan);
	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_finalize);

int cmdq_sec_insert_backup_cookie(struct cmdq_pkt *pkt)
{
	struct cmdq_client *cl = (struct cmdq_client *)pkt->cl;
	struct cmdq_operand left, right;
	struct device *dev;
	dma_addr_t addr;

	if (!cl) {
		pr_err("%s %d: pkt->cl is NULL!\n", __func__, __LINE__);
		return -EINVAL;
	}
	dev = cl->chan->mbox->dev;

	addr = cmdq_sec_get_exec_cnt_addr(cl->chan);
	if (addr == 0) {
		dev_err(dev, "%s %d: failed to get exec cnt addr!\n",
			__func__, __LINE__);
		return -EINVAL;
	}

	cmdq_pkt_assign(pkt, CMDQ_THR_SPR_IDX1, CMDQ_ADDR_HIGH(addr));
	cmdq_pkt_read_s(pkt, CMDQ_THR_SPR_IDX1, CMDQ_ADDR_LOW(addr), CMDQ_THR_SPR_IDX1);

	left.reg = true;
	left.idx = CMDQ_THR_SPR_IDX1;
	right.reg = false;
	right.value = 1;
	cmdq_pkt_logic_command(pkt, CMDQ_THR_SPR_IDX1, &left, CMDQ_LOGIC_ADD, &right);

	addr = cmdq_sec_get_cookie_addr(cl->chan) + cmdq_get_offset_pa(cl->chan);
	if (addr == 0) {
		dev_err(dev, "%s %d: failed to get cookie addr!\n",
			__func__, __LINE__);
		return -EINVAL;
	}

	cmdq_pkt_assign(pkt, CMDQ_THR_SPR_IDX2, CMDQ_ADDR_HIGH(addr));
	cmdq_pkt_write_s(pkt, CMDQ_THR_SPR_IDX2, CMDQ_ADDR_LOW(addr), CMDQ_THR_SPR_IDX1);
	cmdq_pkt_set_event(pkt, cmdq_sec_get_eof_event_id(cl->chan));

	return 0;
}
EXPORT_SYMBOL_GPL(cmdq_sec_insert_backup_cookie);

void cmdq_sec_pkt_free_sec_data(struct cmdq_pkt *pkt)
{
	kfree(pkt->sec_data);
}
EXPORT_SYMBOL_GPL(cmdq_sec_pkt_free_sec_data);

int cmdq_sec_pkt_alloc_sec_data(struct cmdq_pkt *pkt)
{
	struct cmdq_sec_data *sec_data;

	if (pkt->sec_data) {
		memset(pkt->sec_data, 0, sizeof(struct cmdq_sec_data));
		return 0;
	}

	sec_data = kzalloc(sizeof(*sec_data), GFP_KERNEL);
	if (!sec_data)
		return -ENOMEM;

	pkt->sec_data = (void *)sec_data;

	return 0;
}
EXPORT_SYMBOL_GPL(cmdq_sec_pkt_alloc_sec_data);

static int cmdq_sec_append_metadata(struct cmdq_pkt *pkt,
				    const enum cmdq_iwc_addr_metadata_type type,
				    const u32 base, const u32 offset)
{
	struct cmdq_client *cl = (struct cmdq_client *)pkt->cl;
	struct device *dev = cl->chan->mbox->dev;
	struct cmdq_sec_data *sec_data;
	int idx;

	dev_dbg(dev, "[%s %d] pkt:%p type:%u base:%#x offset:%#x",
		__func__, __LINE__, pkt, type, base, offset);

	if (!pkt->sec_data) {
		dev_err(dev, "[%s %d] no sec_data\n", __func__, __LINE__);
		return -ENOMEM;
	}

	sec_data = (struct cmdq_sec_data *)pkt->sec_data;
	idx = sec_data->meta_cnt;
	if (idx >= CMDQ_IWC_MAX_ADDR_LIST_LENGTH) {
		dev_err(dev, "idx:%u reach over:%u", idx, CMDQ_IWC_MAX_ADDR_LIST_LENGTH);
		return -EFAULT;
	}

	sec_data->meta_list[idx].type = type;
	sec_data->meta_list[idx].base_handle = base;
	sec_data->meta_list[idx].offset = offset;
	sec_data->meta_cnt += 1;

	return 0;
}

int cmdq_sec_pkt_write(struct cmdq_pkt *pkt, u8 subsys, u32 pa_base, u16 offset,
		       enum cmdq_iwc_addr_metadata_type type,
		       u32 base, u32 base_offset)
{
	if (subsys != CMDQ_SUBSYS_INVALID)
		cmdq_pkt_write(pkt, subsys, offset, base);
	else {
		cmdq_pkt_assign(pkt, CMDQ_THR_SPR_IDX0, CMDQ_ADDR_HIGH(pa_base));
		cmdq_pkt_write_s_value(pkt, CMDQ_THR_SPR_IDX0, CMDQ_ADDR_LOW(offset), base);
	}

	return cmdq_sec_append_metadata(pkt, type, base, base_offset);
}
EXPORT_SYMBOL_GPL(cmdq_sec_pkt_write);

MODULE_LICENSE("GPL v2");
