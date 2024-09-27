// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/delay.h>
#include <linux/mutex.h>

#include "apummu_cmn.h"
#include "apummu_drv.h"
#include "apummu_remote.h"
#include "linux/soc/mediatek/apummu_mem_def.h"
#include "apummu_msg.h"
#include "apummu_remote_cmd.h"


int apummu_remote_check_reply(void *reply)
{
	struct apummu_msg *msg;

	if (reply == NULL) {
		AMMU_LOG_ERR("Reply Null\n");
		return -EINVAL;
	}

	msg = (struct apummu_msg *)reply;
	if (msg->ack != 0) {
		AMMU_LOG_ERR("Reply Ack Error %x\n", msg->ack);
		return -EINVAL;
	}

	return 0;
}

int apummu_remote_set_op(void *drvinfo, uint32_t *argv, uint32_t argc)
{
	struct apummu_msg req, reply;
	uint32_t i = 0;
	int ret = 0;
	uint32_t max_data = 0;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		return -EINVAL;
	}

	max_data = ARRAY_SIZE(req.data);
	if (argc > max_data) {
		AMMU_LOG_ERR("invalid argc %d / %d\n", argc, max_data);
		return -EINVAL;
	}

	req.cmd = APUMMU_CMD_DBG_OP;
	req.option = APUMMU_OPTION_SET;

	for (i = 0; i < argc; i++)
		req.data[i] = argv[i];


	ret = apummu_remote_send_cmd_sync(drvinfo, (void *) &req, (void *) &reply, 0);
	if (ret) {
		AMMU_LOG_ERR("Send Msg Fail %d\n", ret);
		goto out;
	}
	ret = apummu_remote_check_reply((void *) &reply);
	if (ret) {
		AMMU_LOG_ERR("Check Msg Fail %d\n", ret);
		goto out;
	}

out:
	return ret;
}

int apummu_remote_send_stable(void *drvinfo, uint64_t session, uint32_t ammu_stable_addr,
					uint32_t SRAM_req_size, uint32_t table_size)
{
	struct apummu_msg req, reply;
	uint32_t op;
	int ret = 0, widx = 0;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		return -EINVAL;
	}

	req.cmd = APUMMU_CMD_DBG_OP;
	req.option = APUMMU_OPTION_SET;

	op = 1;

	AMMU_RPMSG_write(&op, req.data, sizeof(op), widx);
	AMMU_RPMSG_write(&session, req.data, sizeof(session), widx);
	AMMU_RPMSG_write(&ammu_stable_addr, req.data, sizeof(ammu_stable_addr), widx);
	AMMU_RPMSG_write(&SRAM_req_size, req.data, sizeof(SRAM_req_size), widx);
	AMMU_RPMSG_write(&table_size, req.data, sizeof(table_size), widx);

	ret = apummu_remote_send_cmd_sync(drvinfo, (void *) &req, (void *) &reply, 0);
	if (ret) {
		AMMU_LOG_ERR("Send Msg Fail %d\n", ret);
		goto out;
	}
	ret = apummu_remote_check_reply((void *) &reply);
	if (ret) {
		AMMU_LOG_ERR("Check Msg Fail %d\n", ret);
		goto out;
	}

out:
	return ret;
}

int apummu_remote_handshake(void *drvinfo, void *remote)
{
	struct apummu_dev_info *adv = NULL;
	struct apummu_msg req, reply;
	int ridx = 0;
	int ret = 0;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		return -EINVAL;
	}

	if (!apummu_is_remote()) {
		AMMU_LOG_ERR("Remote Not Init\n");
		return -EINVAL;
	}

	adv = (struct apummu_dev_info *)drvinfo;

	memset(&req, 0, sizeof(struct apummu_msg));
	memset(&reply, 0, sizeof(struct apummu_msg));


	req.cmd = APUMMU_CMD_HANDSHAKE;
	req.option = APUMMU_OPTION_GET;

	AMMU_LOG_INFO("Remote Handshake...\n");
	ret = apummu_remote_send_cmd_sync(drvinfo, (void *) &req, (void *) &reply, 0);
	if (ret) {
		AMMU_LOG_ERR("Remote Handshake Fail %d\n", ret);
		goto out;
	}

	ret = apummu_remote_check_reply((void *) &reply);
	if (ret) {
		AMMU_LOG_ERR("Check Msg Fail %d\n", ret);
		goto out;
	}

	/* Init Remote Info */
	AMMU_RPMSG_RAED(&adv->remote.dram_max, reply.data, sizeof(adv->remote.dram_max), ridx);
	AMMU_RPMSG_RAED(&adv->remote.vlm_addr, reply.data, sizeof(adv->remote.vlm_addr), ridx);
	AMMU_RPMSG_RAED(&adv->remote.vlm_size, reply.data, sizeof(adv->remote.vlm_size), ridx);
	AMMU_RPMSG_RAED(&adv->remote.SLB_base_addr, reply.data,
			sizeof(adv->remote.SLB_base_addr), ridx);
	AMMU_RPMSG_RAED(&adv->remote.SLB_EXT_addr, reply.data,
			sizeof(adv->remote.SLB_EXT_addr), ridx);
	AMMU_RPMSG_RAED(&adv->remote.SLB_size, reply.data,
			sizeof(adv->remote.SLB_size), ridx);

	adv->remote.TCM_base_addr = adv->remote.vlm_addr;
	adv->remote.general_SRAM_size = adv->remote.vlm_size;

	apummu_remote_sync_sn(drvinfo, reply.sn);
out:
	return ret;
}

int apummu_remote_set_hw_default_iova(void *drvinfo, uint32_t ctx, uint64_t iova)
{
	struct apummu_msg req, reply;
	int ret = 0;
	int widx = 0;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		return -EINVAL;
	}

	if (!apummu_is_remote()) {
		AMMU_LOG_ERR("Remote Not Init\n");
		return -EINVAL;
	}

	memset(&req, 0, sizeof(struct apummu_msg));
	memset(&reply, 0, sizeof(struct apummu_msg));

	req.cmd = APUMMU_CMD_HW_DEFAULT_IOVA;
	req.option = APUMMU_OPTION_SET;

	AMMU_RPMSG_write(&ctx, req.data, sizeof(ctx), widx);
	AMMU_RPMSG_write(&iova, req.data, sizeof(iova), widx);

	ret = apummu_remote_send_cmd_sync(drvinfo, (void *) &req, (void *) &reply, 0);
	if (ret) {
		AMMU_LOG_ERR("Send Msg Fail %d\n", ret);
		goto out;
	}
	ret = apummu_remote_check_reply((void *) &reply);
	if (ret) {
		AMMU_LOG_ERR("Check Msg Fail %d\n", ret);
		goto out;
	}
out:
	return ret;
}

int apummu_remote_set_hw_default_iova_one_shot(void *drvinfo)
{
	struct apummu_dev_info *adv = NULL;
	struct apummu_msg req, reply;
	int ret = 0;
	int widx = 0;
	uint32_t type, in_addr = 0, size = 0;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		return -EINVAL;
	}

	if (!apummu_is_remote()) {
		AMMU_LOG_ERR("Remote Not Init\n");
		return -EINVAL;
	}

	adv = (struct apummu_dev_info *)drvinfo;

	memset(&req, 0, sizeof(struct apummu_msg));
	memset(&reply, 0, sizeof(struct apummu_msg));

	req.cmd = APUMMU_CMD_HW_DEFAULT_IOVA_ONE_SHOT;
	req.option = APUMMU_OPTION_SET;

	type = APUMMU_MEM_TYPE_GENERAL_S;
	if (adv->rsc.genernal_SLB.iova != 0) { // if SLB allocated
		in_addr = (uint32_t) adv->rsc.genernal_SLB.iova;
		size = adv->rsc.genernal_SLB.size;
	}

	AMMU_RPMSG_write(&adv->rsc.vlm_dram.iova, req.data,
			sizeof(adv->rsc.vlm_dram.iova), widx);
	AMMU_RPMSG_write(&type, req.data, sizeof(type), widx);
	AMMU_RPMSG_write(&in_addr, req.data, sizeof(in_addr), widx);
	AMMU_RPMSG_write(&size, req.data, sizeof(size), widx);

	ret = apummu_remote_send_cmd_sync(drvinfo, (void *) &req, (void *) &reply, 0);
	if (ret) {
		AMMU_LOG_ERR("Send Msg Fail %d\n", ret);
		goto out;
	}
	ret = apummu_remote_check_reply((void *) &reply);
	if (ret) {
		AMMU_LOG_ERR("Check Msg Fail %d\n", ret);
		goto out;
	}
out:
	return ret;
}

int apummu_remote_mem_add_pool(void *drvinfo, uint32_t type,
	uint64_t in_addr, uint32_t size, uint32_t start_idx)
{
	struct apummu_msg req, reply;
	int ret = 0, widx = 0;
	uint32_t mem_op = 0;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		return -EINVAL;
	}

	if (!apummu_is_remote()) {
		AMMU_LOG_ERR("Remote Not Init\n");
		return -EINVAL;
	}

	memset(&req, 0, sizeof(struct apummu_msg));
	memset(&reply, 0, sizeof(struct apummu_msg));

	req.cmd = APUMMU_CMD_SYSTEM_RAM;
	req.option = APUMMU_OPTION_SET;

	mem_op = APUMMU_MEM_ADD_POOL;

	AMMU_RPMSG_write(&mem_op,    req.data, sizeof(mem_op),    widx);
	AMMU_RPMSG_write(&type,      req.data, sizeof(type),      widx);
	AMMU_RPMSG_write(&in_addr,   req.data, sizeof(in_addr),   widx);
	AMMU_RPMSG_write(&size,      req.data, sizeof(size),      widx);
	AMMU_RPMSG_write(&start_idx, req.data, sizeof(start_idx), widx);

	ret = apummu_remote_send_cmd_sync(drvinfo, (void *) &req, (void *) &reply, 0);
	if (ret) {
		AMMU_LOG_ERR("Send Msg Fail %d\n", ret);
		goto out;
	}
	ret = apummu_remote_check_reply((void *) &reply);
	if (ret) {
		AMMU_LOG_ERR("Check Msg Fail %d\n", ret);
		goto out;
	}

out:
	return ret;
}

int apummu_remote_mem_free_pool(void *drvinfo)
{
	struct apummu_dev_info *adv = NULL;
	struct apummu_msg req, reply;
	int ret = 0, widx = 0;
	uint32_t mem_op = 0, in_addr = 0, size = 0, type = 0;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		return -EINVAL;
	}

	if (!apummu_is_remote()) {
		AMMU_LOG_ERR("Remote Not Init\n");
		return -EINVAL;
	}

	adv = (struct apummu_dev_info *)drvinfo;

	memset(&req, 0, sizeof(struct apummu_msg));
	memset(&reply, 0, sizeof(struct apummu_msg));

	req.cmd = APUMMU_CMD_SYSTEM_RAM;
	req.option = APUMMU_OPTION_SET;

	mem_op = APUMMU_MEM_FREE_POOL;

	type = APUMMU_MEM_TYPE_GENERAL_S;
	in_addr = (uint32_t) adv->rsc.genernal_SLB.iova;
	size = adv->rsc.genernal_SLB.size;

	AMMU_RPMSG_write(&mem_op, req.data, sizeof(mem_op), widx);
	AMMU_RPMSG_write(&type, req.data, sizeof(type), widx);
	AMMU_RPMSG_write(&in_addr, req.data, sizeof(in_addr), widx);
	AMMU_RPMSG_write(&size, req.data, sizeof(size), widx);

	ret = apummu_remote_send_cmd_sync(drvinfo, (void *) &req, (void *) &reply, 0);
	if (ret) {
		AMMU_LOG_ERR("Send Msg Fail %d\n", ret);
		goto out;
	}
	ret = apummu_remote_check_reply((void *) &reply);
	if (ret) {
		AMMU_LOG_ERR("Check Msg Fail %d\n", ret);
		goto out;
	}

out:
	return ret;
}
