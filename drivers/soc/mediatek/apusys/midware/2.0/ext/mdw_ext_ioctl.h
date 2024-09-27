/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#ifndef __MTK_APUEXT_IOCTL_H__
#define __MTK_APUEXT_IOCTL_H__

#include <linux/ioctl.h>
#include <linux/types.h>

#define APUEXT_MAGICNO 'E'
#define APUEXT_NODE_NAME "apuext"

enum MDW_EXT_HS_TYPE {
	MDW_EXT_HS_TYPE_VERSION,
};

struct mdw_ext_hs_in {
	uint64_t op;

	uint64_t reserve0;
	uint64_t reserve1;
	uint64_t reserve2;
};

struct mdw_ext_hs_out {
	uint64_t out0;
	uint64_t out1;
	uint64_t out2;
	uint64_t out3;
};

union mdw_ext_hs_args {
	struct mdw_ext_hs_in in;
	struct mdw_ext_hs_out out;
};

#define MDW_EXT_IOCTL_HS \
		_IOWR(APUEXT_MAGICNO, 1, union mdw_ext_hs_args)

struct mdw_ext_cmd_in {
	uint64_t ext_id;

	uint64_t reserve0;
	uint64_t reserve1;
	uint64_t reserve2;
};

struct mdw_ext_cmd_out {
	int64_t fence;
	uint64_t u_done;

	uint64_t reserve0;
	uint64_t reserve1;
};

union mdw_ext_cmd_args {
	struct mdw_ext_cmd_in in;
	struct mdw_ext_cmd_out out;
};

#define MDW_EXT_IOCTL_CMD \
	_IOWR(APUEXT_MAGICNO, 2, union mdw_ext_cmd_args)

#define MDW_EXT_IOCTL_START (1)
#define MDW_EXT_IOCTL_END (2)

#endif
