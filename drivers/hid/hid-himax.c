// SPDX-License-Identifier: GPL-2.0
/*
 * Himax hx83102j SPI Driver Code for HID.
 *
 * Copyright (C) 2024 Himax Corporation.
 */

#include "hid-himax.h"

static int himax_cable_detect_func(struct himax_ts_data *ts, bool force_renew);
static int himax_chip_init(struct himax_ts_data *ts);
static int himax_get_data(struct himax_ts_data *ts, u8 *data);
static int himax_heatmap_data_init(struct himax_ts_data *ts);
static int himax_platform_init(struct himax_ts_data *ts);
static int himax_switch_data_type(struct himax_ts_data *ts, u32 type);
static void himax_self_test(struct work_struct *work);
static void himax_ts_work(struct himax_ts_data *ts);

static const unsigned char g_windows_blob_validation_key[] = {
	0xfc, 0x28, 0xfe, 0x84, 0x40, 0xcb, 0x9a, 0x87, 0x0d, 0xbe, 0x57, 0x3c, 0xb6, 0x70,
	0x09, 0x88, 0x07, 0x97, 0x2d, 0x2b, 0xe3, 0x38, 0x34, 0xb6, 0x6c, 0xed, 0xb0, 0xf7,
	0xe5, 0x9c, 0xf6, 0xc2,	0x2e, 0x84, 0x1b, 0xe8, 0xb4, 0x51, 0x78, 0x43, 0x1f, 0x28,
	0x4b, 0x7c, 0x2d, 0x53, 0xaf, 0xfc, 0x47, 0x70, 0x1b, 0x59, 0x6f, 0x74, 0x43, 0xc4,
	0xf3, 0x47, 0x18, 0x53, 0x1a, 0xa2, 0xa1, 0x71,	0xc7, 0x95, 0x0e, 0x31, 0x55, 0x21,
	0xd3, 0xb5, 0x1e, 0xe9, 0x0c, 0xba, 0xec, 0xb8, 0x89, 0x19, 0x3e, 0xb3, 0xaf, 0x75,
	0x81, 0x9d, 0x53, 0xb9, 0x41, 0x57, 0xf4, 0x6d, 0x39, 0x25, 0x29, 0x7c,	0x87, 0xd9,
	0xb4, 0x98, 0x45, 0x7d, 0xa7, 0x26, 0x9c, 0x65, 0x3b, 0x85, 0x68, 0x89, 0xd7, 0x3b,
	0xbd, 0xff, 0x14, 0x67, 0xf2, 0x2b, 0xf0, 0x2a, 0x41, 0x54, 0xf0, 0xfd, 0x2c, 0x66,
	0x7c, 0xf8, 0xc0, 0x8f, 0x33, 0x13, 0x03, 0xf1, 0xd3, 0xc1, 0x0b, 0x89, 0xd9, 0x1b,
	0x62, 0xcd, 0x51, 0xb7,	0x80, 0xb8, 0xaf, 0x3a, 0x10, 0xc1, 0x8a, 0x5b, 0xe8, 0x8a,
	0x56, 0xf0, 0x8c, 0xaa, 0xfa, 0x35, 0xe9, 0x42, 0xc4, 0xd8, 0x55, 0xc3, 0x38, 0xcc,
	0x2b, 0x53, 0x5c, 0x69, 0x52, 0xd5, 0xc8, 0x73,	0x02, 0x38, 0x7c, 0x73, 0xb6, 0x41,
	0xe7, 0xff, 0x05, 0xd8, 0x2b, 0x79, 0x9a, 0xe2, 0x34, 0x60, 0x8f, 0xa3, 0x32, 0x1f,
	0x09, 0x78, 0x62, 0xbc, 0x80, 0xe3, 0x0f, 0xbd, 0x65, 0x20, 0x08, 0x13,	0xc1, 0xe2,
	0xee, 0x53, 0x2d, 0x86, 0x7e, 0xa7, 0x5a, 0xc5, 0xd3, 0x7d, 0x98, 0xbe, 0x31, 0x48,
	0x1f, 0xfb, 0xda, 0xaf, 0xa2, 0xa8, 0x6a, 0x89, 0xd6, 0xbf, 0xf2, 0xd3, 0x32, 0x2a,
	0x9a, 0xe4, 0xcf, 0x17, 0xb7, 0xb8, 0xf4, 0xe1, 0x33, 0x08, 0x24, 0x8b, 0xc4, 0x43,
	0xa5, 0xe5, 0x24, 0xc2
};

/* Extension report descriptor for HIDRAW debug function */
static union himax_host_ext_rd g_host_ext_rd = {
	.host_report_descriptor = {
		0x06, 0x00, 0xff,/* Usage Page (Vendor-defined) */
		0x09, 0x01,/* Usage (0x1) */
		0xa1, 0x01,/* Collection (Application) */
		0x75, 0x08,/* Report Size (8) */
		0x15, 0x00,/* Logical Minimum (0) */
		0x26, 0xff, 0x00,/* Logical Maximum (255) */
		0x85, HIMAX_ID_CFG,/* Report ID (5) */
		0x09, 0x02,/* Usage (0x2) */
		0x96, 0xff, 0x00,/* Report Count (255) */
		0xb1, 0x02,/* Feature (ID: 5, sz: 2040 bits(255 bytes)) */
		0x85, HIMAX_ID_REG_RW,/* Report ID (6) */
		0x09, 0x02,/* Usage (0x2) */
		0x96, (HIMAX_HID_REG_SZ_MAX & 0xff), (HIMAX_HID_REG_SZ_MAX >> 8),
		0xb1, 0x02,/* Feature (ID: 6, sz: 72 bits(9 bytes)) */
		0x85, HIMAX_ID_TOUCH_MONITOR_SEL,/* Report ID (7) */
		0x09, 0x02,/* Usage (0x2) */
		0x96, 0x04, 0x00,/* Report Count (4) */
		0xb1, 0x02,/* Feature (ID: 7, sz: 32 bits(4 bytes)) */
		0x85, HIMAX_ID_TOUCH_MONITOR,/* Report ID (8) */
		0x09, 0x02,/* Usage (0x2) */
		0x96, 0x8d, 0x13,/* Report Count (5005) */
		0xb1, 0x02,/* Feature (ID: 8, sz: 40040 bits(5005 bytes)) */
		0x85, HIMAX_ID_FW_UPDATE,/* Report ID (10) */
		0x09, 0x02,/* Usage (0x2) */
		0x96, 0x00, 0x04,/* Report Count (1024) */
		0x91, 0x02,/* Output (ID: 10, sz: 8192 bits(1024 bytes)) */
		0x85, HIMAX_ID_FW_UPDATE_HANDSHAKING,/* Report ID (11) */
		0x09, 0x02,/* Usage (0x2) */
		0x96, 0x01, 0x00,/* Report Count (1) */
		0xb1, 0x02,/* Feature (ID: 11, sz: 8 bits(1 bytes)) */
		0x85, HIMAX_ID_SELF_TEST,/* Report ID (12) */
		0x09, 0x02,/* Usage (0x2) */
		0x96, 0x01, 0x00,/* Report Count (1) */
		0xb1, 0x02,/* Feature (ID: 12, sz: 8 bits(1 bytes)) */
		0x85, HIMAX_ID_INPUT_RD_DE,/* Report ID (49) */
		0x09, 0x02,/* Usage (0x2) */
		0x96, 0x01, 0x00,/* Report Count (1) */
		0xb1, 0x02,/* Feature (ID: 49, sz: 8 bits(1 bytes)) */
		0x85, HIMAX_ID_WINDOWS_BLOB_VALID,
		0x09, 0xc5,/* Usage (0xc5) */
		0x96, sizeof(g_windows_blob_validation_key) & 0xff,
		(sizeof(g_windows_blob_validation_key) >> 8) & 0xff,/* Report Count (256) */
		0xb1, 0x02,/* Feature (ID: 50, sz: 2048 bits(256 bytes)) */
		0xc0,/* End Collection */
	},
};

static const unsigned int g_host_ext_report_desc_sz =
	sizeof(g_host_ext_rd.host_report_descriptor);

/* Dummy FW layout for user space tool update reference */
static const struct himax_hid_fw_unit g_dummy_main_code[9] = {
	{
		/* 0xa1 means part 0 ready, can send this part of FW */
		.cmd = 0xa1,
		.bin_start_offset = 0,
		.unit_sz = 127,
	},
	{
		/* 0xa2 means part 1 ready, can send this part of FW */
		.cmd = 0xa2,
		.bin_start_offset = 129,
		.unit_sz = 111,
	},
};

static const u16 g_himax_hid_raw_data_type[HIMAX_HID_RAW_DATA_TYPE_MAX] = {
	HIMAX_HID_RAW_DATA_TYPE_DELTA,
	HIMAX_HID_RAW_DATA_TYPE_RAW,
	HIMAX_HID_RAW_DATA_TYPE_BASELINE,
	HIMAX_HID_RAW_DATA_TYPE_NORMAL
};

/* Heatmap report descriptor for input disabled mode */
static union himax_heatmap_rd g_heatmap_rd = {
	.host_report_descriptor = {
		0x05, 0x0d,/* Usage Page (Digitizers) */
		0x09, 0x0f,/* Usage (0xf) */
		0xa1, 0x01,/* Collection (Application) */
		0x85, 0x61,/* Report ID (97) */
		0x05, 0x0d,/* Usage Page (Digitizers) */
		0x15, 0x00,/* Logical Minimum (0) */
		0x27, 0xff, 0xff, 0x00, 0x00,/* Logical Maximum (65535) */
		0x75, 0x10,/* Report Size (16) */
		0x95, 0x01,/* Report Count (1) */
		0x09, 0x6a,/* Usage (0x6a) */
		0x81, 0x02,/* Input (ID: 97, sz: 16 bits(2 bytes)) */
		0x09, 0x6b,/* Usage (0x6b) */
		0x81, 0x02,/* Input (ID: 97, sz: 16 bits(2 bytes)) */
		0x27, 0xff, 0xff, 0xff, 0xff,/* Logical Maximum (-1) */
		0x75, 0x20,/* Report Size (32) */
		0x09, 0x56,/* Usage (0x56) */
		0x81, 0x02,/* Input (ID: 97, sz: 32 bits(4 bytes)) */
		0x05, 0x01,/* Usage Page (Generic Desktop) */
		0x09, 0x3b,/* Usage (0x3b) */
		0x81, 0x02,/* Input (ID: 97, sz: 32 bits(4 bytes)) */
		0x05, 0x0d,/* Usage Page (Digitizers) */
		0x26, 0xff, 0x00,/* Logical Maximum (255) */
		0x09, 0x6c,/* Usage (0x6c) */
		0x75, 0x08,/* Report Size (8) */
		0x96, 0x00, 0x0c,/* Report Count (3072) */
		0x81, 0x02,/* Input (ID: 97, sz: 24576 bits(3072 bytes)) */
		0xc0,/* End Collection */
	},
};

static const unsigned int g_host_heatmap_report_desc_sz =
	sizeof(g_heatmap_rd.host_report_descriptor);

/* Need to map HID_INSPECTION_ENUM */
static char *g_himax_inspection_mode[] = {
	"HIMAX_OPEN",
	"HIMAX_MICRO_OPEN",
	"HIMAX_SHORT",
	"HIMAX_ABS_NOISE",
	"HIMAX_RAWDATA",
	"HIMAX_SORTING",
	"HIMAX_BACK_NORMAL",
	NULL
};

static const u16 g_hx_data_type[HIMAX_DATA_TYPE_MAX] = {
	HIMAX_DATA_TYPE_SORTING,
	HIMAX_DATA_TYPE_OPEN,
	HIMAX_DATA_TYPE_MICRO_OPEN,
	HIMAX_DATA_TYPE_SHORT,
	HIMAX_DATA_TYPE_RAWDATA,
	HIMAX_DATA_TYPE_NOISE,
	HIMAX_DATA_TYPE_BACK_NORMAL,
};

/**
 * himax_spi_read() - Read data from SPI
 * @ts: Himax touch screen data
 * @cmd_len: Length of command
 * @buf: Buffer to store data
 * @len: Length of data to read
 *
 * Himax spi_sync wrapper for read. Read protocol start with write command,
 * and received the data after that.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_spi_read(struct himax_ts_data *ts, u8 cmd_len, u8 *buf, u32 len)
{
	int ret;
	int retry_cnt;
	struct spi_message msg;
	struct spi_transfer xfer = {
		.len = cmd_len + len,
		.tx_buf = ts->xfer_tx_data,
		.rx_buf = ts->xfer_rx_data
	};

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	for (retry_cnt = 0; retry_cnt < HIMAX_BUS_RETRY; retry_cnt++) {
		ret = spi_sync(ts->spi, &msg);
		if (!ret)
			break;
	}

	if (retry_cnt == HIMAX_BUS_RETRY) {
		dev_err(ts->dev, "%s: SPI read error retry over %d\n", __func__, HIMAX_BUS_RETRY);
		return -EIO;
	}

	if (ret < 0)
		return ret;

	if (msg.status < 0)
		return msg.status;

	memcpy(buf, ts->xfer_rx_data + cmd_len, len);

	return 0;
}

/**
 * himax_spi_write() - Write data to SPI
 * @ts: Himax touch screen data
 * @tx_buf: Buffer to write
 * @tx_len: Length of data to write
 * @written: Length of data written
 *
 * Himax spi_sync wrapper for write.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_spi_write(struct himax_ts_data *ts, u8 *tx_buf, u32 tx_len, u32 *written)
{
	int ret;
	struct spi_message msg;
	struct spi_transfer xfer = {
		.tx_buf = tx_buf,
		.len = tx_len,
	};

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	*written = 0;
	ret = spi_sync(ts->spi, &msg);

	if (ret < 0)
		return ret;

	if (msg.status < 0)
		return msg.status;

	*written = msg.actual_length;

	return 0;
}

/**
 * himax_read() - Read data from Himax bus
 * @ts: Himax touch screen data
 * @cmd: Command to send
 * @buf: Buffer to store data, caller should allocate the buffer
 * @len: Length of data to read
 *
 * Basic read operation for Himax SPI bus. Which start with a 3 bytes command,
 * 1st byte is the spi function select, 2nd byte is the command belong to the
 * spi function and 3rd byte is the dummy byte for IC to process the command.
 *
 * The IC takes 1 basic operation at a time, so the read/write operation
 * is proctected by rw_lock mutex_unlock. Also the buffer xfer_rx/tx_data is
 * shared between read and write operation, protected by the same mutex lock.
 * The xfer data limit by SPI constroller max xfer size + BUS_R/W_HLEN
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_read(struct himax_ts_data *ts, u8 cmd, u8 *buf, u32 len)
{
	int ret;

	if (len + HIMAX_BUS_R_HLEN > ts->spi_xfer_max_sz) {
		dev_err(ts->dev, "%s, len[%u] is over %u\n", __func__,
			len + HIMAX_BUS_R_HLEN, ts->spi_xfer_max_sz);
		return -EINVAL;
	}

	mutex_lock(&ts->rw_lock);

	memset(ts->xfer_rx_data, 0, HIMAX_BUS_R_HLEN + len);
	ts->xfer_tx_data[0] = HIMAX_SPI_FUNCTION_READ;
	ts->xfer_tx_data[1] = cmd;
	ts->xfer_tx_data[2] = 0x00;
	ret = himax_spi_read(ts, HIMAX_BUS_R_HLEN, buf, len);

	mutex_unlock(&ts->rw_lock);
	if (ret < 0)
		dev_err(ts->dev, "%s: failed = %d\n", __func__, ret);

	return ret;
}

/**
 * himax_write() - Write data to Himax bus
 * @ts: Himax touch screen data
 * @cmd: Command to send
 * @addr: Address to write
 * @data: Data to write
 * @len: Length of data to write
 *
 * Basic write operation for Himax IC. Which start with a 2 bytes command,
 * 1st byte is the spi function select and 2nd byte is the command belong to the
 * spi function. Else is the data to write.
 *
 * The IC takes 1 basic operation at a time, so the read/write operation
 * is proctected by rw_lock mutex_unlock. Also the buffer xfer_tx_data is
 * shared between read and write operation, protected by the same mutex lock.
 * The xfer data limit by SPI constroller max xfer size + HIMAX_BUS_W_HLEN
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_write(struct himax_ts_data *ts, u8 cmd, u8 *addr, const u8 *data, u32 len)
{
	int ret;
	u8 offset;
	u32 written;
	u32 tmp_len;

	if (len + HIMAX_BUS_W_HLEN > ts->spi_xfer_max_sz) {
		dev_err(ts->dev, "%s: len[%u] is over %u\n", __func__,
			len + HIMAX_BUS_W_HLEN, ts->spi_xfer_max_sz);
		return -EFAULT;
	}

	mutex_lock(&ts->rw_lock);

	memset(ts->xfer_tx_data, 0, len + HIMAX_BUS_W_HLEN);
	ts->xfer_tx_data[0] = HIMAX_SPI_FUNCTION_WRITE;
	ts->xfer_tx_data[1] = cmd;
	offset = HIMAX_BUS_W_HLEN;
	tmp_len = len;

	if (addr) {
		memcpy(ts->xfer_tx_data + offset, addr, 4);
		offset += 4;
		tmp_len -= 4;
	}

	if (data)
		memcpy(ts->xfer_tx_data + offset, data, tmp_len);

	ret = himax_spi_write(ts, ts->xfer_tx_data, len + HIMAX_BUS_W_HLEN, &written);

	mutex_unlock(&ts->rw_lock);

	if (ret < 0) {
		dev_err(ts->dev, "%s: failed, ret = %d\n", __func__, ret);
		return ret;
	}

	if (written != len + HIMAX_BUS_W_HLEN) {
		dev_err(ts->dev, "%s: actual write length mismatched: %u != %u\n",
			__func__, written, len + HIMAX_BUS_W_HLEN);
		return -EIO;
	}

	return 0;
}

/**
 * himax_mcu_set_burst_mode() - Set burst mode for MCU
 * @ts: Himax touch screen data
 * @auto_add_4_byte: Enable auto add 4 byte mode
 *
 * Set burst mode for MCU, which is used for read/write data from/to MCU.
 * HIMAX_AHB_ADDR_CONTI config the IC to take data continuously,
 * HIMAX_AHB_ADDR_INCR4 config the IC to auto increment the address by 4 byte when
 * each 4 bytes read/write.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_set_burst_mode(struct himax_ts_data *ts, bool auto_add_4_byte)
{
	int ret;
	u8 tmp_data[HIMAX_REG_SZ];

	tmp_data[0] = HIMAX_AHB_CMD_CONTI;

	ret = himax_write(ts, HIMAX_AHB_ADDR_CONTI, NULL, tmp_data, 1);
	if (ret < 0) {
		dev_err(ts->dev, "%s: write ahb_addr_conti failed\n", __func__);
		return ret;
	}

	tmp_data[0] = HIMAX_AHB_CMD_INCR4;
	if (auto_add_4_byte)
		tmp_data[0] |= HIMAX_AHB_CMD_INCR4_ADD_4_BYTE;

	ret = himax_write(ts, HIMAX_AHB_ADDR_INCR4, NULL, tmp_data, 1);
	if (ret < 0)
		dev_err(ts->dev, "%s: write ahb_addr_incr4 failed\n", __func__);

	return ret;
}

/**
 * himax_burst_mode_enable() - Enable burst mode for MCU if possible
 * @ts: Himax touch screen data
 * @addr: Address to read/write
 * @len: Length of data to read/write
 *
 * Enable burst mode for MCU, helper function to determine the burst mode
 * operation for MCU. When the address is HIMAX_REG_ADDR_SPI200_DATA, the burst
 * mode is disabled. When the length of data is over HIMAX_REG_SZ, the burst
 * mode is enabled. Else the burst mode is disabled.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_burst_mode_enable(struct himax_ts_data *ts, u32 addr, u32 len)
{
	int ret;

	if (addr == HIMAX_REG_ADDR_SPI200_DATA)
		ret = himax_mcu_set_burst_mode(ts, false);
	else if (len > HIMAX_REG_SZ)
		ret = himax_mcu_set_burst_mode(ts, true);
	else
		ret = himax_mcu_set_burst_mode(ts, false);

	if (ret)
		dev_err(ts->dev, "%s: burst enable fail!\n", __func__);

	return ret;
}

/**
 * himax_mcu_register_read() - Read data from IC register/sram
 * @ts: Himax touch screen data
 * @addr: Address to read
 * @buf: Buffer to store data, caller should allocate the buffer
 * @len: Length of data to read
 *
 * Himax TP IC has its internal register and SRAM, this function is used to
 * read data from it. The reading protocol require a sequence of write and read,
 * which include write address to IC and read data from IC. Thus the read/write
 * operation is proctected by reg_lock mutex_unlock to protect the sequence.
 * The first step is to set the burst mode for MCU, then write the address to
 * AHB register to tell where to read. Then set the access direction to read,
 * and read the data from AHB register. The max length of data to read is decided
 * by AHB register max transfer size, but if it could't bigger then SPI controller
 * max transfer size. When the length of data is over the max transfer size,
 * the data will be read in multiple times.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_register_read(struct himax_ts_data *ts, u32 addr, u8 *buf, u32 len)
{
	int i;
	int ret;
	u8 direction_switch = HIMAX_AHB_CMD_ACCESS_DIRECTION_READ;
	u32 read_sz;
	const u32 max_trans_sz =
		min(HIMAX_HX83102J_REG_XFER_MAX, ts->spi_xfer_max_sz - HIMAX_BUS_R_HLEN);
	union himax_dword_data target_addr;

	mutex_lock(&ts->reg_lock);

	ret = himax_burst_mode_enable(ts, addr, len);
	if (ret)
		goto read_end;

	for (i = 0; i < len; i += read_sz) {
		target_addr.dword = cpu_to_le32(addr + i);
		ret = himax_write(ts, HIMAX_AHB_ADDR_BYTE_0, target_addr.byte, NULL, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: write ahb_addr_byte_0 failed\n", __func__);
			goto read_end;
		}

		ret = himax_write(ts, HIMAX_AHB_ADDR_ACCESS_DIRECTION, NULL,
				  &direction_switch, 1);
		if (ret < 0) {
			dev_err(ts->dev, "%s: write ahb_addr_access_direction failed\n", __func__);
			goto read_end;
		}

		read_sz = min((len - i), max_trans_sz);
		ret = himax_read(ts, HIMAX_AHB_ADDR_RDATA_BYTE_0, buf + i, read_sz);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read ahb_addr_rdata_byte_0 failed\n", __func__);
			goto read_end;
		}
	}

read_end:
	mutex_unlock(&ts->reg_lock);
	if (ret < 0)
		dev_err(ts->dev, "%s: addr = 0x%08X, len = %u, ret = %d\n", __func__,
			addr, len, ret);

	return ret;
}

/**
 * himax_mcu_register_write() - Write data to IC register/sram
 * @ts: Himax touch screen data
 * @addr: Address to write
 * @buf: Data to write
 * @len: Length of data to write
 *
 * Himax TP IC has its internal register and SRAM, this function is used to
 * write data to it. The writing protocol require a sequence of write, which
 * include write address to IC and write data to IC. Thus the write operation
 * is proctected by reg_lock mutex_unlock to protect the sequence. The first
 * step is to set the burst mode for MCU, then write the address and data to
 * AHB register. The max length of data to read is decided by AHB register max
 * transfer size, but if it could't bigger then SPI controller max transfer
 * size. When the length of data is over the max transfer size, the data will
 * be written in multiple times.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_register_write(struct himax_ts_data *ts, u32 addr, const u8 *buf, u32 len)
{
	int i;
	int ret;
	u32 write_sz;
	const u32 max_trans_sz = min(HIMAX_HX83102J_REG_XFER_MAX,
				     ts->spi_xfer_max_sz - HIMAX_BUS_W_HLEN - HIMAX_REG_SZ);
	union himax_dword_data target_addr;

	mutex_lock(&ts->reg_lock);

	ret = himax_burst_mode_enable(ts, addr, len);
	if (ret)
		goto write_end;

	for (i = 0; i < len; i += max_trans_sz) {
		write_sz = min((len - i), max_trans_sz);
		target_addr.dword = cpu_to_le32(addr + i);
		ret = himax_write(ts, HIMAX_AHB_ADDR_BYTE_0,
				  target_addr.byte, buf + i, write_sz + HIMAX_REG_SZ);
		if (ret < 0) {
			dev_err(ts->dev, "%s: write ahb_addr_byte_0 failed\n", __func__);
			break;
		}
	}

write_end:
	mutex_unlock(&ts->reg_lock);
	if (ret < 0)
		dev_err(ts->dev, "%s: addr = 0x%08X, len = %u, ret = %d\n", __func__,
			addr, len, ret);

	return ret;
}

/**
 * himax_write_read_reg() - Write and read back data for handshake confirm
 * @ts: Himax touch screen data
 * @addr: Address to write
 * @data: Data to write
 * @hb: High byte of confirmation data
 * @lb: Low byte of confirmation data
 *
 * Write data to IC register/sram and read back for handshake confirm. The
 * process contain two stage, first stage is to write the data to IC,
 * then read back the data from the same address to confirm the data is written
 * successfully. The second stage is to read back data from the same address
 * again to confirm handshake password is the same as confirmation data,
 * which means the operation is done successfully. There may had a chance that
 * data read back equals to confirmation data at first stage, in this case, return
 * operation complete directly.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_write_read_reg(struct himax_ts_data *ts, u32 addr, u8 *data, u8 hb, u8 lb)
{
	int ret;
	u32 retry_cnt;
	const u32 write_confirm_retry = 40;
	const u32 read_confirm_retry = 200;
	union himax_dword_data r_data, t_data;

	memcpy(t_data.byte, data, HIMAX_REG_SZ);
	for (retry_cnt = 0; retry_cnt < write_confirm_retry; retry_cnt++) {
		ret = himax_mcu_register_read(ts, addr, r_data.byte, HIMAX_REG_SZ);
		if (ret < 0) {
			usleep_range(1000, 1100);
			continue;
		}
		if (r_data.byte[1] == t_data.byte[1] && r_data.byte[0] == t_data.byte[0])
			break;
		else if (r_data.byte[1] == hb && r_data.byte[0] == lb)
			return 0;

		himax_mcu_register_write(ts, addr, t_data.byte, HIMAX_REG_SZ);
		usleep_range(1000, 1100);
	}

	if (retry_cnt == write_confirm_retry)
		goto err_retry_over;

	for (retry_cnt = 0; retry_cnt < read_confirm_retry; retry_cnt++) {
		ret = himax_mcu_register_read(ts, addr, r_data.byte, HIMAX_REG_SZ);
		if (ret < 0) {
			usleep_range(1000, 1100);
			continue;
		}
		if (r_data.byte[1] == hb && r_data.byte[0] == lb)
			return 0;

		usleep_range(10000, 10100);
	}

err_retry_over:
	dev_err(ts->dev, "%s: failed to handshaking with DSRAM\n", __func__);
	dev_err(ts->dev, "%s: addr = 0x%08X; data = 0x%02X%02X%02X%02X\n", __func__,
		addr, data[3], data[2], data[1], data[0]);
	dev_err(ts->dev, "%s: target = %02X%02X; r_data = %02X%02X\n", __func__,
		hb, lb, r_data.byte[1], r_data.byte[0]);

	return -EIO;
}

/**
 * himax_mcu_interface_on() - Wakeup IC bus interface
 * @ts: Himax touch screen data
 *
 * This function is used to wakeup IC bus interface. The IC may enter sleep mode
 * and need to wakeup before any operation. The wakeup process is to read a dummy
 * AHB register to wakeup the IC bus interface. Also, the function setup the burst
 * mode as default for MCU and read back the burst mode setting to confirm the
 * setting is written. The action is a double check to confirm the IC bus interface
 * is ready for operation.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_interface_on(struct himax_ts_data *ts)
{
	int ret;
	u8 buf[2][HIMAX_REG_SZ];
	u32 retry_cnt;
	const u32 burst_retry_limit = 10;

	mutex_lock(&ts->reg_lock);
	/* Read a dummy register to wake up BUS. */
	ret = himax_read(ts, HIMAX_AHB_ADDR_RDATA_BYTE_0, buf[0], 4);
	mutex_unlock(&ts->reg_lock);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read ahb_addr_rdata_byte_0 failed\n", __func__);
		return ret;
	}

	for (retry_cnt = 0; retry_cnt < burst_retry_limit; retry_cnt++) {
		/* AHB: read/write to SRAM in sequential order */
		buf[0][0] = HIMAX_AHB_CMD_CONTI;
		ret = himax_write(ts, HIMAX_AHB_ADDR_CONTI, NULL, buf[0], 1);
		if (ret < 0) {
			dev_err(ts->dev, "%s: write ahb_addr_conti failed\n", __func__);
			return ret;
		}

		/* AHB: Auto increment SRAM addr+4 while each 4 bytes read/write */
		buf[0][0] = HIMAX_AHB_CMD_INCR4;
		ret = himax_write(ts, HIMAX_AHB_ADDR_INCR4, NULL, buf[0], 1);
		if (ret < 0) {
			dev_err(ts->dev, "%s: write ahb_addr_incr4 failed\n", __func__);
			return ret;
		}

		/* Check cmd */
		ret = himax_read(ts, HIMAX_AHB_ADDR_CONTI, buf[0], 1);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read ahb_addr_conti failed\n", __func__);
			return ret;
		}

		ret = himax_read(ts, HIMAX_AHB_ADDR_INCR4, buf[1], 1);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read ahb_addr_incr4 failed\n", __func__);
			return ret;
		}

		if (buf[0][0] == HIMAX_AHB_CMD_CONTI && buf[1][0] == HIMAX_AHB_CMD_INCR4)
			return 0;

		usleep_range(1000, 1100);
	}

	dev_err(ts->dev, "%s: failed!\n", __func__);

	return -EIO;
}

/**
 * hx83102j_pin_reset() - Reset the touch chip by hardware pin
 * @ts: Himax touch screen data
 *
 * This function is used to hardware reset the touch chip. By pull down the
 * reset pin to low over 20ms, ensure the reset circuit perform a complete reset
 * to the touch chip.
 *
 * Return: None
 */
static void hx83102j_pin_reset(struct himax_ts_data *ts)
{
	gpiod_set_value(ts->pdata.gpiod_rst, 1);
	usleep_range(10000, 10100);
	gpiod_set_value(ts->pdata.gpiod_rst, 0);
	usleep_range(20000, 20100);
}

/**
 * himax_int_enable() - Enable/Disable interrupt
 * @ts: Himax touch screen data
 * @enable: true for enable, false for disable
 *
 * This function is used to enable or disable the interrupt.
 *
 * Return: None
 */
static void himax_int_enable(struct himax_ts_data *ts, bool enable)
{
	int irqnum = ts->himax_irq;
	unsigned long flags;

	spin_lock_irqsave(&ts->irq_lock, flags);
	if (enable && atomic_read(&ts->irq_state) == 0) {
		atomic_set(&ts->irq_state, 1);
		enable_irq(irqnum);
	} else if (!enable && atomic_read(&ts->irq_state) == 1) {
		atomic_set(&ts->irq_state, 0);
		disable_irq_nosync(irqnum);
	}
	spin_unlock_irqrestore(&ts->irq_lock, flags);
	dev_info(ts->dev, "%s: Interrupt %s\n", __func__,
		 atomic_read(&ts->irq_state) ? "enabled" : "disabled");
}

/**
 * himax_mcu_ic_reset() - Reset the touch chip and disable/enable interrupt
 * @ts: Himax touch screen data
 * @int_off: true for disable/enable interrupt, false for not
 *
 * This function is used to reset the touch chip with interrupt control. The
 * TPIC will pull low the interrupt pin when IC is reset. When the ISR has been
 * set and need to be take care of, the caller could set int_off to true to disable
 * the interrupt before reset and enable the interrupt after reset.
 *
 * Return: None
 */
static void himax_mcu_ic_reset(struct himax_ts_data *ts, bool int_off)
{
	if (int_off)
		himax_int_enable(ts, false);

	hx83102j_pin_reset(ts);

	if (int_off)
		himax_int_enable(ts, true);
}

/**
 * hx83102j_reload_to_active() - Reload to active mode
 * @ts: Himax touch screen data
 *
 * This function is used to write a flag to the IC register to make MCU restart without
 * reload the firmware.
 *
 * Return: 0 on success, negative error code on failure
 */
static int hx83102j_reload_to_active(struct himax_ts_data *ts)
{
	int ret;
	u32 retry_cnt;
	const u32 addr = HIMAX_REG_ADDR_RELOAD_TO_ACTIVE;
	const u32 reload_to_active_cmd = 0xec;
	const u32 reload_to_active_done = 0x01ec;
	const u32 retry_limit = 5;
	union himax_dword_data data;

	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		data.dword = cpu_to_le32(reload_to_active_cmd);
		ret = himax_mcu_register_write(ts, addr, data.byte, 4);
		if (ret < 0)
			return ret;
		usleep_range(1000, 1100);
		ret = himax_mcu_register_read(ts, addr, data.byte, 4);
		if (ret < 0)
			return ret;
		data.dword = le32_to_cpu(data.dword);
		if (data.word[0] == reload_to_active_done)
			break;
	}

	if (data.word[0] != reload_to_active_done) {
		dev_err(ts->dev, "%s: Reload to active failed!\n", __func__);
		return -EINVAL;
	}

	return 0;
}

/**
 * hx83102j_en_hw_crc() - Enable/Disable HW CRC
 * @ts: Himax touch screen data
 * @en: true for enable, false for disable
 *
 * This function is used to enable or disable the HW CRC. The HW CRC
 * is used to protect the SRAM data.
 *
 * Return: 0 on success, negative error code on failure
 */
static int hx83102j_en_hw_crc(struct himax_ts_data *ts, bool en)
{
	int ret;
	u32 retry_cnt;
	const u32 addr = HIMAX_HX83102J_REG_ADDR_HW_CRC;
	const u32 retry_limit = 5;
	union himax_dword_data data, wrt_data;

	if (en)
		data.dword = cpu_to_le32(HIMAX_HX83102J_REG_DATA_HW_CRC);
	else
		data.dword = cpu_to_le32(HIMAX_HX83102J_REG_DATA_HW_CRC_DISABLE);

	wrt_data.dword = data.dword;
	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		ret = himax_mcu_register_write(ts, addr, data.byte, 4);
		if (ret < 0)
			return ret;
		usleep_range(1000, 1100);
		ret = himax_mcu_register_read(ts, addr, data.byte, 4);
		if (ret < 0)
			return ret;

		if (data.word[0] == wrt_data.word[0])
			break;
	}

	if (data.word[0] != wrt_data.word[0]) {
		dev_err(ts->dev, "%s: ECC fail!\n", __func__);
		return -EINVAL;
	}

	return 0;
}

/**
 * hx83102j_sense_off() - Stop MCU and enter safe mode
 * @ts: Himax touch screen data
 * @check_en: Check if need to ensure FW is stopped by its owne process
 *
 * Sense off is a process to make sure the MCU inside the touch chip is stopped.
 * The process has two stage, first stage is to request FW to stop. Write
 * HIMAX_REG_DATA_FW_GO_SAFEMODE to HIMAX_REG_ADDR_CTRL_FW tells the FW to stop by its own.
 * Then read back the FW status to confirm the FW is stopped. When check_en is true,
 * the function will resend the stop FW command until the retry limit reached.
 * There maybe a chance that the FW is not stopped by its own, in this case, the
 * safe mode in next stage still stop the MCU, but FW internal flag may not be
 * configured correctly. The second stage is to enter safe mode and reset TCON.
 * Safe mode is a mode that the IC circuit ensure the internal MCU is stopped.
 * Since this IC is TDDI, the TCON need to be reset to make sure the IC is ready
 * for next operation.
 *
 * Return: 0 on success, negative error code on failure
 */
static int hx83102j_sense_off(struct himax_ts_data *ts, bool check_en)
{
	int ret;
	u32 retry_cnt;
	const u32 stop_fw_retry_limit = 35;
	const u32 enter_safe_mode_retry_limit = 5;
	const union himax_dword_data safe_mode = {
		.dword = cpu_to_le32(HIMAX_REG_DATA_FW_GO_SAFEMODE)
	};
	union himax_dword_data data;

	dev_info(ts->dev, "%s: check %s\n", __func__, check_en ? "True" : "False");
	if (!check_en)
		goto without_check;

	for (retry_cnt = 0; retry_cnt < stop_fw_retry_limit; retry_cnt++) {
		if (retry_cnt == 0 ||
		    (data.byte[0] != HIMAX_REG_DATA_FW_GO_SAFEMODE &&
		    data.byte[0] != HIMAX_REG_DATA_FW_RE_INIT &&
		    data.byte[0] != HIMAX_REG_DATA_FW_IN_SAFEMODE)) {
			ret = himax_mcu_register_write(ts, HIMAX_REG_ADDR_CTRL_FW,
						       safe_mode.byte, 4);
			if (ret < 0) {
				dev_err(ts->dev, "%s: stop FW failed\n", __func__);
				return ret;
			}
		}
		usleep_range(10000, 11000);

		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_FW_STATUS, data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read central state failed\n", __func__);
			return ret;
		}
		if (data.byte[0] != HIMAX_REG_DATA_FW_STATE_RUNNING) {
			dev_info(ts->dev, "%s: Do not need wait FW, Status = 0x%02X!\n", __func__,
				 data.byte[0]);
			break;
		}

		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_CTRL_FW, data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read ctrl FW failed\n", __func__);
			return ret;
		}
		if (data.byte[0] == HIMAX_REG_DATA_FW_IN_SAFEMODE)
			break;
	}

	if (data.byte[0] != HIMAX_REG_DATA_FW_IN_SAFEMODE)
		dev_warn(ts->dev, "%s: Failed to stop FW!\n", __func__);

without_check:
	for (retry_cnt = 0; retry_cnt < enter_safe_mode_retry_limit; retry_cnt++) {
		/* set Enter safe mode : 0x31 ==> 0x9527 */
		data.word[0] = cpu_to_le16(HIMAX_HX83102J_SAFE_MODE_PASSWORD);
		ret = himax_write(ts, HIMAX_AHB_ADDR_PSW_LB, NULL, data.byte, 2);
		if (ret < 0) {
			dev_err(ts->dev, "%s: enter safe mode failed\n", __func__);
			return ret;
		}

		/* Check enter_save_mode */
		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_FW_STATUS, data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read central state failed\n", __func__);
			return ret;
		}

		if (data.byte[0] == HIMAX_REG_DATA_FW_STATE_SAFE_MODE) {
			dev_info(ts->dev, "%s: Safe mode entered\n", __func__);
			/* Reset TCON */
			data.dword = cpu_to_le32(HIMAX_REG_DATA_TCON_RST);
			ret = himax_mcu_register_write(ts, HIMAX_HX83102J_REG_ADDR_TCON_RST,
						       data.byte, 4);
			if (ret < 0) {
				dev_err(ts->dev, "%s: reset TCON failed\n", __func__);
				return ret;
			}
			usleep_range(1000, 1100);
			return 0;
		}
		usleep_range(5000, 5100);
		hx83102j_pin_reset(ts);
	}
	dev_err(ts->dev, "%s: failed!\n", __func__);

	return -EIO;
}

/**
 * hx83102j_sense_on() - Sense on the touch chip
 * @ts: Himax touch screen data
 * @sw_reset: true for software reset, false for hardware reset
 *
 * This function is used to sense on the touch chip, which means to start running the
 * FW. The process begin with wakeup the IC bus interface, then write a flag to the IC
 * register to make MCU restart running the FW. When sw_reset is true, the function will
 * send a command to the IC to leave safe mode. Otherwise, the function will call
 * himax_mcu_ic_reset() to reset the touch chip by hardware pin.
 * Then enable the HW CRC to protect sram data, and reload to active to make the MCU
 * start running without reload the firmware.
 *
 * Return: 0 on success, negative error code on failure
 */
static int hx83102j_sense_on(struct himax_ts_data *ts, bool sw_reset)
{
	int ret;
	const union himax_dword_data re_init = {
		.dword = cpu_to_le32(HIMAX_REG_DATA_FW_RE_INIT)
	};
	union himax_dword_data data;

	dev_info(ts->dev, "%s: software reset %s\n", __func__, sw_reset ? "true" : "false");
	ret = himax_mcu_interface_on(ts);
	if (ret < 0)
		return ret;

	ret = himax_mcu_register_write(ts, HIMAX_REG_ADDR_CTRL_FW, re_init.byte, 4);
	if (ret < 0)
		return ret;
	usleep_range(10000, 11000);
	if (!sw_reset) {
		himax_mcu_ic_reset(ts, false);
	} else {
		data.word[0] = cpu_to_le16(HIMAX_AHB_CMD_LEAVE_SAFE_MODE);
		ret = himax_write(ts, HIMAX_AHB_ADDR_PSW_LB, NULL, data.byte, 2);
		if (ret < 0)
			return ret;
	}
	ret = hx83102j_en_hw_crc(ts, true);
	if (ret < 0)
		return ret;
	ret = hx83102j_reload_to_active(ts);
	if (ret < 0)
		return ret;

	return 0;
}

/**
 * hx83102j_chip_detect() - Check if the touch chip is HX83102J
 * @ts: Himax touch screen data
 *
 * This function is used to check if the touch chip is HX83102J. The process
 * start with a hardware reset to the touch chip, then knock the IC bus interface
 * to wakeup the IC bus interface. Then sense off the MCU to prevent bus conflict
 * when reading the IC ID. The IC ID is read from the IC register, and compare
 * with the expected ID. If the ID is matched, the chip is HX83102J. Due to display
 * IC initial code may not ready before the IC ID is read, the function will retry
 * to read the IC ID for several times to make sure the IC ID is read correctly.
 * In any case, the SPI bus shouldn't have error when reading the IC ID, so the
 * function will return error if the SPI bus has error. When the IC is not HX83102J,
 * the function will return -ENODEV.
 *
 * Return: 0 on success, negative error code on failure
 */
static int hx83102j_chip_detect(struct himax_ts_data *ts)
{
	int ret;
	u32 retry_cnt;
	const u32 read_icid_retry_limit = 5;
	const u32 ic_id_mask = GENMASK(31, 8);
	union himax_dword_data data;

	hx83102j_pin_reset(ts);
	ret = himax_mcu_interface_on(ts);
	if (ret)
		return ret;

	ret = hx83102j_sense_off(ts, false);
	if (ret)
		return ret;

	for (retry_cnt = 0; retry_cnt < read_icid_retry_limit; retry_cnt++) {
		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_ICID, data.byte, 4);
		if (ret) {
			dev_err(ts->dev, "%s: Read IC ID Fail\n", __func__);
			return ret;
		}

		data.dword = le32_to_cpu(data.dword);
		if ((data.dword & ic_id_mask) == HIMAX_REG_DATA_ICID) {
			ts->ic_data.bl_size = HIMAX_HX83102J_FLASH_SIZE;
			ts->ic_data.icid = data.dword;
			dev_info(ts->dev, "%s: Detect IC HX83102J successfully\n", __func__);
			return 0;
		}
	}
	dev_err(ts->dev, "%s: Read driver ID register Fail! IC ID = %X,%X,%X\n", __func__,
		data.byte[3], data.byte[2], data.byte[1]);

	return -ENODEV;
}

/**
 * himax_ts_thread() - Thread for interrupt handling
 * @irq: Interrupt number
 * @ptr: Pointer to the touch screen data
 *
 * This function is used to handle the interrupt. The function will call himax_ts_work()
 * to handle the interrupt.
 *
 * Return: IRQ_HANDLED
 */
static irqreturn_t himax_ts_thread(int irq, void *ptr)
{
	himax_ts_work((struct himax_ts_data *)ptr);

	return IRQ_HANDLED;
}

/**
 * __himax_ts_register_interrupt() - Register interrupt trigger
 * @ts: Himax touch screen data
 *
 * This function is used to register the interrupt. The function will call
 * devm_request_threaded_irq() to register the interrupt by the trigger type.
 *
 * Return: 0 on success, negative error code on failure
 */
static int __himax_ts_register_interrupt(struct himax_ts_data *ts)
{
	if (ts->ic_data.interrupt_is_edge)
		return devm_request_threaded_irq(ts->dev, ts->himax_irq, NULL,
						 himax_ts_thread,
						 IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
						 ts->dev->driver->name, ts);

	return devm_request_threaded_irq(ts->dev, ts->himax_irq, NULL,
					 himax_ts_thread,
					 IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					 ts->dev->driver->name, ts);
}

/**
 * himax_ts_register_interrupt() - Register interrupt
 * @ts: Himax touch screen data
 *
 * This function is a wrapper to call __himax_ts_register_interrupt() to register the
 * interrupt and set irq_state.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_ts_register_interrupt(struct himax_ts_data *ts)
{
	int ret;

	if (!ts || !ts->himax_irq) {
		dev_err(ts->dev, "%s: ts or ts->himax_irq invalid!\n", __func__);
		return -EINVAL;
	}

	ret = __himax_ts_register_interrupt(ts);
	if (!ret) {
		atomic_set(&ts->irq_state, 1);
		dev_info(ts->dev, "%s: irq enabled at: %d\n", __func__, ts->himax_irq);
		return 0;
	}

	atomic_set(&ts->irq_state, 0);
	dev_err(ts->dev, "%s: request_irq failed\n", __func__);

	return ret;
}

/**
 * himax_check_power_status() - Check power status
 * @work: Work struct
 *
 * This function is used to check the power status. The function will call
 * power_supply_is_system_supplied() to get the power status, and call
 * himax_cable_detect_func() to update power status to FW.
 *
 * Return: None
 */
static void himax_check_power_status(struct work_struct *work)
{
	struct himax_ts_data *ts = container_of(work, struct himax_ts_data,
						work_pwr.work);

	ts->latest_power_status = power_supply_is_system_supplied();

	dev_info(ts->dev, "Update ts->latest_power_status = %X\n", ts->latest_power_status);

	if (himax_cable_detect_func(ts, true))
		dev_err(ts->dev, "%s: update cable status failed!\n", __func__);
}

/**
 * pwr_notifier_callback() - Power notifier callback
 * @self: Notifier block
 * @event: Event from notifier
 * @data: private data from notifier
 *
 * This function is used to handle the power notifier event. The function will
 * schedule a delayed work to call himax_check_power_status() to check the power
 * status. Due to power notifier often called multiple times, the function will
 * cancel the previous delayed work and schedule a new one to work as a debounce.
 *
 * Return: 0
 */
static int pwr_notifier_callback(struct notifier_block *self, unsigned long event, void *data)
{
	struct himax_ts_data *ts = container_of(self, struct himax_ts_data,
						power_notif);

	cancel_delayed_work_sync(&ts->work_pwr);
	queue_delayed_work(ts->himax_pwr_wq, &ts->work_pwr,
			   msecs_to_jiffies(HIMAX_DELAY_PWR_CHECK_MS));

	return 0;
}

/**
 * himax_pwr_register() - Register power notifier
 * @work: Work struct
 *
 * This function is used to register the power notifier. The function will call
 * power_supply_reg_notifier() to register the power notifier, and schedule a
 * delayed work to call himax_check_power_status() to check the power status.
 *
 * Return: None
 */
static void himax_pwr_register(struct work_struct *work)
{
	int ret;
	struct himax_ts_data *ts = container_of(work, struct himax_ts_data,
						work_pwr.work);

	ts->power_notif.notifier_call = pwr_notifier_callback;
	ret = power_supply_reg_notifier(&ts->power_notif);
	if (ret) {
		dev_err(ts->dev, "%s: Unable to register power_notif: %d\n", __func__, ret);
	} else {
		INIT_DELAYED_WORK(&ts->work_pwr, himax_check_power_status);
		queue_delayed_work(ts->himax_pwr_wq, &ts->work_pwr,
				   msecs_to_jiffies(HIMAX_DELAY_PWR_INIT_CHECK_MS));
	}
}

/**
 * hx83102j_read_event_stack() - Read event stack from touch chip
 * @ts: Himax touch screen data
 * @buf: Buffer to store the data
 * @length: Length of data to read
 *
 * This function is used to read the event stack from the touch chip. The event stack
 * is an AHB output buffer, which store the touch report data.
 *
 * Return: 0 on success, negative error code on failure
 */
static int hx83102j_read_event_stack(struct himax_ts_data *ts, u8 *buf, u32 length)
{
	u32 i;
	int ret;
	const u32 max_trunk_sz = ts->spi_xfer_max_sz - HIMAX_BUS_R_HLEN;

	for (i = 0; i < length; i += max_trunk_sz) {
		ret = himax_read(ts, HIMAX_AHB_ADDR_EVENT_STACK, buf + i,
				 min(length - i, max_trunk_sz));
		if (ret) {
			dev_err(ts->dev, "%s: read event stack error!\n", __func__);
			return ret;
		}
	}

	return 0;
}

/**
 * hx83102j_chip_init_data() - Initialize the touch chip data
 * @ts: Himax touch screen data
 *
 * This function is used to initialize hx83102j touch specific data in himax_ts_data.
 * The chip_max_dsram_size is the maximum size of the DSRAM of hx83102j.
 * The ic_data.enc16bits is the flag to indicate the heatmap data is transferred in
 * 16 bits or 12 bits.
 *
 * Return: None
 */
static void hx83102j_chip_init_data(struct himax_ts_data *ts)
{
	ts->chip_max_dsram_size = HIMAX_HX83102J_DSRAM_SZ;
	ts->ic_data.enc16bits = false;
}

/**
 * himax_touch_get() - Get touch data from touch chip
 * @ts: Himax touch screen data
 * @buf: Buffer to store the data
 *
 * This function is a wrapper to call hx83102j_read_event_stack() to read the touch
 * data from the touch chip. The touch_data_sz is the size of the touch data to read,
 * which is calculated by hid report descriptor provided by the firmware.
 *
 * Return: HIMAX_TS_SUCCESS on success, negative error code on failure. We categorize
 * the error code into HIMAX_TS_GET_DATA_FAIL when the read fails, and HIMAX_TS_SUCCESS
 * when the read is successful. The reason is that the may need special handling when
 * the read fails.
 */
static int himax_touch_get(struct himax_ts_data *ts, u8 *buf)
{
	if (hx83102j_read_event_stack(ts, buf, ts->touch_data_sz)) {
		dev_err(ts->dev, "can't read data from chip!");
		return HIMAX_TS_GET_DATA_FAIL;
	}

	return HIMAX_TS_SUCCESS;
}

/**
 * himax_mcu_assign_sorting_mode() - Write sorting mode to dsram and verify
 * @ts: Himax touch screen data
 * @tmp_data_in: password to write
 *
 * This function is used to write the sorting mode password to dsram and verify the
 * password is written correctly. The sorting mode password is used as a flag to
 * FW to let it know which mode the touch chip is working on.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_assign_sorting_mode(struct himax_ts_data *ts, u8 *tmp_data_in)
{
	int ret;
	u8 rdata[4];
	u32 retry_cnt;
	const u32 retry_limit = 3;

	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_SORTING_MODE_EN,
					       tmp_data_in, HIMAX_REG_SZ);
		if (ret < 0) {
			dev_err(ts->dev, "%s: write sorting mode fail\n", __func__);
			return ret;
		}
		usleep_range(1000, 1100);
		ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_SORTING_MODE_EN,
					      rdata, HIMAX_REG_SZ);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read sorting mode fail\n", __func__);
			return ret;
		}

		if (!memcmp(tmp_data_in, rdata, HIMAX_REG_SZ))
			return 0;
	}
	dev_err(ts->dev, "%s: fail to write sorting mode\n", __func__);

	return -EINVAL;
}

/**
 * himax_mcu_read_FW_status() - Read FW status from touch chip
 * @ts: Himax touch screen data
 *
 * This function is used to read the FW status from the touch chip. The FW status is
 * values from dsram and register from TPIC. Which shows the FW vital working status
 * for developer debug.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_read_FW_status(struct himax_ts_data *ts)
{
	int i;
	int ret;
	size_t len;
	u8 data[4];
	const char * const reg_name[] = {
		"DBG_MSG",
		"FW_STATUS",
		"DD_STATUS",
		"RESET_FLAG"
	};
	const u32 dbg_reg_array[] = {
		HIMAX_DSRAM_ADDR_DBG_MSG,
		HIMAX_REG_ADDR_FW_STATUS,
		HIMAX_REG_ADDR_DD_STATUS,
		HIMAX_REG_ADDR_RESET_FLAG
	};

	len = ARRAY_SIZE(dbg_reg_array);

	for (i = 0; i < len; i++) {
		ret = himax_mcu_register_read(ts, dbg_reg_array[i], data, HIMAX_REG_SZ);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read FW status fail\n", __func__);
			return ret;
		}

		dev_info(ts->dev, "%s: %10s(0x%08X) = 0x%02X, 0x%02X, 0x%02X, 0x%02X\n",
			 __func__, reg_name[i], dbg_reg_array[i],
			 data[0], data[1], data[2], data[3]);
	}

	return 0;
}

/**
 * himax_mcu_power_on_init() - Power on initialization
 * @ts: Himax touch screen data
 *
 * This function is used to do the power on initialization after firmware has been
 * loaded to sram. The process initialize varies IC register and dsram to make sure
 * FW start running correctly. When all set, sense on the touch chip to make the FW
 * start running and wait for the FW reload done password.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_power_on_init(struct himax_ts_data *ts)
{
	int ret;
	u32 retry_cnt;
	const u32 retry_limit = 30;
	union himax_dword_data data;

	/* RawOut select initial */
	data.dword = cpu_to_le32(HIMAX_DATA_CLEAR);
	ret = himax_mcu_register_write(ts, HIMAX_HX83102J_DSRAM_ADDR_RAW_OUT_SEL, data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: set RawOut select fail\n", __func__);
		return ret;
	}
	/* Initial sorting mode password to normal mode */
	ret = himax_mcu_assign_sorting_mode(ts, data.byte);
	if (ret < 0) {
		dev_err(ts->dev, "%s: assign sorting mode fail\n", __func__);
		return ret;
	}
	/* N frame initial */
	/* reset N frame back to default value 1 for normal mode */
	data.dword = cpu_to_le32(1);
	ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_SET_NFRAME, data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: set N frame fail\n", __func__);
		return ret;
	}
	/* Initial FW reload status */
	data.dword = cpu_to_le32(HIMAX_DATA_CLEAR);
	ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_2ND_FLASH_RELOAD, data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: initial FW reload status fail\n", __func__);
		return ret;
	}

	ret = hx83102j_sense_on(ts, false);
	if (ret < 0) {
		dev_err(ts->dev, "%s: sense on fail\n", __func__);
		return ret;
	}

	dev_info(ts->dev, "%s: waiting for FW reload data\n", __func__);
	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_2ND_FLASH_RELOAD, data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read FW reload status fail\n", __func__);
			return ret;
		}

		/* use all 4 bytes to compare */
		if (le32_to_cpu(data.dword) == HIMAX_DSRAM_DATA_FW_RELOAD_DONE) {
			dev_info(ts->dev, "%s: FW reload done\n", __func__);
			break;
		}
		dev_info(ts->dev, "%s: wait FW reload %u times\n", __func__, retry_cnt + 1);
		ret = himax_mcu_read_FW_status(ts);
		if (ret < 0)
			dev_err(ts->dev, "%s: read FW status fail\n", __func__);

		usleep_range(10000, 11000);
	}

	if (retry_cnt == retry_limit) {
		dev_err(ts->dev, "%s: FW reload fail!\n", __func__);
		return -EINVAL;
	}

	return 0;
}

/**
 * himax_mcu_calculate_crc() - Calculate CRC-32 of given data
 * @data: Data to calculate CRC
 * @len: Length of data
 *
 * This function is used to calculate the CRC-32 of the given data. The function
 * calculate the CRC-32 value by the polynomial 0x82f63b78.
 *
 * Return: CRC-32 value
 */
static u32 himax_mcu_calculate_crc(const u8 *data, int len)
{
	int i, j, length;
	u32 crc = GENMASK(31, 0);
	u32 current_data;
	u32 tmp;
	const u32 mask = GENMASK(30, 0);

	length = len / 4;

	for (i = 0; i < length; i++) {
		current_data = data[i * 4];

		for (j = 1; j < 4; j++) {
			tmp = data[i * 4 + j];
			current_data += (tmp) << (8 * j);
		}
		crc = current_data ^ crc;
		for (j = 0; j < 32; j++) {
			if ((crc % 2) != 0)
				crc = ((crc >> 1) & mask) ^ CRC32C_POLY_LE;
			else
				crc = (((crc >> 1) & mask));
		}
	}

	return crc;
}

/**
 * himax_mcu_check_crc() - Let TPIC check CRC itself
 * @ts: Himax touch screen data
 * @start_addr: Start address of the data in sram to check
 * @reload_length: Length of the data to check
 * @crc_result: CRC result for return
 *
 * This function is used to let TPIC check the CRC of the given data in sram. The
 * function write the start address and length of the data to the TPIC, and wait for
 * the TPIC to finish the CRC check. When the CRC check is done, the function read
 * the CRC result from the TPIC.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_check_crc(struct himax_ts_data *ts, u32 start_addr,
			       int reload_length, u32 *crc_result)
{
	int ret;
	int length = reload_length / HIMAX_REG_SZ;
	u32 retry_cnt;
	const u32 retry_limit = 100;
	union himax_dword_data data, addr;

	addr.dword = cpu_to_le32(start_addr);
	ret = himax_mcu_register_write(ts, HIMAX_REG_ADDR_RELOAD_ADDR_FROM, addr.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: write reload start address fail\n", __func__);
		return ret;
	}

	data.word[1] = cpu_to_le16(HIMAX_REG_DATA_RELOAD_PASSWORD);
	data.word[0] = cpu_to_le16(length);
	ret = himax_mcu_register_write(ts, HIMAX_REG_ADDR_RELOAD_ADDR_CMD_BEAT, data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: write reload length and password fail!\n", __func__);
		return ret;
	}

	ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_RELOAD_ADDR_CMD_BEAT, data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read reload length and password fail!\n", __func__);
		return ret;
	}

	if (le16_to_cpu(data.word[0]) != length) {
		dev_err(ts->dev, "%s: length verify failed!\n", __func__);
		return -EINVAL;
	}

	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_RELOAD_STATUS, data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read reload status fail!\n", __func__);
			return ret;
		}

		data.dword = le32_to_cpu(data.dword);
		if ((data.byte[0] & HIMAX_REG_DATA_RELOAD_DONE) != HIMAX_REG_DATA_RELOAD_DONE) {
			ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_RELOAD_CRC32_RESULT,
						      data.byte, HIMAX_REG_SZ);
			if (ret < 0) {
				dev_err(ts->dev, "%s: read crc32 result fail!\n", __func__);
				return ret;
			}
			*crc_result = le32_to_cpu(data.dword);
			return 0;
		}

		dev_info(ts->dev, "%s: Waiting for HW ready!\n", __func__);
		usleep_range(1000, 1100);
	}

	if (retry_cnt == retry_limit) {
		ret = himax_mcu_read_FW_status(ts);
		if (ret < 0)
			dev_err(ts->dev, "%s: read FW status fail\n", __func__);
	}

	return -EINVAL;
}

/**
 * himax_mcu_usb_detect_set() - Update power status to FW
 * @ts: Himax touch screen data
 * @plugged: Power status, 0 for not connected, 1 for connected
 *
 * This function is used to update the power status to the touch chip. The function
 * write the power status to the TPIC, and read back to verify the write is successful.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_usb_detect_set(struct himax_ts_data *ts, bool plugged)
{
	int ret;
	u32 retry_cnt;
	const u32 retry_limit = 5;
	union himax_dword_data wdata, rdata;

	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		if (plugged)
			wdata.dword = cpu_to_le32(HIMAX_DSRAM_DATA_USB_ATTACH);
		else
			wdata.dword = cpu_to_le32(HIMAX_DSRAM_DATA_USB_DETACH);

		ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_USB_DETECT, wdata.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: write USB detect status fail!\n", __func__);
			return ret;
		}

		ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_USB_DETECT, rdata.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read USB detect status fail!\n", __func__);
			return ret;
		}

		if (rdata.dword == wdata.dword)
			break;
	}

	if (retry_cnt == retry_limit) {
		dev_err(ts->dev, "%s: Failed to set USB detect status\n", __func__);
		return -EINVAL;
	}

	return 0;
}

/**
 * himax_mcu_diag_register_set() - Set diag command for hidraw debug
 * @ts: Himax touch screen data
 * @diag_cmd: Diagnose command
 *
 * This function is used to set the diagnose parameter for hidraw ioctl. Which is the
 * same as the raw out select register in TPIC. The ioctl may call in any time, so we
 * call himax_mcu_interface_on() to make sure the TPIC is ready to receive the command.
 * Then write data to TPIC and read back to verify the write is successful.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_diag_register_set(struct himax_ts_data *ts, u8 diag_cmd)
{
	int ret;
	u32 retry_cnt;
	const u32 retry_limit = 50;
	union himax_dword_data tmp_data, back_data;

	tmp_data.dword = cpu_to_le32(diag_cmd);
	ret = himax_mcu_interface_on(ts);
	if (ret < 0)
		return ret;

	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		ret = himax_mcu_register_write(ts, HIMAX_HX83102J_DSRAM_ADDR_RAW_OUT_SEL,
					       tmp_data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: write raw out select fail!\n", __func__);
			return ret;
		}
		ret = himax_mcu_register_read(ts, HIMAX_HX83102J_DSRAM_ADDR_RAW_OUT_SEL,
					      back_data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read raw out select fail!\n", __func__);
			return ret;
		}

		if (tmp_data.byte[0] == back_data.byte[0])
			break;
	}

	if (tmp_data.byte[0] != back_data.byte[0]) {
		dev_err(ts->dev, "%s: Failed to set diagnose register\n", __func__);
		return -EINVAL;
	}

	return 0;
}

/**
 * himax_mcu_diag_register_get() - Get diag command for hidraw debug
 * @ts: Himax touch screen data
 * @val: Diagnose value to return
 *
 * This function is used to get the diagnose parameter for hidraw ioctl.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_diag_register_get(struct himax_ts_data *ts, u32 *val)
{
	int ret;

	ret = himax_mcu_register_read(ts, HIMAX_HX83102J_DSRAM_ADDR_RAW_OUT_SEL, (u8 *)val, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read raw out select fail!\n", __func__);
		return ret;
	}
	*val = le32_to_cpu(*val);

	return 0;
}

/**
 * himax_hid_update_info() - Update hid info
 * @ts: Himax touch screen data
 *
 * This function is used to update the hid info from firmware image and IC data
 * for hidraw ioctl to get the hid info. Which tell user space tool the touch
 * information and how to update the firmware at runtime.The firmware update
 * mapping tells user space tool how to update the firmware, it separates into
 * bl part and main part. The bl part is used to update the bootloader, and runs
 * only once. Which suits the need to update firmware through SPI. So we give
 * bin_start_offset 0, and unit_sz as the size of firmware image in KB.
 *
 * Return: None
 */
static void himax_hid_update_info(struct himax_ts_data *ts)
{
	memcpy(&ts->hid_info.fw_bin_desc, &ts->fw_bin_desc, sizeof(struct himax_bin_desc));
	ts->hid_info.vid = cpu_to_be16(ts->hid_desc.vendor_id);
	ts->hid_info.pid = cpu_to_be16(ts->hid_desc.product_id);
	ts->hid_info.cfg_version = ts->ic_data.vendor_touch_cfg_ver;
	ts->hid_info.disp_version = ts->ic_data.vendor_display_cfg_ver;
	ts->hid_info.rx = ts->ic_data.rx_num;
	ts->hid_info.tx = ts->ic_data.tx_num;
	ts->hid_info.y_res = cpu_to_be16(ts->ic_data.y_res);
	ts->hid_info.x_res = cpu_to_be16(ts->ic_data.x_res);
	ts->hid_info.pt_num = ts->ic_data.max_point;
	ts->hid_info.mkey_num = ts->ic_data.button_num;
	/* firmware table parameters, use only bl part. */
	ts->hid_info.bl_mapping.cmd = HIMAX_HID_FW_UPDATE_BL_CMD;
	ts->hid_info.bl_mapping.bin_start_offset = 0;
	ts->hid_info.bl_mapping.unit_sz = ts->ic_data.bl_size / 1024;
}

/**
 * himax_mcu_read_FW_ver() - Read varies version from touch chip
 * @ts: Himax touch screen data
 *
 * This function is used to read the firmware version, config version, touch config
 * version, display config version, customer ID, customer info, and project info from
 * the touch chip. The function will call himax_mcu_register_read() to read the data
 * from the TPIC, and store the data to the IC data in himax_ts_data.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_read_FW_ver(struct himax_ts_data *ts)
{
	int ret;
	u8 data[HIMAX_TP_INFO_STR_LEN];

	ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_FW_VER, data, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read FW version fail\n", __func__);
		return ret;
	}
	ts->ic_data.vendor_panel_ver =  data[0];
	ts->ic_data.vendor_fw_ver = data[1] << 8 | data[2];
	dev_info(ts->dev, "%s: PANEL_VER: %X\n", __func__, ts->ic_data.vendor_panel_ver);
	dev_info(ts->dev, "%s: FW_VER: %X\n", __func__, ts->ic_data.vendor_fw_ver);

	ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_CFG, data, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read CFG version fail\n", __func__);
		return ret;
	}
	ts->ic_data.vendor_config_ver = data[2] << 8 | data[3];
	ts->ic_data.vendor_touch_cfg_ver = data[2];
	dev_info(ts->dev, "%s: TOUCH_VER: %X\n", __func__, ts->ic_data.vendor_touch_cfg_ver);
	ts->ic_data.vendor_display_cfg_ver = data[3];
	dev_info(ts->dev, "%s: DISPLAY_VER: %X\n", __func__, ts->ic_data.vendor_display_cfg_ver);

	ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_VENDOR, data, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read customer ID fail\n", __func__);
		return ret;
	}
	ts->ic_data.vendor_cid_maj_ver = data[2];
	ts->ic_data.vendor_cid_min_ver = data[3];
	dev_info(ts->dev, "%s: CID_VER: %X\n", __func__, (ts->ic_data.vendor_cid_maj_ver << 8
		 | ts->ic_data.vendor_cid_min_ver));

	ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_CUS_INFO, data, HIMAX_TP_INFO_STR_LEN);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read customer info fail\n", __func__);
		return ret;
	}
	memcpy(ts->ic_data.vendor_cus_info, data, HIMAX_TP_INFO_STR_LEN);
	dev_info(ts->dev, "%s: Cusomer ID : %s\n", __func__, ts->ic_data.vendor_cus_info);

	ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_PROJ_INFO, data, HIMAX_TP_INFO_STR_LEN);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read project info fail\n", __func__);
		return ret;
	}
	memcpy(ts->ic_data.vendor_proj_info, data, HIMAX_TP_INFO_STR_LEN);
	dev_info(ts->dev, "%s: Project ID : %s\n", __func__, ts->ic_data.vendor_proj_info);
	himax_hid_update_info(ts);

	return 0;
}

/**
 * himax_mcu_check_sorting_mode() - Read sorting mode from touch chip
 * @ts: Himax touch screen data
 * @tmp_data_in: Buffer to store the data
 *
 * This function is used to read the sorting mode from the touch chip
 * for hidraw ioctl to get the sorting mode.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_check_sorting_mode(struct himax_ts_data *ts, u8 *tmp_data_in)
{
	int ret;

	ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_SORTING_MODE_EN, tmp_data_in, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read sorting mode fail\n", __func__);
		return ret;
	}

	return 0;
}

/**
 * himax_bin_desc_data_get() - Parse descriptor data from firmware token
 * @ts: Himax touch screen data
 * @addr: Address of the data in firmware image
 * @descript_buf: token for parsing
 * @fw_all_data: Firmware image
 *
 * This function is used to parse the descriptor data from the firmware token. The
 * descriptors are mappings of information in the firmware image. The function will
 * check checksum of each token first, and then parse the token to get the related
 * data. The data includes CID version, FW version, CFG version, touch config table,
 * HID table, HID descriptor, and HID read descriptor.
 *
 * Return: true on success, false on failure
 */
static bool himax_bin_desc_data_get(struct himax_ts_data *ts,
				    u32 addr, u8 *descript_buf,	const u8 *fw_all_data)
{
	u16 chk_end;
	u16 chk_sum;
	u32 hid_table_addr;
	u32 i, j;
	u32 image_offset;
	u32 map_code;
	const u32 data_sz = 16;
	const u32 report_desc_offset = 24;
	union {
		u8 *buf;
		u32 *word;
	} map_data;

	/* looking for mapping in page, each mapping is 16 bytes */
	for (i = 0; i < HIMAX_HX83102J_PAGE_SIZE; i = i + data_sz) {
		chk_end = 0;
		chk_sum = 0;
		for (j = i; j < (i + data_sz); j++) {
			chk_end |= descript_buf[j];
			chk_sum += descript_buf[j];
		}
		if (!chk_end) { /* 1. Check all zero */
			return false;
		} else if (chk_sum % 0x100) { /* 2. Check sum */
			dev_warn(ts->dev, "%s: chk sum failed in %X\n", __func__, i + addr);
		} else { /* 3. get data */
			map_data.buf = &descript_buf[i];
			map_code = le32_to_cpup(map_data.word);
			map_data.buf = &descript_buf[i + 4];
			image_offset = le32_to_cpup(map_data.word);
			/* 4. load info from FW image by specified mapping offset */
			switch (map_code) {
			/* Config ID */
			case HIMAX_FW_CID:
				ts->fw_info_table.addr_cid_ver_major = image_offset;
				ts->fw_info_table.addr_cid_ver_minor = image_offset + 1;
				memcpy(&ts->fw_bin_desc, &fw_all_data
				       [image_offset - sizeof(ts->hid_info.fw_bin_desc.passwd)],
				       sizeof(struct himax_bin_desc));
				break;
			/* FW version */
			case HIMAX_FW_VER:
				ts->fw_info_table.addr_fw_ver_major = image_offset;
				ts->fw_info_table.addr_fw_ver_minor = image_offset + 1;
				break;
			/* Config version */
			case HIMAX_CFG_VER:
				ts->fw_info_table.addr_cfg_ver_major = image_offset;
				ts->fw_info_table.addr_cfg_ver_minor = image_offset + 1;
				break;
			/* Touch config table */
			case HIMAX_TP_CONFIG_TABLE:
				ts->fw_info_table.addr_cfg_table = image_offset;
				break;
			/* HID table */
			case HIMAX_HID_TABLE:
				ts->fw_info_table.addr_hid_table = image_offset;
				hid_table_addr = image_offset;
				ts->fw_info_table.addr_hid_desc = hid_table_addr;
				ts->fw_info_table.addr_hid_rd_desc =
					hid_table_addr + report_desc_offset;
				break;
			}
		}
	}

	return true;
}

/**
 * himax_mcu_bin_desc_get() - Check and get the bin description from the data
 * @fw: Firmware data
 * @ts: Himax touch screen data
 * @max_sz: Maximum size to check
 *
 * This function is used to check and get the bin description from the firmware data.
 * It will check the given data to see if it match the bin description format, and
 * call himax_bin_desc_data_get() to get the related data.
 *
 * Return: true on mapping_count > 0, false on otherwise
 */
static bool himax_mcu_bin_desc_get(unsigned char *fw, struct himax_ts_data *ts, u32 max_sz)
{
	bool keep_on_flag;
	u32 addr;
	u32 mapping_count;
	unsigned char *fw_buf;
	const u8 header_id = 0x87;
	const u8 header_id_loc = 0x0e;
	const u8 header_sz = 8;
	const u8 header[8] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	/* Check bin is with description table or not */
	if (!(memcmp(fw, header, header_sz) == 0 && fw[header_id_loc] == header_id)) {
		dev_err(ts->dev, "%s: No description table\n", __func__);
		return false;
	}

	for (addr = 0, mapping_count = 0; addr < max_sz; addr += HIMAX_HX83102J_PAGE_SIZE) {
		fw_buf = &fw[addr];
		/* Get related data */
		keep_on_flag = himax_bin_desc_data_get(ts, addr, fw_buf, fw);
		if (keep_on_flag)
			mapping_count++;
		else
			break;
	}

	return mapping_count > 0;
}

/**
 * himax_mcu_get_DSRAM_data() - Get DSRAM data from touch chip
 * @ts: Himax touch screen data
 * @info_data: Buffer to store the data
 *
 * This function is used to get the inspection data from DSRAM for hidraw ioctl.
 * The inspection data contains capacitance data in two forms: mutual and self.
 * The mutual data size is (rx_num * tx_num) * 2, and the self data size is
 * (rx_num + tx_num) * 2. The first 4 bytes are the password, used as handshake
 * to request FW put the data to DSRAM. And read back to confirm data is ready.
 * After the data is read, the function will check the checksum of the data to
 * make sure the data is correct. If the checksum is correct, the data will be
 * stored to the info_data buffer. After we got the data, we will tell the FW
 * that data is read, and stop outputing the data.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_get_DSRAM_data(struct himax_ts_data *ts, u8 *info_data)
{
	int ret, stop_ret;
	u8  *temp_info_data;
	const u8 password_mask = GENMASK(7, 0);
	u32 checksum;
	u32 i;
	u32 retry_cnt;
	const u32 handshake_pwd_sz = 4;
	const u32 retry_limit = 5;
	const u32 x_num = ts->ic_data.rx_num;
	const u32 y_num = ts->ic_data.tx_num;
	/* 1. determine total size from rx tx amount */
	u32 total_sz = (x_num * y_num + x_num + y_num) * 2 + handshake_pwd_sz;
	union himax_dword_data data;

	temp_info_data = kcalloc((total_sz + 8), sizeof(u8), GFP_KERNEL);
	if (!temp_info_data)
		return -ENOMEM;

	/* 2. Start handshake and Wait Data Ready */
	data.dword = cpu_to_le32(HIMAX_SRAM_PASSWRD_START);
	ret = himax_write_read_reg(ts, HIMAX_DSRAM_ADDR_RAWDATA,
				   data.byte, HIMAX_SRAM_PASSWRD_END >> 8,
				   HIMAX_SRAM_PASSWRD_END & password_mask);
	if (ret < 0) {
		dev_err(ts->dev, "Data NOT ready => bypass");
		kfree(temp_info_data);
		return ret;
	}

	/* 3. Read RawData */
	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_RAWDATA,
					      temp_info_data, total_sz);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read DSRAM data fail\n", __func__);
			goto error_exit;
		}

		/*
		 * 4. Check Data Checksum
		 * start from location 2 means PASSWORD NOT included
		 */
		checksum = 0;
		for (i = 2; i < total_sz; i += 2)
			checksum += temp_info_data[i + 1] << 8 | temp_info_data[i];

		if (checksum % 0x10000 != 0) {
			dev_err(ts->dev, "%s: check_sum_cal fail=%08X\n", __func__, checksum);
		} else {
			memcpy(info_data, temp_info_data, total_sz * sizeof(u8));
			break;
		}
	}
	if (checksum % 0x10000 != 0) {
		dev_err(ts->dev, "%s: retry_cnt = %u\n", __func__, retry_cnt);
		ret = -EINVAL;
	}

error_exit:
	/* 4. FW stop outputing */
	data.dword = 0;
	data.byte[3] = temp_info_data[3];
	data.byte[2] = temp_info_data[2];
	stop_ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_RAWDATA, data.byte, HIMAX_REG_SZ);
	kfree(temp_info_data);
	if (stop_ret < 0) {
		dev_err(ts->dev, "%s: stop outputing fail\n", __func__);
		return stop_ret;
	}

	return ret;
}

/**
 * himax_mcu_tp_info_check() - Read touch information from touch chip
 * @ts: Himax touch screen data
 *
 * This function is used to read the touch information from the touch chip. The
 * information includes the touch resolution, touch point number, interrupt type,
 * button number, stylus function, stylus version, and stylus ratio. These information
 * is filled by FW after the FW initialized, so it must be called after FW finish
 * loading.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_tp_info_check(struct himax_ts_data *ts)
{
	int ret;
	char data[HIMAX_REG_SZ];
	u8 stylus_ratio;
	u32 button_num;
	u32 max_pt;
	u32 rx_num;
	u32 tx_num;
	u32 x_res;
	u32 y_res;
	const u32 button_num_mask = 0x03;
	const u32 interrupt_type_mask = 0x01;
	const u32 interrupt_type_edge = 0x01;
	bool int_is_edge;
	bool stylus_func;
	bool stylus_id_v2;

	ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_RXNUM_TXNUM, data, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read rx/tx num fail\n", __func__);
		return ret;
	}
	rx_num = data[2];
	tx_num = data[3];

	ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_MAXPT_XYRVS, data, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read max touch point fail\n", __func__);
		return ret;
	}
	max_pt = data[0];

	ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_X_Y_RES, data, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read x/y resolution fail\n", __func__);
		return ret;
	}
	y_res = be16_to_cpup((u16 *)&data[0]);
	x_res = be16_to_cpup((u16 *)&data[2]);

	ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_INT_IS_EDGE, data, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read interrupt type fail\n", __func__);
		return ret;
	}
	if ((data[1] & interrupt_type_mask) == interrupt_type_edge)
		int_is_edge = true;
	else
		int_is_edge = false;

	ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_MKEY, data, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read button number fail\n", __func__);
		return ret;
	}
	button_num = data[0] & button_num_mask;

	ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_STYLUS_FUNCTION, data, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read stylus function fail\n", __func__);
		return ret;
	}
	stylus_func = data[3] ? true : false;

	if (stylus_func) {
		ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_STYLUS_VERSION, data, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read stylus version fail\n", __func__);
			return ret;
		}
		/* dsram_addr_stylus_version + 2 : 0=off 1=on */
		stylus_id_v2 = data[2] ? true : false;
		/* dsram_addr_stylus_version + 3 : 0=ratio_1 10=ratio_10 */
		stylus_ratio = data[3];
	}

	ts->ic_data.button_num = button_num;
	ts->ic_data.stylus_function = stylus_func;
	ts->ic_data.rx_num = rx_num;
	ts->ic_data.tx_num = tx_num;
	ts->ic_data.x_res = x_res;
	ts->ic_data.y_res = y_res;
	ts->ic_data.max_point = max_pt;
	ts->ic_data.interrupt_is_edge = int_is_edge;
	if (stylus_func) {
		ts->ic_data.stylus_v2 = stylus_id_v2;
		ts->ic_data.stylus_ratio = stylus_ratio;
	} else {
		ts->ic_data.stylus_v2 = false;
		ts->ic_data.stylus_ratio = 0;
	}

	dev_info(ts->dev, "%s: rx_num = %u, tx_num = %u\n", __func__,
		 ts->ic_data.rx_num, ts->ic_data.tx_num);
	dev_info(ts->dev, "%s: max_point = %u\n", __func__, ts->ic_data.max_point);
	dev_info(ts->dev, "%s: interrupt_is_edge = %s, stylus_function = %s\n", __func__,
		 ts->ic_data.interrupt_is_edge ? "true" : "false",
		 ts->ic_data.stylus_function ? "true" : "false");
	dev_info(ts->dev, "%s: stylus_v2 = %s, stylus_ratio = %u\n", __func__,
		 ts->ic_data.stylus_v2 ? "true" : "false", ts->ic_data.stylus_ratio);
	dev_info(ts->dev, "%s: TOUCH INFO updated\n", __func__);

	return 0;
}

/**
 * himax_mcu_resned_cmd_func() - Resend command collection
 * @ts: Himax touch screen data
 *
 * This function is used to collect commands that need to be resent to TPIC after
 * firmware restore. Usually we put system configuration status FW need to know
 * after FW restore from system resume, firmware update or esd recovery. Currently,
 * we put cable status here.
 *
 * return: 0 on success, negative error code on failure
 */
static int himax_mcu_resend_cmd_func(struct himax_ts_data *ts)
{
	return himax_cable_detect_func(ts, true);
}

/**
 * himax_disable_fw_reload() - Disable the FW reload data from flash
 * @ts: Himax touch screen data
 *
 * This function is used to tell FW not to reload data from flash. It needs to be
 * set before FW start running.
 *
 * return: 0 on success, negative error code on failure
 */
static int himax_disable_fw_reload(struct himax_ts_data *ts)
{
	union himax_dword_data data = {
		/*
		 * HIMAX_DSRAM_ADDR_FLASH_RELOAD: 0x10007f00
		 * 0x10007f00 <= 0x9aa9, let FW know there's no flash
		 *	      <= 0x5aa5, there has flash, but not reload
		 *	      <= 0x0000, there has flash, and reload
		 */
		.dword = cpu_to_le32(HIMAX_DSRAM_DATA_DISABLE_FLASH_RELOAD)
	};

	/* Disable Flash Reload */
	return himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_FLASH_RELOAD, data.byte, 4);
}

/**
 * himax_sram_write_crc_check() - Write the data to SRAM and check the CRC by hardware
 * @ts: Himax touch screen data
 * @addr: Address to write to
 * @data: Data to write
 * @len: Length of data
 *
 * This function is use to write FW code/data to SRAM and check the CRC by hardware to make
 * sure the written data is correct. The FW code is designed to be CRC result 0, so if the
 * CRC result is not 0, it means the written data is not correct.
 *
 * return: 0 on success, negative error code on failure
 */
static int himax_sram_write_crc_check(struct himax_ts_data *ts, u32 addr, const u8 *data, u32 len)
{
	int ret;
	u32 crc;
	u32 retry_cnt;
	const u32 retry_limit = 3;

	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		dev_info(ts->dev, "%s: Write FW to SRAM - total write size = %u\n", __func__, len);
		ret = himax_mcu_register_write(ts, addr, data, len);
		if (ret) {
			dev_err(ts->dev, "%s: write FW to SRAM fail\n", __func__);
			return ret;
		}
		ret = himax_mcu_check_crc(ts, addr, len, &crc);
		if (ret) {
			dev_err(ts->dev, "%s: check CRC fail\n", __func__);
			return ret;
		}
		dev_info(ts->dev, "%s: HW CRC %s in %u time\n", __func__,
			 crc == 0 ? "OK" : "Fail", retry_cnt);

		if (crc == 0)
			break;
	}

	if (crc != 0) {
		dev_err(ts->dev, "%s: HW CRC fail\n", __func__);
		return -EINVAL;
	}

	return 0;
}

/**
 * himax_zf_part_info() - Get and write the partition from the firmware to SRAM
 * @fw: Firmware data
 * @ts: Himax touch screen data
 *
 * This function is used to get the partition information from the firmware and write
 * the partition to SRAM. The partition information includes the DSRAM address, the
 * firmware offset, and the write size. The function will get the partition information
 * into a table, and then write the partition to SRAM according to the table. After
 * writing the partition to SRAM, the function will check the CRC by hardware to make
 * sure the written data is correct.
 *
 * return: 0 on success, negative error code on failure
 */
static int himax_zf_part_info(const struct firmware *fw, struct himax_ts_data *ts)
{
	int i;
	int i_max = -1;
	int i_min = -1;
	int pnum;
	int ret;
	u8 buf[HIMAX_ZF_PARTITION_DESC_SZ];
	u32 cfg_crc_sw;
	u32 cfg_crc_hw;
	u32 cfg_sz;
	u32 dsram_base = 0xffffffff;
	u32 dsram_max = 0;
	u32 retry_cnt = 0;
	u32 sram_min;
	const u32 retry_limit = 3;
	const u32 table_addr = ts->fw_info_table.addr_cfg_table;
	struct himax_zf_info *info;

	/* 1. initial check */
	ret = hx83102j_en_hw_crc(ts, true);
	if (ret < 0) {
		dev_err(ts->dev, "%s: Failed to enable HW CRC\n", __func__);
		return ret;
	}
	pnum = fw->data[table_addr + HIMAX_ZF_PARTITION_AMOUNT_OFFSET];
	if (pnum < 2) {
		dev_err(ts->dev, "%s: partition number is not correct\n", __func__);
		return -EINVAL;
	}

	info = kcalloc(pnum, sizeof(struct himax_zf_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	memset(info, 0, pnum * sizeof(struct himax_zf_info));

	/*
	 * 2. record partition information:
	 * partition 0: FW main code
	 */
	memcpy(buf, &fw->data[table_addr], HIMAX_ZF_PARTITION_DESC_SZ);
	memcpy(info[0].sram_addr, buf, 4);
	info[0].write_size = le32_to_cpup((u32 *)&buf[4]);
	info[0].fw_addr = le32_to_cpup((u32 *)&buf[8]);

	/* partition 1 ~ n: config data */
	for (i = 1; i < pnum; i++) {
		memcpy(buf, &fw->data[i * HIMAX_ZF_PARTITION_DESC_SZ + table_addr],
		       HIMAX_ZF_PARTITION_DESC_SZ);
		memcpy(info[i].sram_addr, buf, 4);
		info[i].write_size = le32_to_cpup((u32 *)&buf[4]);
		info[i].fw_addr = le32_to_cpup((u32 *)&buf[8]);
		info[i].cfg_addr = le32_to_cpup((u32 *)&info[i].sram_addr[0]);

		/* Write address must be multiple of 4 */
		if (info[i].cfg_addr % 4 != 0) {
			info[i].cfg_addr -= (info[i].cfg_addr % 4);
			info[i].fw_addr -= (info[i].cfg_addr % 4);
			info[i].write_size += (info[i].cfg_addr % 4);
		}

		if (dsram_base > info[i].cfg_addr) {
			dsram_base = info[i].cfg_addr;
			i_min = i;
		}
		if (dsram_max < info[i].cfg_addr) {
			dsram_max = info[i].cfg_addr;
			i_max = i;
		}
	}

	if (i_min < 0 || i_max < 0) {
		dev_err(ts->dev, "%s: DSRAM address invalid!\n", __func__);
		return -EINVAL;
	}

	/* 3. prepare data to update */
	sram_min = info[i_min].cfg_addr;

	cfg_sz = (dsram_max - dsram_base) + info[i_max].write_size;
	/* Wrtie size must be multiple of 4 */
	if (cfg_sz % 4 != 0)
		cfg_sz = cfg_sz + 4 - (cfg_sz % 4);

	dev_info(ts->dev, "%s: main code sz = %d, config sz = %d\n", __func__,
		 info[0].write_size, cfg_sz);
	/* config size should be smaller than DSRAM size */
	if (cfg_sz > ts->chip_max_dsram_size) {
		dev_err(ts->dev, "%s: config size error[%d, %u]!!\n", __func__,
			cfg_sz, ts->chip_max_dsram_size);
		ret = -EINVAL;
		goto alloc_cfg_buffer_failed;
	}

	memset(ts->zf_update_cfg_buffer, 0x00,
	       ts->chip_max_dsram_size * sizeof(u8));

	/* Collect all partition in FW for DSRAM in a cfg buffer */
	for (i = 1; i < pnum; i++)
		memcpy(&ts->zf_update_cfg_buffer[info[i].cfg_addr - dsram_base],
		       &fw->data[info[i].fw_addr], info[i].write_size);

	/*
	 * 4. write to sram
	 * First, write FW main code and check CRC by HW
	 */
	ret = himax_sram_write_crc_check(ts, le32_to_cpup((u32 *)info[0].sram_addr),
					 &fw->data[info[0].fw_addr], info[0].write_size);
	if (ret < 0) {
		dev_err(ts->dev, "%s: HW CRC fail\n", __func__);
		goto write_main_code_failed;
	}

	/*
	 * Second, FW config data: Calculate CRC of CFG data which is going to write.
	 * CFG data don't have CRC pre-defined in FW and need to be calculated by driver.
	 */
	cfg_crc_sw = himax_mcu_calculate_crc(ts->zf_update_cfg_buffer, cfg_sz);
	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		/* Write hole cfg data to DSRAM */
		dev_info(ts->dev, "%s: Write cfg to SRAM - total write size = %d\n",
			 __func__, cfg_sz);
		ret = himax_mcu_register_write(ts, sram_min, ts->zf_update_cfg_buffer, cfg_sz);
		if (ret < 0) {
			dev_err(ts->dev, "%s: write cfg to SRAM fail\n", __func__);
			goto write_cfg_failed;
		}
		/*
		 * Check CRC: Tell HW to calculate CRC from CFG start address in SRAM and check
		 * size is equal to size of CFG buffer written. Then we compare the two CRC data
		 * make sure data written is correct.
		 */
		ret = himax_mcu_check_crc(ts, sram_min, cfg_sz, &cfg_crc_hw);
		if (ret) {
			dev_err(ts->dev, "%s: check CRC failed!\n", __func__);
			goto crc_failed;
		}

		if (cfg_crc_hw != cfg_crc_sw)
			dev_err(ts->dev, "%s: Cfg CRC FAIL, HWCRC = %X, SWCRC = %X, retry = %u\n",
				__func__, cfg_crc_hw, cfg_crc_sw, retry_cnt);
		else
			break;
	}

	if (retry_cnt == retry_limit && cfg_crc_hw != cfg_crc_sw) {
		dev_err(ts->dev, "%s: Write cfg to SRAM fail\n", __func__);
		ret = -EINVAL;
		goto crc_not_match;
	}

	/* write back system config */
	if (himax_mcu_resend_cmd_func(ts))
		dev_warn(ts->dev, "%s: failed to resend config!\n", __func__);

crc_not_match:
crc_failed:
write_cfg_failed:
write_main_code_failed:
alloc_cfg_buffer_failed:
	kfree(info);

	return ret;
}

/**
 * himax_mcu_firmware_update_zf() - Update the firmware to the touch chip
 * @fw: Firmware data
 * @ts: Himax touch screen data
 *
 * This function is used to update the firmware to the touch chip. The first step is
 * to reset the touch chip, stop the MCU and then write the firmware to the touch chip.
 *
 * return: 0 on success, negative error code on failure
 */
static int himax_mcu_firmware_update_zf(const struct firmware *fw, struct himax_ts_data *ts)
{
	int ret;
	union himax_dword_data data_system_reset = {
		.dword = cpu_to_le32(HIMAX_REG_DATA_SYSTEM_RESET)
	};

	dev_info(ts->dev, "%s: Updating FW - total FW size = %u\n", __func__, (u32)fw->size);
	ret = himax_mcu_register_write(ts, HIMAX_REG_ADDR_SYSTEM_RESET, data_system_reset.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: system reset fail\n", __func__);
		return ret;
	}

	ret = hx83102j_sense_off(ts, false);
	if (ret)
		return ret;

	ret = himax_zf_part_info(fw, ts);

	return ret;
}

/**
 * himax_zf_reload_from_file() - Complete firmware update sequence
 * @file_name: File name of the firmware
 * @ts: Himax touch screen data
 *
 * This function process the full sequence of updating the firmware to the touch chip.
 * It will first check if the other thread is updating now, if not, it will request the
 * firmware from user space and then call himax_mcu_firmware_update_zf() to update the
 * firmware, and then tell firmware not to reload data from flash and initial the touch
 * chip by calling himax_mcu_power_on_init().
 *
 * return: 0 on success, negative error code on failure
 */
static int himax_zf_reload_from_file(char *file_name, struct himax_ts_data *ts)
{
	int ret;
	const struct firmware *fw;

	if (!mutex_trylock(&ts->zf_update_lock)) {
		dev_warn(ts->dev, "%s: Other thread is updating now!\n", __func__);
		return 0;
	}
	dev_info(ts->dev, "%s: Preparing to update %s!\n", __func__, file_name);

	ret = request_firmware(&fw, file_name, ts->dev);
	if (ret < 0) {
		dev_err(ts->dev, "%s: request firmware fail, code[%d]!!\n", __func__, ret);
		goto load_firmware_error;
	}

	ret = himax_mcu_firmware_update_zf(fw, ts);
	release_firmware(fw);
	if (ret < 0)
		goto load_firmware_error;

	ret = himax_disable_fw_reload(ts);
	if (ret < 0)
		goto load_firmware_error;
	ret = himax_mcu_power_on_init(ts);

load_firmware_error:
	mutex_unlock(&ts->zf_update_lock);

	return ret;
}

/**
 * himax_input_check() - Check the interrupt data
 * @ts: Himax touch screen data
 * @buf: Buffer of interrupt data
 *
 * This function is used to check the interrupt data. The function will check
 * the interrupt data to see if it is normal or abnormal. If the interrupt data
 * is all the same, it will return HIMAX_TS_ABNORMAL_PATTERN, otherwise, it will
 * return HIMAX_TS_REPORT_DATA.
 *
 * Return: HIMAX_TS_ABNORMAL_PATTERN when all data is the same, HIMAX_TS_REPORT_DATA
 * when data is normal.
 */
static int himax_input_check(struct himax_ts_data *ts, u8 *buf)
{
	int i;
	int length;
	int same_cnt = 1;

	/* Check all interrupt data */
	length = ts->touch_data_sz;
	if (!length)
		return HIMAX_TS_REPORT_DATA;

	for (i = 1; i < length; i++) {
		if (buf[i] == buf[i - 1])
			same_cnt++;
		else
			same_cnt = 1;
	}

	if (same_cnt == length) {
		dev_warn(ts->dev, "%s: [HIMAX TP MSG] Detected abnormal input pattern\n", __func__);
		return HIMAX_TS_ABNORMAL_PATTERN;
	}

	return HIMAX_TS_REPORT_DATA;
}

/**
 * himax_hid_parse() - Parse the HID report descriptor
 * @hid: HID device
 *
 * This function is used to parse the HID report descriptor.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_hid_parse(struct hid_device *hid)
{
	int ret;
	struct himax_ts_data *ts;

	if (!hid)
		return -ENODEV;

	ts = hid->driver_data;
	if (!ts)
		return -EINVAL;

	ret = hid_parse_report(hid, ts->hid_rd_data.rd_data, ts->hid_rd_data.rd_length);
	if (ret) {
		dev_err(ts->dev, "%s: failed parse report\n", __func__);
		return ret;
	}

	return 0;
}

/**
 * himax_hid_start - Start the HID device
 * @hid: HID device
 *
 * The function for hid_ll_driver.start to start the HID device.
 * This driver does not need to do anything here.
 *
 * Return: 0 for success
 */
static int himax_hid_start(struct hid_device *hid)
{
	return 0;
}

/**
 * himax_hid_stop - Stop the HID device
 * @hid: HID device
 *
 * The function for hid_ll_driver.stop to stop the HID device.
 * This driver does not need to do anything here.
 *
 * Return: None
 */
static void himax_hid_stop(struct hid_device *hid)
{
}

/**
 * himax_hid_open - Open the HID device
 * @hid: HID device
 *
 * The function for hid_ll_driver.open to open the HID device.
 * This driver does not need to do anything here.
 *
 * Return: 0 for success
 */
static int himax_hid_open(struct hid_device *hid)
{
	return 0;
}

/**
 * himax_hid_close - Close the HID device
 * @hid: HID device
 *
 * The function for hid_ll_driver.close to close the HID device.
 * This driver does not need to do anything here.
 *
 * Return: None
 */
static void himax_hid_close(struct hid_device *hid)
{
}

/**
 * free_firmware - Free the firmware data for hidraw run-time update
 * @ts: Himax touch screen data
 * @fw: Firmware data
 *
 * The function is used to free the firmware data for hidraw run-time update.
 *
 * Return: None
 */
static void free_firmware(struct himax_ts_data *ts, struct firmware *fw)
{
	if (fw) {
		devm_kfree(ts->dev, fw->data);
		devm_kfree(ts->dev, fw->priv);
		devm_kfree(ts->dev, fw);
	}
}

/**
 * himax_hid_load_user_firmware() - Load the firmware data for hidraw run-time update
 * @ts: Himax touch screen data
 * @fwdata: Firmware data, usually partial due to ioctl operation
 * @sz: Size of the firmware data
 *
 * The function is used to load the firmware data for hidraw run-time update. The FW data
 * is loaded to the hid_req_cfg.fw->data buffer. The function will check if the FW data
 * is complete, if it is complete, it will return HIMAX_LOAD_FIRMWARE_DONE, otherwise, it
 * will return HIMAX_LOAD_FIRMWARE_ONGOING. Due to IOCTL operation, the FW data is
 * usually part of the FW image and need to be combined to a complete FW image.
 *
 * Return: HIMAX_LOAD_FIRMWARE_DONE when the FW data is complete, HIMAX_LOAD_FIRMWARE_ONGOING
 * when the FW data is not complete
 */
static int himax_hid_load_user_firmware(struct himax_ts_data *ts, u8 *fwdata, size_t sz)
{
	if (ts->hid_req_cfg.fw) {
		/*
		 * if size is equal to complete FW image size, means new FW is be uploading
		 * then free the old FW. FW image size is equal to the size of the flash.
		 */
		if (ts->hid_req_cfg.fw->size == ts->ic_data.bl_size) {
			dev_info(ts->dev, "%s: free old fw\n", __func__);
			free_firmware(ts, ts->hid_req_cfg.fw);
			ts->hid_req_cfg.fw = NULL;
		}
	}

	/* if no FW data, allocate new firmware structure and FW image data space */
	if (!ts->hid_req_cfg.fw) {
		ts->hid_req_cfg.fw = devm_kzalloc(ts->dev, sizeof(*ts->hid_req_cfg.fw), GFP_KERNEL);
		if (!ts->hid_req_cfg.fw)
			return -ENOMEM;

		ts->hid_req_cfg.fw->data = devm_kzalloc(ts->dev, ts->ic_data.bl_size, GFP_KERNEL);
		if (!ts->hid_req_cfg.fw->data) {
			devm_kfree(ts->dev, ts->hid_req_cfg.fw);
			ts->hid_req_cfg.fw = NULL;
			return -ENOMEM;
		}
	}

	/* copy the FW data to the FW image buffer, exclude the ID byte */
	memcpy((u8 *)ts->hid_req_cfg.fw->data + ts->hid_req_cfg.fw->size,
	       (u8 *)fwdata + 1, sz - 1);
	ts->hid_req_cfg.fw->size += sz - 1;
	/* When accumlate size is equal to FW image size, means transfer is completed */
	if (ts->hid_req_cfg.fw->size == ts->ic_data.bl_size) {
		dev_info(ts->dev, "%s: load firmware done\n", __func__);
		return HIMAX_LOAD_FIRMWARE_DONE;
	}

	return HIMAX_LOAD_FIRMWARE_ONGOING;
}

/**
 * himax_usi_write_cmd() - Write USI command to IC sram
 * @ts: Himax touch screen data
 * @cmd: USI command from user-space through HIDRAW
 *
 * This function write USI command from user-space to IC SRAM. It check the corresponding
 * address data first, if they equals all zero in size of himax_usi_cmd. It means FW is ready
 * to receive next USI command, FW will clean this address when it processed the command.
 * Then we just write the command to this address for FW to process.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_usi_write_cmd(struct himax_ts_data *ts, struct himax_usi_cmd *cmd)
{
	int ret;
	u32 retry_cnt;
	const u32 retry_limit = 3;
	struct himax_usi_cmd tmp;
	const struct himax_usi_cmd ready_to_write = { 0 };

	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_STYLUS_CMD, (u8 *)&tmp,
					      sizeof(struct himax_usi_cmd));
		if (ret < 0) {
			dev_err(ts->dev, "%s: read USI cmd fail\n", __func__);
			return ret;
		}

		if (memcmp(&tmp, &ready_to_write, sizeof(struct himax_usi_cmd)) != 0)
			usleep_range(1000, 2000);
		else
			break;
	}

	if (retry_cnt == retry_limit) {
		dev_err(ts->dev, "%s: FW is not ready to receive USI command\n", __func__);
		return -EBUSY;
	}

	ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_STYLUS_CMD, (u8 *)cmd,
				       sizeof(struct himax_usi_cmd));
	if (ret < 0) {
		dev_err(ts->dev, "%s: write USI cmd fail\n", __func__);
		return ret;
	}

	return 0;
}

/**
 * himax_hid_get_raw_report - Process hidraw GET REPORT operation
 * @hid: HID device
 * @reportnum: Report ID
 * @buf: Buffer for communication
 * @len: Length of data in the buffer
 * @report_type: Report type
 *
 * The function for hid_ll_driver.get_raw_report to handle the HIDRAW ioctl
 * get report request. The report number to handle is based on the report
 * descriptor of the HID device. The buf is used to communicate with user
 * program, user pass the ID and parameters to the driver use this buf, and
 * the driver will return the result to user also use this buf. The len is
 * the length of data in the buf, passed by user program. The report_type is
 * not used in this driver. We currently support the following report number:
 * - HIMAX_ID_CONTACT_COUNT: Report the maximum number of touch points
 * - HIMAX_ID_CFG: Report the configuration of the HID device
 * - HIMAX_ID_FW_UPDATE_HANDSHAKING: Report the handshake status of the FW update
 * - HIMAX_ID_SELF_TEST: Report the self test status
 * - HIMAX_ID_TOUCH_MONITOR: Report the touch data
 * - HIMAX_ID_TOUCH_MONITOR_SEL: Report current touch data type
 * - HIMAX_ID_REG_RW: Report the register read data
 * - HIMAX_ID_INPUT_RD_DE: Report current report descriptor disable state
 * - HIMAX_ID_FW_UPDATE: Dummy report return ok only
 * - HIMAX_ID_USI_COLOR: Report the stylus color data
 * - HIMAX_ID_USI_WIDTH: Report the stylus width data
 * - HIMAX_ID_USI_STYLE: Report the stylus style data
 * - HIMAX_ID_USI_BUTTONS: Report the stylus buttons data {barrel, side, eraser}
 * - HIMAX_ID_USI_FIRMWARE: Report the stylus firmware version
 * - HIMAX_ID_USI_PROTOCOL: Report the stylus protocol version
 * - HIMAX_ID_USI_TRANSDUCER: Report 0, write only attribute
 * - HIMAX_ID_WINDOWS_BLOB_VALID: Report the windows blob data
 * Case not listed here will return -EINVAL.
 *
 * Return: The length of the data in the buf on success, negative error code
 */
static int himax_hid_get_raw_report(const struct hid_device *hid,
				    unsigned char reportnum, __u8 *buf,
				    size_t len, unsigned char report_type)
{
	int ret;
	const u8 data_mask = GENMASK(7, 0);
	u32 tmp_data;
	struct himax_ts_data *ts;
	struct himax_usi_info usi_info;
	union himax_dword_data *tmp;

	ts = hid->driver_data;
	if (!ts) {
		dev_err(ts->dev, "hid->driver_data is NULL");
		return -EINVAL;
	}

	switch (reportnum) {
	case HIMAX_ID_CONTACT_COUNT:
		/* buf[0] is ID; buf[1] and later used as parameters for ID */
		buf[0] = HIMAX_ID_CONTACT_COUNT;
		buf[1] = ts->ic_data.max_point;
		ret = len;
		break;
	case HIMAX_ID_CFG:
		buf[0] = HIMAX_ID_CFG;
		memcpy(buf + HIMAX_HID_ID_SZ, &ts->hid_info, sizeof(struct himax_hid_info));
		ret = len;
		break;
	/*
	 * User check FW update status, the correct user update flow is:
	 * 1. set parameter to HIMAX_HID_FW_UPDATE_MAIN_CMD(0x55), send by
	 *    HIMAX_ID_FW_UPDATE_HANDSHAKING
	 * 2. get HIMAX_ID_FW_UPDATE_HANDSHAKING see if status is ready to send FW
	 * 3. ready, send FW content by HIMAX_ID_FW_UPDATE
	 * 4. send complete, get HIMAX_ID_FW_UPDATE_HANDSHAKING see if status OK
	 * 5. by g_dummy_main_code, send 2 parts of main code which we don't use
	 * 6. tool start sending BL part when status is HIMAX_FWUP_BL_READY
	 * 7. FW image receive by himax_hid_load_user_firmware() and update
	 * 8. report hid_info.bl_mapping.cmd which is HIMAX_HID_FW_UPDATE_BL_CMD if tool check.
	 *    Which means FW transfer complete.
	 */
	case HIMAX_ID_FW_UPDATE_HANDSHAKING:
		/* Already in handshake mode */
		if (ts->hid_req_cfg.processing_id == HIMAX_ID_FW_UPDATE_HANDSHAKING) {
			/* Last set command is Bootloader update */
			if (ts->hid_req_cfg.handshake_set == ts->hid_info.bl_mapping.cmd) {
				ts->hid_req_cfg.handshake_get = ts->hid_info.bl_mapping.cmd;
			/* Last set command is main code start */
			} else if (ts->hid_req_cfg.handshake_set == HIMAX_HID_FW_UPDATE_MAIN_CMD) {
				/*
				 * Just report part 0 is ready for update, rest data xferred
				 * size to 0
				 */
				ts->hid_req_cfg.handshake_get = g_dummy_main_code[0].cmd;
				ts->hid_req_cfg.current_size = 0;
			/* Last set command is main code part 0 start */
			} else if (ts->hid_req_cfg.handshake_set == g_dummy_main_code[0].cmd) {
				/* when part 0 data transfer complete */
				if (ts->hid_req_cfg.current_size >= g_dummy_main_code[0].unit_sz) {
					/*
					 * Just report part 1 is ready for update, reset xferred
					 * data size to 0
					 */
					ts->hid_req_cfg.handshake_get = g_dummy_main_code[1].cmd;
					ts->hid_req_cfg.current_size = 0;
				}
			/* Last set command is main code part 1 start */
			} else if (ts->hid_req_cfg.handshake_set == g_dummy_main_code[1].cmd) {
				/* Part 1 xferred completed, request BL part */
				if (ts->hid_req_cfg.current_size >= g_dummy_main_code[1].unit_sz) {
					ts->hid_req_cfg.handshake_get = HIMAX_FWUP_BL_READY;
					ts->hid_req_cfg.current_size = 0;
				}
			/* Else just report no error */
			} else {
				ts->hid_req_cfg.handshake_get = HIMAX_FWUP_NO_ERROR;
			}
			buf[0] = HIMAX_ID_FW_UPDATE_HANDSHAKING;
			buf[1] = ts->hid_req_cfg.handshake_get;
		/* Last command is FW sending, report last get update status */
		} else if (ts->hid_req_cfg.processing_id == HIMAX_ID_FW_UPDATE) {
			/* Need to lock due to progress check can have only one */
			mutex_lock(&ts->hid_ioctl_lock);
			buf[0] = HIMAX_ID_FW_UPDATE_HANDSHAKING;
			buf[1] = ts->hid_req_cfg.handshake_get;
			mutex_unlock(&ts->hid_ioctl_lock);
		/* Shouldn't be here if using right tool, just return ok */
		} else {
			buf[0] = HIMAX_ID_FW_UPDATE_HANDSHAKING;
			buf[1] = HIMAX_FWUP_NO_ERROR;
		}
		ret = len;
		break;
	/* Self test status check, return the result left in himax_self_test() */
	case HIMAX_ID_SELF_TEST:
		mutex_lock(&ts->hid_ioctl_lock);
		buf[0] = HIMAX_ID_SELF_TEST;
		buf[1] = ts->hid_req_cfg.handshake_get;
		/* turn on interrupt, in case tool fail to set HIMAX_INSPECT_BACK_NORMAL */
		himax_int_enable(ts, true);
		mutex_unlock(&ts->hid_ioctl_lock);
		ret = len;
		break;
	case HIMAX_ID_TOUCH_MONITOR:
		ret = himax_get_data(ts, &buf[2]);
		if (ret == HIMAX_INSPECT_OK) {
			/* get data succeed */
			buf[0] = HIMAX_ID_TOUCH_MONITOR;
			buf[1] = 0;
			ret = len;
		} else {
			ret = 0;
		}
		break;
	case HIMAX_ID_TOUCH_MONITOR_SEL:
		ret = himax_mcu_diag_register_get(ts, &tmp_data);
		if (!ret) {
			/* return read back value of diag register */
			buf[0] = HIMAX_ID_TOUCH_MONITOR_SEL;
			buf[1] = tmp_data & data_mask;
			ret = len;
		} else {
			ret = 0;
		}
		break;
	case HIMAX_ID_REG_RW:
		/*
		 * standard REG RW, Address 4 bytes, Data 4 bytes all fixed
		 * standard REG RW len = 10 : ID(1) | R/W(1) | ADDR(4) | DATA(4)
		 */
		if (len == 10 &&
		    le32_to_cpup((u32 *)&buf[2]) != HIMAX_REG_TYPE_EXT_TYPE) {
			/* standard REG RW */
			ts->hid_req_cfg.reg_addr_sz = 4;
			ts->hid_req_cfg.reg_data_sz = 4;
			ts->hid_req_cfg.reg_addr.dword =
				le32_to_cpup((u32 *)&buf[2]);
			ret = himax_mcu_register_read(ts, ts->hid_req_cfg.reg_addr.dword,
						      ts->hid_req_cfg.reg_data,
						      ts->hid_req_cfg.reg_data_sz);
			if (!ret) {
				tmp = (union himax_dword_data *)ts->hid_req_cfg.reg_data;
				tmp->dword = le32_to_cpu(tmp->dword);
				buf[0] = HIMAX_ID_REG_RW;
				/* Reg data at buf[6] */
				memcpy(&buf[6], ts->hid_req_cfg.reg_data,
				       ts->hid_req_cfg.reg_data_sz);
				ret = len;
			} else {
				ret = 0;
			}
		/*
		 * EXT type REG RW, Address 1 for AHB, 4 for SRAM/REG, Data 1~256
		 * EXT REG RW len >= 9 : ID(1) | R/W(1) | EXT_HDR(4) | EXT_TYPE(1) | REG_ADDR(1/4)
		 *			 | REG_DATA(1~256) => min 9, max 267
		 */
		} else if ((len >= 9) && (len <= (1 + HIMAX_HID_REG_SZ_MAX)) &&
			(((union himax_dword_data *)&buf[2])->dword == HIMAX_REG_TYPE_EXT_TYPE)) {
			switch (buf[6]) {
			/* AHB type REG RW, Address 1, Data 1~256 */
			case HIMAX_REG_TYPE_EXT_AHB:
				ts->hid_req_cfg.reg_addr_sz = 1;
				/*
				 * Data length = total length from user space tool - ID(1) - R/W(1)
				 *		 - EXT_HDR(4) - EXT_TYPE(1) - REG_ADDR(1)
				 */
				ts->hid_req_cfg.reg_data_sz = len - 1 - 1 - 4 - 1 - 1;
				/* Reg addr at buf[7] */
				ts->hid_req_cfg.reg_addr.dword = buf[7];
				/* Call himax_read to read AHB register */
				ret = himax_read(ts, ts->hid_req_cfg.reg_addr.dword,
						 ts->hid_req_cfg.reg_data,
						 ts->hid_req_cfg.reg_data_sz);
				/* If read success, copy data to buf[8:] */
				if (!ret) {
					buf[0] = HIMAX_ID_REG_RW;
					memcpy(&buf[8], ts->hid_req_cfg.reg_data,
					       ts->hid_req_cfg.reg_data_sz);
					ret = len;
				}
				break;
			/* SRAM/REG type REG RW, Address 4, Data 1~256 */
			case HIMAX_REG_TYPE_EXT_SRAM:
				ts->hid_req_cfg.reg_addr_sz = 4;
				/*
				 * Data length = total length from user space tool - ID(1) - R/W(1)
				 *		 - EXT_HDR(4) - EXT_TYPE(1) - REG_ADDR(4)
				 */
				ts->hid_req_cfg.reg_data_sz = len - 1 - 1 - 4 - 1 - 4;
				/* Reg addr at buf[7:10] */
				ts->hid_req_cfg.reg_addr.dword =
					((union himax_dword_data *)&buf[7])->dword;
				ret = himax_mcu_register_read(ts, ts->hid_req_cfg.reg_addr.dword,
							      ts->hid_req_cfg.reg_data,
							      ts->hid_req_cfg.reg_data_sz);
				/* If read success, copy data to buf[11:] */
				if (!ret) {
					buf[0] = HIMAX_ID_REG_RW;
					memcpy(&buf[11], ts->hid_req_cfg.reg_data,
					       ts->hid_req_cfg.reg_data_sz);
					ret = len;
				} else {
					ret = 0;
				}
				break;
			default:
				dev_err(ts->dev, "%s: Invalid ext type\n", __func__);
				ret = -EINVAL;
			}
		} else {
			dev_err(ts->dev, "%s: Invalid reg format!\n", __func__);
			ret = -EINVAL;
		}
		break;
	/* Report current report descriptor disable state */
	case HIMAX_ID_INPUT_RD_DE:
		buf[0] = HIMAX_ID_INPUT_RD_DE;
		buf[1] = ts->hid_req_cfg.input_RD_de;
		ret = len;
		break;
	/* HIMAX_ID_FW_UPDATE is for FW write only */
	case HIMAX_ID_FW_UPDATE:
		ret = 0;
		break;
	/* Report the stylus data */
	case HIMAX_ID_USI_COLOR:
		ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_STYLUS_INFO, (u8 *)&usi_info,
					      sizeof(struct himax_usi_info));
		if (ret)
			break;
		buf[0] = HIMAX_ID_USI_COLOR;
		buf[1] = usi_info.pen_transducer;
		buf[2] = usi_info.pen_color;
		buf[3] = usi_info.pen_color_locked;
		ret = 4;
		break;
	case HIMAX_ID_USI_WIDTH:
		ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_STYLUS_INFO, (u8 *)&usi_info,
					      sizeof(struct himax_usi_info));
		if (ret)
			break;
		buf[0] = HIMAX_ID_USI_WIDTH;
		buf[1] = usi_info.pen_transducer;
		buf[2] = usi_info.pen_width;
		buf[3] = usi_info.pen_width_locked;
		ret = 4;
		break;
	case HIMAX_ID_USI_STYLE:
		ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_STYLUS_INFO, (u8 *)&usi_info,
					      sizeof(struct himax_usi_info));
		if (ret)
			break;
		buf[0] = HIMAX_ID_USI_STYLE;
		buf[1] = usi_info.pen_transducer;
		buf[2] = usi_info.pen_style;
		buf[3] = usi_info.pen_style_locked;
		ret = 4;
		break;
	case HIMAX_ID_USI_BUTTONS:
		ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_STYLUS_INFO, (u8 *)&usi_info,
					      sizeof(struct himax_usi_info));
		if (ret)
			break;
		buf[0] = HIMAX_ID_USI_BUTTONS;
		buf[1] = usi_info.pen_transducer;
		memcpy(&buf[2], usi_info.pen_buttons, 3);
		ret = 5;
		break;
	case HIMAX_ID_USI_FIRMWARE:
		ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_STYLUS_INFO, (u8 *)&usi_info,
					      sizeof(struct himax_usi_info));
		if (ret)
			break;
		buf[0] = HIMAX_ID_USI_FIRMWARE;
		buf[1] = usi_info.pen_transducer;
		memcpy(&buf[2], usi_info.pen_firmware_version, 12);
		ret = 14;
		break;
	case HIMAX_ID_USI_PROTOCOL:
		ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_STYLUS_INFO, (u8 *)&usi_info,
					      sizeof(struct himax_usi_info));
		if (ret)
			break;
		buf[0] = HIMAX_ID_USI_PROTOCOL;
		buf[1] = usi_info.pen_transducer;
		buf[2] = usi_info.pen_protocol_major;
		buf[3] = usi_info.pen_protocol_minor;
		ret = 4;
		break;
	case HIMAX_ID_USI_TRANSDUCER:
		ret = 0;
		break;
	/* Report the windows blob data */
	case HIMAX_ID_WINDOWS_BLOB_VALID:
		buf[0] = HIMAX_ID_WINDOWS_BLOB_VALID;
		memcpy(buf + 1, g_windows_blob_validation_key,
		       sizeof(g_windows_blob_validation_key));
		ret = len;
		break;
	default:
		dev_err(ts->dev, "%s: Invalid report number\n", __func__);
		ret = -EINVAL;
		break;
	};

	return ret;
}

/**
 * himax_hid_set_raw_report() - process hidraw SET REPORT operation
 * @hid: HID device
 * @reportnum: Report ID
 * @buf: Buffer for communication
 * @len: Length of data in the buffer
 * @report_type: Report type
 *
 * The function for hid_ll_driver.set_raw_report to handle the HIDRAW ioctl
 * set report request. The report number to handle is based on the report
 * descriptor of the HID device. The buf is used to communicate with user
 * program, user pass the ID and parameters to the driver use this buf, and
 * the driver will return the result to user also use this buf. The len is
 * the length of data in the buf, passed by user program. The report_type is
 * not used in this driver. We currently support the following report number:
 * - HIMAX_ID_FW_UPDATE: Collect the firmware data and update the firmware
 * - HIMAX_ID_FW_UPDATE_HANDSHAKING: Handshaking status of the FW update
 * - HIMAX_ID_SELF_TEST: Start the specified self test
 * - HIMAX_ID_TOUCH_MONITOR_SEL: Set the touch data type, part of the self test
 * - HIMAX_ID_REG_RW: Write date to specified register/sram
 * - HIMAX_ID_INPUT_RD_DE: Set the report descriptor disable state
 * - HIMAX_ID_CONTACT_COUNT: Not support set report, return 0
 * - HIMAX_ID_CFG: Not support set report, return 0
 * - HIMAX_ID_TOUCH_MONITOR: Not support set report, return 0
 * - HIMAX_ID_USI_COLOR: USI color report ID
 * - HIMAX_ID_USI_WIDTH: USI width report ID
 * - HIMAX_ID_USI_STYLE: USI style report ID
 * - HIMAX_ID_USI_BUTTONS: USI buttons report ID
 * - HIMAX_ID_USI_FIRMWARE: Not support set report, return 0
 * - HIMAX_ID_USI_PROTOCOL: Not support set report, return 0
 * - HIMAX_ID_USI_TRANSDUCER: USI transducer report ID for SET
 * - HIMAX_ID_WINDOWS_BLOB_VALID: Not support set report, return 0
 * Case not listed here will return -EINVAL.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_hid_set_raw_report(const struct hid_device *hid,
				    unsigned char reportnum, __u8 *buf, size_t len,
				    unsigned char report_type)
{
	int ret;
	int i;
	struct himax_ts_data *ts;
	struct himax_usi_cmd usi_cmd;
	union himax_dword_data *tmp_data;

	ts = hid->driver_data;
	if (!ts) {
		dev_err(ts->dev, "hid->driver_data is NULL");
		return -EINVAL;
	}

	switch (reportnum) {
	case HIMAX_ID_FW_UPDATE:
		if (ts->hid_req_cfg.processing_id == HIMAX_ID_FW_UPDATE_HANDSHAKING) {
			if (ts->hid_req_cfg.handshake_get == g_dummy_main_code[0].cmd) {
				ts->hid_req_cfg.handshake_set = g_dummy_main_code[0].cmd;
				ts->hid_req_cfg.current_size += len - 1;
				ret = 0;
				break;
			} else if (ts->hid_req_cfg.handshake_get == g_dummy_main_code[1].cmd) {
				ts->hid_req_cfg.handshake_set = g_dummy_main_code[1].cmd;
				ts->hid_req_cfg.current_size += len - 1;
				ret = 0;
				break;
			}
		}
		ret = himax_hid_load_user_firmware(ts, buf, len);
		if (ret < 0) {
			dev_err(ts->dev, "%s: load user firmware failed\n", __func__);
			break;
		} else if (ret == HIMAX_LOAD_FIRMWARE_ONGOING) {
			ret = 0;
			break;
		}
		dev_info(ts->dev, "%s: load user firmware succeeded\n", __func__);

		ts->hid_req_cfg.processing_id = HIMAX_ID_FW_UPDATE;
		ts->hid_req_cfg.handshake_get = HIMAX_FWUP_FLASH_PROG_ERROR;
		himax_int_enable(ts, false);
		ret = hx83102j_sense_off(ts, false);
		if (ret)
			break;
		/* Lock it, free after re-init complete */
		mutex_lock(&ts->hid_ioctl_lock);
		/* Need to remove HID and probe again, queue re-init work and return immediately */
		schedule_delayed_work(&ts->initial_work, msecs_to_jiffies(0));
		ret = 0;
		break;
	case HIMAX_ID_FW_UPDATE_HANDSHAKING:
		ts->hid_req_cfg.processing_id = HIMAX_ID_FW_UPDATE_HANDSHAKING;
		ts->hid_req_cfg.handshake_set = buf[1];
		ret = 0;
		break;
	case HIMAX_ID_SELF_TEST:
		ts->hid_req_cfg.processing_id = HIMAX_ID_SELF_TEST;
		ts->hid_req_cfg.handshake_set = buf[1];
		dev_info(ts->dev, "%s: Initial self test\n", __func__);
		switch (buf[1]) {
		case HIMAX_HID_SELF_TEST_SHORT:
			ts->hid_req_cfg.self_test_type = HIMAX_INSPECT_SHORT;
			break;
		case HIMAX_HID_SELF_TEST_OPEN:
			ts->hid_req_cfg.self_test_type = HIMAX_INSPECT_OPEN;
			break;
		case HIMAX_HID_SELF_TEST_MICRO_OPEN:
			ts->hid_req_cfg.self_test_type = HIMAX_INSPECT_MICRO_OPEN;
			break;
		case HIMAX_HID_SELF_TEST_RAWDATA:
			ts->hid_req_cfg.self_test_type = HIMAX_INSPECT_RAWDATA;
			break;
		case HIMAX_HID_SELF_TEST_NOISE:
			ts->hid_req_cfg.self_test_type = HIMAX_INSPECT_ABS_NOISE;
			break;
		case HIMAX_HID_SELF_TEST_RESET:
			ts->hid_req_cfg.self_test_type = HIMAX_INSPECT_BACK_NORMAL;
			break;
		default:
			dev_info(ts->dev, "Not support self test type, set to default(HIMAX_INSPECT_BACK_NORMAL)");
			ts->hid_req_cfg.self_test_type = HIMAX_INSPECT_BACK_NORMAL;
		}
		/* Back to normal TP report mode */
		if (ts->hid_req_cfg.self_test_type == HIMAX_INSPECT_BACK_NORMAL) {
			ret = himax_switch_data_type(ts, HIMAX_INSPECT_BACK_NORMAL);
			if (ret < 0) {
				dev_err(ts->dev, "%s: switch data type to normal failed\n",
					__func__);
				break;
			}

			himax_int_enable(ts, false);
			ret = himax_disable_fw_reload(ts);
			if (ret < 0) {
				dev_err(ts->dev, "%s: disable FW reload failed\n", __func__);
				break;
			}

			ret = himax_mcu_power_on_init(ts);
			if (ret < 0) {
				dev_err(ts->dev, "%s: power on init failed\n", __func__);
				break;
			}

			himax_int_enable(ts, true);
			ret = 0;
			break;
		}
		/* Lock it, free after self test finished */
		mutex_lock(&ts->hid_ioctl_lock);
		/* Disable interrupt, all work should be AP controlled */
		himax_int_enable(ts, false);
		/* Queue self test work, unblock hidraw interface */
		queue_delayed_work(ts->himax_hidraw_wq, &ts->work_self_test,
				   msecs_to_jiffies(0));
		ret = 0;
		break;
	case HIMAX_ID_TOUCH_MONITOR_SEL:
		for (i = 0; i < HIMAX_HID_RAW_DATA_TYPE_MAX; i++) {
			if (buf[1] == g_himax_hid_raw_data_type[i]) {
				himax_mcu_diag_register_set(ts, buf[1]);
				break;
			}
		}
		if (i == HIMAX_HID_RAW_DATA_TYPE_MAX) {
			dev_err(ts->dev, "%s: Not support data type\n", __func__);
			ret = -EINVAL;
			break;
		}
		ts->hid_req_cfg.processing_id = HIMAX_ID_TOUCH_MONITOR_SEL;
		ts->hid_req_cfg.handshake_set = buf[1];
		ret = 0;
		break;
	case HIMAX_ID_REG_RW:
		/*
		 * standard REG RW, Address 4 bytes, Data 4 bytes all fixed
		 * standard REG RW len = 10 : ID(1) | R/W(1) | ADDR(4) | DATA(4)
		 */
		if (len == 10 &&
		    ((union himax_dword_data *)&buf[2])->dword != HIMAX_REG_TYPE_EXT_TYPE) {
			/* standard REG RW */
			if (buf[1] == HIMAX_HID_REG_READ) {
				ret = 0;
				break;
			}
			ts->hid_req_cfg.reg_addr_sz = 4;
			ts->hid_req_cfg.reg_data_sz = 4;
			ts->hid_req_cfg.reg_addr.dword =
				((union himax_dword_data *)&buf[2])->dword;
			memcpy(ts->hid_req_cfg.reg_data, &buf[6], 4);
			tmp_data = (union himax_dword_data *)(ts->hid_req_cfg.reg_data);
			tmp_data->dword = cpu_to_le32(tmp_data->dword);
			ret = himax_mcu_register_write(ts,
						       ts->hid_req_cfg.reg_addr.dword,
						       ts->hid_req_cfg.reg_data, 4);
		/*
		 * EXT type REG RW, Address 1 for AHB, 4 for SRAM/REG, Data 1~256
		 * EXT REG RW len >= 9 : ID(1) | R/W(1) | EXT_HDR(4) | EXT_TYPE(1) | REG_ADDR(1/4)
		 *			 | REG_DATA(1~256) => min 9, max 267
		 */
		} else if ((len >= 9) && (len <= (1 + HIMAX_HID_REG_SZ_MAX)) &&
			(((union himax_dword_data *)&buf[2])->dword == HIMAX_REG_TYPE_EXT_TYPE)) {
			if (buf[1] == HIMAX_HID_REG_READ) {
				ret = 0;
				break;
			}
			switch (buf[6]) {
			/* AHB type REG RW, Address 1, Data 1~256 */
			case HIMAX_REG_TYPE_EXT_AHB:
				ts->hid_req_cfg.reg_addr_sz = 1;
				/*
				 * Data length = total length from user space tool - ID(1) - R/W(1)
				 *		 - EXT_HDR(4) - EXT_TYPE(1) - REG_ADDR(1)
				 */
				ts->hid_req_cfg.reg_data_sz = len - 1 - 1 - 4 - 1 - 1;
				ts->hid_req_cfg.reg_addr.dword = buf[7];
				memcpy(ts->hid_req_cfg.reg_data, &buf[8],
				       ts->hid_req_cfg.reg_data_sz);
				ret = himax_write(ts, ts->hid_req_cfg.reg_addr.dword, NULL,
						  ts->hid_req_cfg.reg_data,
						  ts->hid_req_cfg.reg_data_sz);
				break;
			/* SRAM/REG type REG RW, Address 4, Data 1~256 */
			case HIMAX_REG_TYPE_EXT_SRAM:
				ts->hid_req_cfg.reg_addr_sz = 4;
				/*
				 * Data length = total length from user space tool - ID(1) - R/W(1)
				 *		 - EXT_HDR(4) - EXT_TYPE(1) - REG_ADDR(4)
				 */
				ts->hid_req_cfg.reg_data_sz = len - 1 - 1 - 4 - 1 - 4;
				ts->hid_req_cfg.reg_addr.dword =
					((union himax_dword_data *)&buf[7])->dword;
				memcpy(ts->hid_req_cfg.reg_data, &buf[11],
				       ts->hid_req_cfg.reg_data_sz);
				ret = himax_mcu_register_write(ts,
							       ts->hid_req_cfg.reg_addr.dword,
							       ts->hid_req_cfg.reg_data,
							       ts->hid_req_cfg.reg_data_sz);
				break;
			default:
				dev_err(ts->dev, "%s: Invalid ext type\n", __func__);
				ret = -EINVAL;
				break;
			}
		} else {
			dev_err(ts->dev, "%s: Invalid reg format!\n", __func__);
			ret = -EINVAL;
			break;
		}
		ts->hid_req_cfg.processing_id = HIMAX_ID_REG_RW;
		ts->hid_req_cfg.handshake_set = ts->hid_req_cfg.reg_addr.dword;
		break;
	case HIMAX_ID_INPUT_RD_DE:
		ts->hid_req_cfg.processing_id = HIMAX_ID_INPUT_RD_DE;
		ts->hid_req_cfg.handshake_set = !!buf[1];
		if (ts->hid_req_cfg.input_RD_de != (!!buf[1])) {
			ts->hid_req_cfg.input_RD_de = !!buf[1];
			/* Re-register HID to update report descriptor */
			queue_delayed_work(ts->himax_hidraw_wq, &ts->work_hid_update,
					   msecs_to_jiffies(0));
		}
		ret = 0;
		break;
	case HIMAX_ID_USI_COLOR:
	case HIMAX_ID_USI_WIDTH:
	case HIMAX_ID_USI_STYLE:
	case HIMAX_ID_USI_BUTTONS:
	case HIMAX_ID_USI_TRANSDUCER:
		memset(&usi_cmd, 0, sizeof(struct himax_usi_cmd));
		memcpy(&usi_cmd, buf, min(len, sizeof(struct himax_usi_cmd)));
		ret = himax_usi_write_cmd(ts, &usi_cmd);
		break;
	case HIMAX_ID_USI_FIRMWARE:
	case HIMAX_ID_USI_PROTOCOL:
	case HIMAX_ID_CONTACT_COUNT:
	case HIMAX_ID_CFG:
	case HIMAX_ID_TOUCH_MONITOR:
	case HIMAX_ID_WINDOWS_BLOB_VALID:
		ret = 0;
		break;
	default:
		ret = -EINVAL;
	};

	return ret;
}

/**
 * himax_raw_request - Handle the HIDRAW ioctl request
 * @hid: HID device
 * @reportnum: Report ID
 * @buf: Buffer for communication
 * @len: Length of data in the buffer
 * @rtype: Report type
 * @reqtype: Request type
 *
 * The function for hid_ll_driver.raw_request to handle the HIDRAW ioctl
 * request. We handle only the GET_REPORT and SET_REPORT request.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_raw_request(struct hid_device *hid, unsigned char reportnum, __u8 *buf,
			     size_t len, unsigned char rtype, int reqtype)
{
	int ret;

	switch (reqtype) {
	case HID_REQ_GET_REPORT:
		ret = himax_hid_get_raw_report(hid, reportnum, buf, len, rtype);
		break;
	case HID_REQ_SET_REPORT:
		if (buf[0] != reportnum) {
			ret = -EINVAL;
			break;
		}
		ret = himax_hid_set_raw_report(hid, reportnum, buf, len, rtype);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static struct hid_ll_driver himax_hid_ll_driver = {
	.parse = himax_hid_parse,
	.start = himax_hid_start,
	.stop = himax_hid_stop,
	.open = himax_hid_open,
	.close = himax_hid_close,
	.raw_request = himax_raw_request
};

/**
 * himax_hid_report() - Report input data to HID core
 * @ts: Himax touch screen data
 * @data: Data to report
 * @len: Length of the data
 *
 * This function is a wrapper to report input data to HID core.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_hid_report(const struct himax_ts_data *ts, u8 *data, s32 len)
{
	return hid_input_report(ts->hid, HID_INPUT_REPORT, data, len, 1);
}

/**
 * himax_hid_probe() - Probe the HID device
 * @ts: Himax touch screen data
 *
 * This function is used to probe the HID device.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_hid_probe(struct himax_ts_data *ts)
{
	int ret;
	struct hid_device *hid;

	hid = ts->hid;
	if (hid) {
		hid_destroy_device(hid);
		hid = NULL;
	}

	hid = hid_allocate_device();
	if (IS_ERR(hid)) {
		ret = PTR_ERR(hid);
		return ret;
	}

	hid->driver_data = ts;
	hid->ll_driver = &himax_hid_ll_driver;
	hid->bus = BUS_SPI;
	hid->dev.parent = &ts->spi->dev;

	hid->version = ts->hid_desc.bcd_version;
	hid->vendor = ts->hid_desc.vendor_id;
	hid->product = ts->hid_desc.product_id;
	snprintf(hid->name, sizeof(hid->name), "%s %04X:%04X", "hid-hxtp",
		 hid->vendor, hid->product);

	ret = hid_add_device(hid);
	if (ret) {
		dev_err(ts->dev, "%s: failed add hid device\n", __func__);
		goto err_hid_data;
	}
	ts->hid = hid;

	return 0;

err_hid_data:
	hid_destroy_device(hid);

	return ret;
}

/**
 * himax_hid_remove() - Remove the HID device
 * @ts: Himax touch screen data
 *
 * This function is used to remove the HID device.
 * It will free the firmware space if it is not NULL.
 *
 * Return: None
 */
static void himax_hid_remove(struct himax_ts_data *ts)
{
	mutex_lock(&ts->hid_ioctl_lock);
	if (ts && ts->hid)
		hid_destroy_device(ts->hid);
	else
		goto out;

	ts->hid = NULL;
	if (ts->hid_req_cfg.fw) {
		dev_info(ts->dev, "%s: free fw\n", __func__);
		free_firmware(ts, ts->hid_req_cfg.fw);
		ts->hid_req_cfg.fw = NULL;
	}
out:
	mutex_unlock(&ts->hid_ioctl_lock);
}

/**
 * himax_mcu_ic_excp_check() - Check the exception type
 * @ts: Himax touch screen data
 * @buf: input buffer
 *
 * This function is used to categorize the exception type and report the exception
 * event to caller. The event type is categorized into exception event and all zero
 * event. The function will check the first byte of interrupt data only because
 * previous function has already confirm all data is the same. If the 1st byte data
 * is not zero then return HIMAX_TS_EXCP_EVENT. Otherwise, it will increment the
 * global all zero event count and check if it reached exception threshold. If it
 * reached, it will return HIMAX_TS_EXCP_EVENT. If it is not reached, it will return
 * HIMAX_TS_ZERO_EVENT_CNT.
 *
 * return:
 * - HIMAX_TS_EXCP_EVENT     : recovery is needed
 * - HIMAX_TS_ZERO_EVENT_CNT : all zero event checked
 */
static int himax_mcu_ic_excp_check(struct himax_ts_data *ts, const u8 *buf)
{
	bool excp_flag = false;
	const u32 all_zero_excp_event_threshold = 5;

	switch (buf[0]) {
	case 0x00:
		dev_info(ts->dev, "%s: [HIMAX TP MSG]: EXCEPTION event checked - ALL 0x00\n",
			 __func__);
		excp_flag = false;
		break;
	default:
		dev_info(ts->dev, "%s: [HIMAX TP MSG]: EXCEPTION event checked - All 0x%02X\n",
			 __func__, buf[0]);
		excp_flag = true;
	}

	if (!excp_flag) {
		ts->excp_zero_event_count++;
		dev_info(ts->dev, "%s: ALL Zero event %d times\n", __func__,
			 ts->excp_zero_event_count);
		if (ts->excp_zero_event_count == all_zero_excp_event_threshold) {
			ts->excp_zero_event_count = 0;
			return HIMAX_TS_EXCP_EVENT;
		}

		return HIMAX_TS_ZERO_EVENT_CNT;
	}

	ts->excp_zero_event_count = 0;

	return HIMAX_TS_EXCP_EVENT;
}

/**
 * himax_excp_hw_reset() - Do the ESD recovery
 * @ts: Himax touch screen data
 *
 * This function is used to do the ESD recovery. It will remove the HID device,
 * reload the firmware, and probe the HID device again. Because finger/stylus
 * may stuck on the touch screen, it will remove the HID device first to avoid
 * this situation.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_excp_hw_reset(struct himax_ts_data *ts)
{
	int ret;

	dev_info(ts->dev, "%s: START EXCEPTION Reset\n", __func__);
	himax_hid_remove(ts);
	ret = himax_zf_reload_from_file(ts->firmware_name, ts);
	if (ret) {
		dev_err(ts->dev, "%s: update FW fail, error: %d\n", __func__, ret);
		return ret;
	}

	dev_info(ts->dev, "%s: update FW success\n", __func__);
	ret = himax_hid_probe(ts);
	if (ret) {
		dev_err(ts->dev, "%s: hid probe fail, error: %d\n", __func__, ret);
		return ret;
	}
	dev_info(ts->dev, "%s: END EXCEPTION Reset\n", __func__);

	return 0;
}

/**
 * himax_ts_event_check() - Check the abnormal interrupt data
 * @ts: Himax touch screen data
 * @buf: Interrupt data
 *
 * This function is used to check the abnormal interrupt data.
 * If the data pattern matched the exception pattern, it will try to do
 * the ESD recovery. For all zero data, it needs to be continuous reported
 * for several times to trigger the ESD recovery(checked by himax_mcu_ic_excp_check())
 *
 * Return:
 * - HIMAX_TS_EXCP_EVENT     : exception recovery event
 * - HIMAX_TS_ZERO_EVENT_CNT : zero event count
 * - HIMAX_TS_EXCP_REC_OK    : exception recovery done
 * - HIMAX_TS_EXCP_REC_FAIL  : exception recovery error
 */
static int himax_ts_event_check(struct himax_ts_data *ts, const u8 *buf)
{
	int ret;

	/* The first data read out right after chip reset is invalid. Drop it. */
	if (ts->excp_reset_active) {
		ts->excp_reset_active = false;
		dev_info(ts->dev, "%s: Skip data after reset\n", __func__);

		return HIMAX_TS_EXCP_REC_OK;
	}

	/* No data after reset, check exception pattern */
	ret = himax_mcu_ic_excp_check(ts, buf);
	switch (ret) {
	/* Exception pattern matched, do recovery */
	case HIMAX_TS_EXCP_EVENT:
		/* Print debug message only, no check return */
		himax_mcu_read_FW_status(ts);
		ret = himax_excp_hw_reset(ts);
		if (ret) {
			dev_err(ts->dev, "%s: Recovery error!\n", __func__);
			return HIMAX_TS_EXCP_REC_FAIL;
		}
		ts->excp_reset_active = true;
		ret = HIMAX_TS_EXCP_EVENT;
		break;
	/* All zero event, but not reach reset threshold print debug message only */
	case HIMAX_TS_ZERO_EVENT_CNT:
		/* Print debug message only, no check return */
		himax_mcu_read_FW_status(ts);
		break;
	}

	return ret;
}

/**
 * himax_err_ctrl() - ESD check and recovery
 * @ts: Himax touch screen data
 * @buf: Interrupt data
 *
 * This function is used to check the interrupt data. Use himax_input_check()
 * to check the data. If the data is not valid, it will call himax_ts_event_check()
 * to see if data match the exception pattern and do the ESD recovery when needed.
 * If the data is valid, it will clear the exception counters and return
 * HIMAX_TS_REPORT_DATA.
 *
 * Return:
 * - HIMAX_TS_REPORT_DATA   : valid data
 * - HIMAX_TS_EXCP_EVENT    : exception match
 * - HIMAX_TS_ZERO_EVENT_CNT: zero frame event counted
 * - HIMAX_TS_EXCP_REC_OK   : exception recovery done
 * - HIMAX_TS_EXCP_REC_FAIL : exception recovery error
 */
static int himax_err_ctrl(struct himax_ts_data *ts, u8 *buf)
{
	int ret;

	ret = himax_input_check(ts, buf);
	if (ret == HIMAX_TS_ABNORMAL_PATTERN)
		return himax_ts_event_check(ts, buf);

	/* data check passed, clear excp counters */
	ts->excp_zero_event_count = 0;
	ts->excp_reset_active = false;

	return ret;
}

/**
 * heatmap_decompress_12bits() - Decompress 12bits heatmap data to 16bits
 * @ts: Himax touch screen data
 * @in_buf: Compressed heatmap data
 * @target: Decompressed heatmap data
 *
 * This function is used to decompress the 12bits heatmap data to 16bits.
 * The 12bits heatmap data is compressed in 3 bytes for 2 pixels, here we
 * restore each 3 bytes to 4 bytes in the target buffer. the format of 12 bits:
 * [value 1 low byte]|[value 2 low byte]|[value 1 high nibble][value 2 high nibble]
 *   8 bits		8 bits		   4 bits		4 bits
 * restored 16 bits:
 * [high nibble][low byte] : mask 0x0FFF, MSB 4 bits are ignored
 *
 * Return: None
 */
static void heatmap_decompress_12bits(struct himax_ts_data *ts, u8 *in_buf, u8 *target)
{
	int i;
	/* 1 byte for ID, else are HEATMAP headers */
	const int in_offset = HIMAX_HEAT_MAP_INFO_SZ + 1;
	const int heatmap_data_sz = ts->ic_data.rx_num * ts->ic_data.tx_num;
	u8 *in_ptr;
	const u8 h_nibble = GENMASK(7, 4);
	const u8 l_nibble = GENMASK(3, 0);
	u16 *target_ptr;

	/* Copy headers to target buffer */
	memcpy(target, in_buf, in_offset);
	/* Restore 2 16bits raw data each time */
	for (i = 0; i < heatmap_data_sz; i += 2) {
		/* locate offset in in_buf, step 3 bytes each time */
		in_ptr = &in_buf[in_offset + i * 3 / 2];
		/* locate target offset, step 2 16bits each time */
		target_ptr = (u16 *)&target[in_offset + i * 2];
		/* 1st 16bits of 2 16bits raw data */
		*target_ptr = (u16)in_ptr[0] | ((u16)(in_ptr[2] & h_nibble) << 4);
		/* 2nd 16bits of 2 16bits raw data */
		*(target_ptr + 1) = (u16)in_ptr[1] | (u16)(in_ptr[2] & l_nibble) << 8;
	}
}

/**
 * himax_ts_operation() - Process the touch interrupt data
 * @ts: Himax touch screen data
 *
 * This function is used to process the touch interrupt data. It will
 * call the himax_touch_get() to get the touch data.
 * Check the data by calling the himax_err_ctrl() to see if the data is
 * valid. If the data is not valid, it also process the ESD recovery.
 * If the hid is probed, it will call the himax_hid_report() to report the
 * touch data to the HID core. Due to the report data must match the HID
 * report descriptor, the size of report data is fixed. To prevent next report
 * data being contaminated by the previous data, all the data must be reported
 * wheather previous data is valid or not.
 * The heatmap data will be decompressed by calling the heatmap_decompress_12bits()
 * if heatmap is not encoded in 16bits, otherwise it will be copied directly.
 *
 * Return: HIMAX_TS_SUCCESS on success, negative error code in
 * himax_touch_report_status on failure
 */
static int himax_ts_operation(struct himax_ts_data *ts)
{
	int ret;
	u32 offset;

	memset(ts->xfer_buf, 0x00, ts->xfer_buf_sz);
	ret = himax_touch_get(ts, ts->xfer_buf);
	if (ret == HIMAX_TS_GET_DATA_FAIL)
		return ret;
	ret = himax_err_ctrl(ts, ts->xfer_buf);
	if (!(ret == HIMAX_TS_REPORT_DATA))
		return ret;
	if (ts->hid_probed) {
		offset = 0;
		if (!ts->hid_req_cfg.input_RD_de)
			ret = himax_hid_report(ts,
					       ts->xfer_buf + offset + HIMAX_HID_REPORT_HDR_SZ,
					       ts->hid_desc.max_input_length -
					       HIMAX_HID_REPORT_HDR_SZ);
		offset = ts->hid_desc.max_input_length;
		if (ts->ic_data.stylus_function) {
			if (!ts->hid_req_cfg.input_RD_de)
				ret += himax_hid_report(ts,
							ts->xfer_buf +
							offset + HIMAX_HID_REPORT_HDR_SZ,
							ts->hid_desc.max_input_length -
							HIMAX_HID_REPORT_HDR_SZ);
			offset += ts->hid_desc.max_input_length;
		}
		if (!ts->ic_data.enc16bits)
			heatmap_decompress_12bits(ts,
						  ts->xfer_buf + offset + HIMAX_HID_REPORT_HDR_SZ,
						  ts->heatmap_buf);
		else
			memcpy(ts->heatmap_buf,
			       ts->xfer_buf + offset + HIMAX_HID_REPORT_HDR_SZ,
			       ts->heatmap_data_size + HIMAX_HEAT_MAP_INFO_SZ + 1);

		ret += himax_hid_report(ts, ts->heatmap_buf,
					(ts->ic_data.rx_num * ts->ic_data.tx_num * 2) +
					HIMAX_HEAT_MAP_INFO_SZ + 1);
	}

	if (ret != 0)
		return HIMAX_TS_GET_DATA_FAIL;

	return HIMAX_TS_SUCCESS;
}

/**
 * himax_cable_detect_func - cable status update handler
 * @ts: himax touch screen data
 * @force_renew: force renew cable status
 *
 * This function is used to maintain the cable status and call the
 * corresponding function to update the cable status. If the update action
 * is failed, it will print the error message and retry the action at the next
 * time called.
 *
 * Return: Return: 0 on success, negative error code on failure
 */
static int himax_cable_detect_func(struct himax_ts_data *ts, bool force_renew)
{
	int ret;
	bool connect_status = ts->latest_power_status > 0 ? true : false;

	if (connect_status != ts->usb_connected || force_renew) {
		ret = himax_mcu_usb_detect_set(ts, connect_status);
		if (ret < 0) {
			dev_err(ts->dev, "%s: Cable status change to %s failed\n", __func__,
				connect_status ? "connected" : "disconnected");
			return ret;
		}
		ts->usb_connected = connect_status;
		dev_info(ts->dev, "%s: Cable status change: %s\n", __func__,
			 ts->usb_connected ? "connected" : "disconnected");
	}

	return 0;
}

/**
 * himax_ts_work() - Work function for the touch screen
 * @ts: Himax touch screen data
 *
 * This function is used to handle interrupt bottom half work. It will
 * call the himax_ts_operation() to get the touch data, dispatch the data
 * to HID core. If the touch data is not valid, it will reset the TPIC.
 * It will also call the hx83102j_reload_to_active() after the reset action.
 * It will also call the himax_cable_detect_func() to check the cable status,
 * and update the cable status to the FW if needed.
 *
 * Return: void
 */
static void himax_ts_work(struct himax_ts_data *ts)
{
	himax_cable_detect_func(ts, false);
	if (himax_ts_operation(ts) == HIMAX_TS_GET_DATA_FAIL) {
		dev_info(ts->dev, "%s: Now reset the Touch chip\n", __func__);
		himax_mcu_ic_reset(ts, true);
		if (hx83102j_reload_to_active(ts))
			dev_warn(ts->dev, "%s: Reload to active failed\n", __func__);
	}
}

/**
 * himax_update_fw() - update firmware using firmware structure
 * @ts: Himax touch screen data
 *
 * This function use already initialize firmware structure in ts to update
 * firmware.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_update_fw(struct himax_ts_data *ts)
{
	int ret;
	u32 retry_cnt;
	const u32 retry_limit = 3;

	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		ret = himax_mcu_firmware_update_zf(ts->himax_fw, ts);
		if (ret < 0) {
			dev_err(ts->dev, "%s: TP upgrade error, upgrade_times = %d\n", __func__,
				retry_cnt);
		} else {
			dev_info(ts->dev, "%s: TP FW upgrade OK\n", __func__);
			return 0;
		}
	}

	return -EIO;
}

/**
 * himax_hid_rd_init() - Initialize the HID report descriptor
 * @ts: Himax touch screen data
 *
 * The function is used to calculate the size of the HID report descriptor,
 * allocate the memory and copy the report descriptor from firmware data to
 * the allocated memory for later hid device registration.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_hid_rd_init(struct himax_ts_data *ts)
{
	u32 rd_sz;
	const u32 x_num = ts->ic_data.rx_num;
	const u32 y_num = ts->ic_data.tx_num;
	/* Data in 16bits and handshake data 4 bytes */
	u32 raw_data_sz = (x_num * y_num + x_num + y_num) * 2 + 4;

	/*
	 * If hidraw input debug function is enabled, the rd_sz is combined by
	 * heatmap_rd size and hidraw_debug_rd size. Otherwise the rd_sz is
	 * combined by FW RD size and hidraw_debug_rd size.
	 */
	if (!ts->hid_req_cfg.input_RD_de)
		rd_sz = ts->hid_desc.report_desc_length + g_host_ext_report_desc_sz;
	else
		rd_sz = g_host_heatmap_report_desc_sz + g_host_ext_report_desc_sz;
	/* fw_info_table should contain address of hid_rd_desc in FW image */
	if (ts->fw_info_table.addr_hid_rd_desc != 0) {
		/* if rd_sz has been change, need to release old one */
		if (ts->hid_rd_data.rd_data &&
		    rd_sz != ts->hid_rd_data.rd_length) {
			devm_kfree(ts->dev, ts->hid_rd_data.rd_data);
			ts->hid_rd_data.rd_data = NULL;
		}

		if (!ts->hid_rd_data.rd_data) {
			ts->hid_rd_data.rd_data = devm_kzalloc(ts->dev, rd_sz, GFP_KERNEL);
			if (!ts->hid_rd_data.rd_data)
				return -ENOMEM;
		}

		/* Copy the base RD from firmware table or heatmap RD only */
		if (!ts->hid_req_cfg.input_RD_de) {
			memcpy((void *)ts->hid_rd_data.rd_data,
			       &ts->himax_fw->data[ts->fw_info_table.addr_hid_rd_desc],
			       ts->hid_desc.report_desc_length);
			ts->hid_rd_data.rd_length = ts->hid_desc.report_desc_length;
		} else {
			memcpy((void *)ts->hid_rd_data.rd_data,
			       g_heatmap_rd.host_report_descriptor,
			       g_host_heatmap_report_desc_sz);
			ts->hid_rd_data.rd_length = g_host_heatmap_report_desc_sz;
		}

		/* Update monitor report_cnt by actual rawdata size */
		g_host_ext_rd.rd_struct.monitor.report_cnt = cpu_to_le16(raw_data_sz);
		/* Append hidraw_debug_rd to hid_rd_data */
		memcpy((void *)(ts->hid_rd_data.rd_data + ts->hid_rd_data.rd_length),
		       &g_host_ext_rd.host_report_descriptor, g_host_ext_report_desc_sz);
		ts->hid_rd_data.rd_length += g_host_ext_report_desc_sz;
	}

	return 0;
}

/**
 * himax_hid_register() - Register the HID device
 * @ts: Himax touch screen data
 *
 * The function is used to register the HID device. If the hid is probed,
 * it will destroy the previous hid device and re-probe the hid device.
 *
 * Return: None
 */
static void himax_hid_register(struct himax_ts_data *ts)
{
	if (ts->hid_probed) {
		hid_destroy_device(ts->hid);
		ts->hid = NULL;
		ts->hid_probed = false;
	}

	if (himax_hid_probe(ts)) {
		dev_err(ts->dev, "%s: hid probe fail\n", __func__);
		ts->hid_probed = false;
	} else {
		ts->hid_probed = true;
	}
}

/**
 * himax_hid_report_data_init() - Calculate the size of the HID report data
 * @ts: Himax touch screen data
 *
 * The function is used to calculate the final size of the HID report data.
 * The base size is equal to the max_input_length of the hid descriptor.
 * Plus the size of the heatmap data and its header. If the heatmap data is
 * encoded in 12bits, the size of heatmap is 1.5 times of the size of the
 * rx_num * tx_num. Otherwise, the size of heatmap is 2 times of the size of
 * the rx_num * tx_num.
 * If the size of the HID report data is not equal to the previous size, it
 * will free the previous allocated memory and allocate the new memory which
 * size is equal to the final size of touch_data_sz.
 * It will also call the himax_heatmap_data_init() to initialize the heatmap
 * data.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_hid_report_data_init(struct himax_ts_data *ts)
{
	ts->touch_data_sz = ts->hid_desc.max_input_length;
	if (ts->ic_data.stylus_function)
		ts->touch_data_sz += ts->hid_desc.max_input_length;
	ts->touch_data_sz += HIMAX_HEAT_MAP_HEADER_SZ;
	ts->touch_data_sz += HIMAX_HEAT_MAP_INFO_SZ;
	if (!ts->ic_data.enc16bits)
		ts->heatmap_data_size = (ts->ic_data.rx_num * ts->ic_data.tx_num * 3) / 2;
	else
		ts->heatmap_data_size = (ts->ic_data.rx_num * ts->ic_data.tx_num * 2);
	ts->touch_data_sz += ts->heatmap_data_size;
	if (ts->touch_data_sz != ts->xfer_buf_sz) {
		kfree(ts->xfer_buf);
		ts->xfer_buf_sz = 0;
		ts->xfer_buf = kzalloc(ts->touch_data_sz, GFP_KERNEL);
		if (!ts->xfer_buf)
			return -ENOMEM;
		ts->xfer_buf_sz = ts->touch_data_sz;
	}
	if (himax_heatmap_data_init(ts)) {
		dev_err(ts->dev, "%s: report data init fail\n", __func__);
		return -ENOMEM;
	}

	return 0;
}

/**
 * himax_power_set() - Set the power supply of touch screen
 * @ts: Himax touch screen data
 * @en: enable/disable regualtor
 *
 * This function is used to set the power supply of touch screen.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_power_set(struct himax_ts_data *ts, bool en)
{
	int ret;
	struct himax_platform_data *pdata = &ts->pdata;

	if (pdata->vccd_supply) {
		if (en)
			ret = regulator_enable(pdata->vccd_supply);
		else
			ret = regulator_disable(pdata->vccd_supply);
		if (ret) {
			dev_err(ts->dev, "%s: unable to %s vccd supply\n", __func__,
				en ? "enable" : "disable");
			return ret;
		}
	}

	if (pdata->vccd_supply)
		usleep_range(2000, 2100);

	return 0;
}

/**
 * himax_power_deconfig() - De-configure the power supply of touchsreen
 * @pdata: Himax platform data
 *
 * This function is used to de-configure the power supply of touch screen.
 *
 * Return: None
 */
static void himax_power_deconfig(struct himax_platform_data *pdata)
{
	if (pdata->vccd_supply) {
		regulator_disable(pdata->vccd_supply);
		regulator_put(pdata->vccd_supply);
	}
}

/**
 * himax_initial_work() - Initial work for the touch screen
 * @work: Work structure
 *
 * This function is used to do the initial work for the touch screen. It will
 * call the request_firmware() to get the firmware from the file system, and parse the
 * mapping table in 1k header. If the headers are parsed successfully, it will
 * call the himax_update_fw() to update the firmware and power on the touch screen.
 * If the power on action is successful, it will load the hid descriptor and
 * check the touch panel information. If the touch panel information is correct,
 * it will call the himax_hid_rd_init() to initialize the HID report descriptor,
 * and call the himax_hid_register() to register the HID device. After all is done,
 * it will release the firmware and enable the interrupt.
 *
 * Return: None
 */
static void himax_initial_work(struct work_struct *work)
{
	struct himax_ts_data *ts = container_of(work, struct himax_ts_data,
						initial_work.work);
	int ret;
	bool fw_load_status;
	const u32 fw_bin_header_sz = 1024;

	ts->ic_boot_done = false;
	if (ts->hid_req_cfg.fw) {
		ts->himax_fw = ts->hid_req_cfg.fw;
		dev_info(ts->dev, "%s: get fw from hid_req_cfg\n", __func__);
	} else {
		dev_info(ts->dev, "%s: request file %s\n", __func__, ts->firmware_name);
		ret = request_firmware(&ts->himax_fw, ts->firmware_name, ts->dev);
		if (ret < 0) {
			dev_err(ts->dev, "%s: request firmware failed, error code = %d\n",
				__func__, ret);
			return;
		}
	}
	/* Parse the mapping table in 1k header */
	fw_load_status = himax_mcu_bin_desc_get((unsigned char *)ts->himax_fw->data,
						ts, fw_bin_header_sz);
	if (!fw_load_status) {
		dev_err(ts->dev, "%s: Failed to parse the mapping table!\n", __func__);
		goto err_load_bin_descriptor;
	}

	if (himax_update_fw(ts)) {
		dev_err(ts->dev, "%s: Update FW fail\n", __func__);
		goto err_update_fw_failed;
	}

	dev_info(ts->dev, "%s: Update FW success\n", __func__);
	/* write flag to sram to stop fw reload again. */
	if (himax_disable_fw_reload(ts))
		goto err_disable_fw_reload;
	if (himax_mcu_power_on_init(ts))
		goto err_power_on_init;
	/* get hid descriptors */
	if (!ts->fw_info_table.addr_hid_desc) {
		dev_err(ts->dev, "%s: No HID descriptor! Wrong FW!\n", __func__);
		goto err_wrong_firmware;
	}
	memcpy(&ts->hid_desc,
	       &ts->himax_fw->data[ts->fw_info_table.addr_hid_desc],
	       sizeof(struct himax_hid_desc));
	ts->hid_desc.desc_length =
		le16_to_cpu(ts->hid_desc.desc_length);
	ts->hid_desc.bcd_version =
		le16_to_cpu(ts->hid_desc.bcd_version);
	ts->hid_desc.report_desc_length =
		le16_to_cpu(ts->hid_desc.report_desc_length);
	ts->hid_desc.max_input_length =
		le16_to_cpu(ts->hid_desc.max_input_length);
	ts->hid_desc.max_output_length =
		le16_to_cpu(ts->hid_desc.max_output_length);
	ts->hid_desc.max_fragment_length =
		le16_to_cpu(ts->hid_desc.max_fragment_length);
	ts->hid_desc.vendor_id =
		le16_to_cpu(ts->hid_desc.vendor_id);
	ts->hid_desc.product_id =
		le16_to_cpu(ts->hid_desc.product_id);
	ts->hid_desc.version_id =
		le16_to_cpu(ts->hid_desc.version_id);
	ts->hid_desc.flags =
		le16_to_cpu(ts->hid_desc.flags);

	if (himax_mcu_tp_info_check(ts))
		goto err_tp_info_failed;
	if (himax_mcu_read_FW_ver(ts))
		goto err_read_fw_ver;
	if (himax_hid_rd_init(ts)) {
		dev_err(ts->dev, "%s: hid rd init fail\n", __func__);
		goto err_hid_rd_init_failed;
	}

	if (ts->hid_req_cfg.fw) {
		/* Set flag of HIDRAW FW update result */
		ts->hid_req_cfg.handshake_get = HIMAX_FWUP_BL_READY;
		/* Unlock HIDRAW ioctl for result checking */
		mutex_unlock(&ts->hid_ioctl_lock);
	}
	usleep_range(1000000, 1000100);
	himax_hid_register(ts);
	if (!ts->hid_probed) {
		goto err_hid_probe_failed;
	} else {
		if (himax_hid_report_data_init(ts)) {
			dev_err(ts->dev, "%s: report data init fail\n", __func__);
			goto err_report_data_init_failed;
		}
	}

	/* Release FW if it is from request_firmware */
	if (!ts->hid_req_cfg.fw)
		release_firmware(ts->himax_fw);
	ts->himax_fw = NULL;

	ts->ic_boot_done = true;
	himax_int_enable(ts, true);

	return;

err_report_data_init_failed:
	himax_hid_remove(ts);
	ts->hid_probed = false;
err_hid_probe_failed:
err_hid_rd_init_failed:
err_read_fw_ver:
err_tp_info_failed:
err_wrong_firmware:
err_power_on_init:
err_disable_fw_reload:
err_update_fw_failed:
err_load_bin_descriptor:
	if (!ts->hid_req_cfg.fw) {
		release_firmware(ts->himax_fw);
	} else {
		ts->hid_req_cfg.handshake_get = HIMAX_FWUP_FLASH_PROG_ERROR;
		mutex_unlock(&ts->hid_ioctl_lock);
	}
	ts->himax_fw = NULL;
}

/**
 * himax_heatmap_data_init() - Initialize the heatmap data
 * @ts: Himax touch screen data
 *
 * The function is used to initialize the heatmap data. It will allocate the
 * memory for the heatmap data. The size of the heatmap data is equal to the
 * rx_num * tx_num * 2 plus the size of the heatmap header and the size of
 * the heatmap ID.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_heatmap_data_init(struct himax_ts_data *ts)
{
	ts->heatmap_buf = devm_kzalloc(ts->dev, (ts->ic_data.rx_num * ts->ic_data.tx_num) * 2
				       + HIMAX_HEAT_MAP_INFO_SZ + HIMAX_HID_ID_SZ, GFP_KERNEL);
	if (!ts->heatmap_buf)
		return -ENOMEM;

	return 0;
}

/**
 * himax_heatmap_data_deinit() - Deinitialize the heatmap data
 * @ts: Himax touch screen data
 *
 * The function is used to deinitialize the heatmap data.
 *
 * Return: None
 */
static void himax_heatmap_data_deinit(struct himax_ts_data *ts)
{
	devm_kfree(ts->dev, ts->heatmap_buf);
	ts->heatmap_buf = NULL;
}

/**
 * himax_hid_update() - Update the HID device
 * @work: Work structure
 *
 * This function is used to update the HID device. When userspace tool switch
 * on/off the input RD, the HID device should be re-registered to update the
 * report descriptor. The heatmap size may change and need to release the old
 * one first.
 *
 * Return: None
 */
static void himax_hid_update(struct work_struct *work)
{
	struct himax_ts_data *ts = container_of(work, struct himax_ts_data, work_hid_update.work);

	himax_int_enable(ts, false);
	himax_heatmap_data_deinit(ts);
	if (!ts->hid_req_cfg.input_RD_de) {
		himax_initial_work(&ts->initial_work.work);
	} else {
		if (!himax_hid_rd_init(ts)) {
			dev_info(ts->dev, "%s: hid rd init success\n", __func__);
			himax_hid_register(ts);
			if (ts->hid_probed)
				himax_hid_report_data_init(ts);
		}
	}
	himax_int_enable(ts, true);
}

/**
 * himax_ap_notify_fw_suspend() - Notify the FW of AP suspend status
 * @ts: Himax touch screen data
 * @suspend: Suspend status, true for suspend, false for resume
 *
 * This function is used to notify the FW of AP suspend status. It will write
 * the suspend status to the DSRAM and read the status back to check if the
 * status is written successfully. If IC is powered off when suspend, this
 * function will only be used when resume.
 *
 * Return: None
 */
static void himax_ap_notify_fw_suspend(struct himax_ts_data *ts, bool suspend)
{
	int ret;
	u32 retry_cnt;
	const u32 retry_limit = 10;
	union himax_dword_data rdata, data;

	if (suspend)
		data.dword = cpu_to_le32(HIMAX_DSRAM_DATA_AP_NOTIFY_FW_SUSPEND);
	else
		data.dword = cpu_to_le32(HIMAX_DSRAM_DATA_AP_NOTIFY_FW_RESUME);

	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_AP_NOTIFY_FW_SUSPEND,
					       data.byte, 4);
		if (ret) {
			dev_err(ts->dev, "%s: write suspend status failed!\n", __func__);
			return;
		}
		usleep_range(1000, 1100);
		ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_AP_NOTIFY_FW_SUSPEND,
					      rdata.byte, 4);
		if (ret) {
			dev_err(ts->dev, "%s: read suspend status failed!\n", __func__);
			return;
		}

		if (rdata.dword == data.dword)
			break;
	}
}

/**
 * himax_resume_proc() - Chip resume procedure of touch screen
 * @ts: Himax touch screen data
 *
 * This function is used to resume the touch screen. It will call the
 * himax_zf_reload_from_file() to reload the firmware. And call the
 * himax_ap_notify_fw_suspend() to notify the FW of AP resume status.
 *
 * Return: None
 */
static void himax_resume_proc(struct himax_ts_data *ts)
{
	int ret;

	ret = himax_zf_reload_from_file(ts->firmware_name, ts);
	if (ret) {
		dev_err(ts->dev, "%s: update FW fail, code[%d]!!\n", __func__, ret);
		return;
	}
	ts->resume_succeeded = true;

	himax_ap_notify_fw_suspend(ts, false);
}

/**
 * himax_chip_suspend() - Suspend the touch screen
 * @ts: Himax touch screen data
 *
 * This function is used to suspend the touch screen. It will disable the
 * interrupt and set the reset pin to activate state. Remove the HID at
 * the end, to prevent stuck finger when resume.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_chip_suspend(struct himax_ts_data *ts)
{
	himax_int_enable(ts, false);
	gpiod_set_value(ts->pdata.gpiod_rst, 1);
	himax_power_set(ts, false);
	himax_hid_remove(ts);

	return 0;
}

/**
 * himax_chip_resume() - Setup flags, I/O and resume
 * @ts: Himax touch screen data
 *
 * This function is used to resume the touch screen. It will set the resume
 * success flag to false, and disable reset pin. Then call the himax_resume_proc()
 * to process detailed resume procedure.
 * If the resume action is succeeded, it will call the himax_hid_probe() to restore
 * the HID device and enable the interrupt.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_chip_resume(struct himax_ts_data *ts)
{
	ts->resume_succeeded = false;
	ts->excp_zero_event_count = 0;
	ts->excp_reset_active = false;
	if (himax_power_set(ts, true))
		return -EIO;
	gpiod_set_value(ts->pdata.gpiod_rst, 0);
	himax_resume_proc(ts);
	if (ts->resume_succeeded) {
		himax_hid_probe(ts);
		himax_int_enable(ts, true);
	} else {
		dev_err(ts->dev, "%s: resume failed!\n", __func__);
		return -ECANCELED;
	}

	return 0;
}

/**
 * himax_suspend() - Suspend the touch screen
 * @dev: Device structure
 *
 * Wrapper function for himax_chip_suspend() to be called by the PM or
 * the DRM panel notifier.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_suspend(struct device *dev)
{
	struct himax_ts_data *ts = dev_get_drvdata(dev);

	if (!ts->initialized) {
		dev_err(ts->dev, "%s: init not ready, skip!\n", __func__);
		return -ECANCELED;
	}
	himax_chip_suspend(ts);

	return 0;
}

/**
 * himax_resume() - Resume the touch screen
 * @dev: Device structure
 *
 * Wrapper function for himax_chip_resume() to be called by the PM or
 * the DRM panel notifier.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_resume(struct device *dev)
{
	int ret;
	struct himax_ts_data *ts = dev_get_drvdata(dev);

	if (!ts->initialized) {
		if (himax_chip_init(ts))
			return -ECANCELED;
	}

	ret = himax_chip_resume(ts);
	if (ret < 0)
		dev_err(ts->dev, "%s: resume failed!\n", __func__);

	return ret;
}

/**
 * himax_chip_init() - Initialize the Himax touch screen
 * @ts: Himax touch screen data
 *
 * This function is used to initialize the Himax touch screen. It will
 * initialize interrupt lock, register the interrupt, and disable the
 * interrupt. If later part of initialization succeed, then interrupt will
 * be enabled.
 * And initialize varies flags, workqueue and delayed work for later use.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_chip_init(struct himax_ts_data *ts)
{
	int ret;

	hx83102j_chip_init_data(ts);
	if (himax_ts_register_interrupt(ts)) {
		dev_err(ts->dev, "%s: register interrupt failed\n", __func__);
		return -EIO;
	}
	himax_int_enable(ts, false);
	ts->zf_update_cfg_buffer = devm_kzalloc(ts->dev, ts->chip_max_dsram_size, GFP_KERNEL);
	if (!ts->zf_update_cfg_buffer) {
		ret = -ENOMEM;
		goto err_update_cfg_buf_alloc_failed;
	}
	INIT_DELAYED_WORK(&ts->initial_work, himax_initial_work);
	schedule_delayed_work(&ts->initial_work, msecs_to_jiffies(HIMAX_DELAY_BOOT_UPDATE_MS));
	ts->himax_hidraw_wq =
		create_singlethread_workqueue("himax_hidraw_wq");
	if (!ts->himax_hidraw_wq) {
		dev_err(ts->dev, "%s: allocate himax_hidraw_wq failed\n", __func__);
		ret = -ENOMEM;
		goto err_hidraw_wq_failed;
	}
	INIT_DELAYED_WORK(&ts->work_self_test, himax_self_test);
	INIT_DELAYED_WORK(&ts->work_hid_update, himax_hid_update);
	ts->usb_connected = false;
	ts->initialized = true;

	return 0;
err_hidraw_wq_failed:
	cancel_delayed_work_sync(&ts->initial_work);
err_update_cfg_buf_alloc_failed:

	return ret;
}

/**
 * himax_chip_deinit() - Deinitialize the Himax touch screen
 * @ts: Himax touch screen data
 *
 * This function is used to deinitialize the Himax touch screen.
 *
 * Return: None
 */
static void himax_chip_deinit(struct himax_ts_data *ts)
{
	cancel_delayed_work_sync(&ts->work_self_test);
	destroy_workqueue(ts->himax_hidraw_wq);
	cancel_delayed_work_sync(&ts->initial_work);
}

#if defined(CONFIG_OF)
/**
 * himax_parse_dt() - Parse the device tree
 * @dt: Device node
 * @pdata: Himax platform data
 *
 * This function is used to parse the device tree. If "himax,pid" is found,
 * it will parse the PID value and set it to the platform data. The firmware
 * name will set to himax_i2chid_$PID.bin if the PID is found, or
 * himax_i2chid.bin if the PID is not found.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_parse_dt(struct device_node *dt, struct himax_platform_data *pdata)
{
	int err;
	size_t fw_name_len;
	const char *fw_name;
	struct himax_ts_data *ts;

	if (!dt || !pdata)
		return -EINVAL;

	ts = container_of(pdata, struct himax_ts_data, pdata);
	/* Set default firmware name, without PID */
	strscpy(ts->firmware_name, HIMAX_BOOT_UPGRADE_FWNAME HIMAX_FW_EXT_NAME,
		sizeof(HIMAX_BOOT_UPGRADE_FWNAME HIMAX_FW_EXT_NAME));

	if (of_property_read_bool(dt, "vccd-supply")) {
		pdata->vccd_supply = regulator_get(ts->dev, "vccd");
		if (IS_ERR(pdata->vccd_supply)) {
			dev_err(ts->dev, "%s:  DT:failed to get vccd supply\n", __func__);
			err = PTR_ERR(pdata->vccd_supply);
			pdata->vccd_supply = NULL;
			return err;
		}
		dev_info(ts->dev, "%s: DT:vccd-supply=%p\n", __func__, pdata->vccd_supply);
	} else {
		pdata->vccd_supply = NULL;
	}

	if (of_property_read_string(dt, "firmware-name", &fw_name)) {
		dev_info(ts->dev, "%s: DT:firmware-name not found\n", __func__);
	} else {
		fw_name_len = strlen(fw_name) + 1;
		strscpy(ts->firmware_name, fw_name, min(sizeof(ts->firmware_name), fw_name_len));
		dev_info(ts->dev, "%s: firmware-name = %s\n", __func__, ts->firmware_name);
	}

	return 0;
}
#endif

/**
 * __himax_initial_power_up() - Initial power up of the Himax touch screen
 * @ts: Himax touch screen data
 *
 * This function is used to perform the initial power up sequence of the Himax
 * touch screen for DRM panel notifier.
 *
 * Return: 0 on success, negative error code on failure
 */
static int __himax_initial_power_up(struct himax_ts_data *ts)
{
	int ret;

	ret = himax_platform_init(ts);
	if (ret) {
		dev_err(ts->dev, "%s: platform init failed\n", __func__);
		return ret;
	}

	ret = hx83102j_chip_detect(ts);
	if (ret) {
		dev_err(ts->dev, "%s: IC detect failed\n", __func__);
		return ret;
	}

	ret = himax_chip_init(ts);
	if (ret) {
		dev_err(ts->dev, "%s: chip init failed\n", __func__);
		return ret;
	}

	ts->himax_pwr_wq = create_singlethread_workqueue("HMX_PWR_request");
	if (!ts->himax_pwr_wq) {
		dev_err(ts->dev, "%s:  allocate himax_pwr_wq failed\n", __func__);
		ret = -ENOMEM;
		goto err_create_pwr_wq_failed;
	}

	INIT_DELAYED_WORK(&ts->work_pwr, himax_pwr_register);
	himax_pwr_register(&ts->work_pwr.work);
	ts->probe_finish = true;

	return 0;

err_create_pwr_wq_failed:
	himax_chip_deinit(ts);
	return ret;
}

/**
 * himax_panel_prepared() - Panel prepared callback
 * @follower: DRM panel follower
 *
 * This function is called when the panel is prepared. It will call the
 * __himax_initial_power_up() when the probe is not finished which means
 * the first time driver start. Otherwise, it will call the himax_resume()
 * to performed resume process.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_panel_prepared(struct drm_panel_follower *follower)
{
	struct himax_platform_data *pdata =
		container_of(follower, struct himax_platform_data, panel_follower);
	struct himax_ts_data *ts = container_of(pdata, struct himax_ts_data, pdata);

	if (!ts->probe_finish)
		return __himax_initial_power_up(ts);
	else
		return himax_resume(ts->dev);
}

/**
 * himax_panel_unpreparing() - Panel unpreparing callback
 * @follower: DRM panel follower
 *
 * This function is called when the panel is unpreparing. It will call the
 * himax_suspend() to perform the suspend process.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_panel_unpreparing(struct drm_panel_follower *follower)
{
	struct himax_platform_data *pdata =
		container_of(follower, struct himax_platform_data, panel_follower);
	struct himax_ts_data *ts = container_of(pdata, struct himax_ts_data, pdata);

	return himax_suspend(ts->dev);
}

/* Panel follower function table */
static const struct drm_panel_follower_funcs himax_panel_follower_funcs = {
	.panel_prepared = himax_panel_prepared,
	.panel_unpreparing = himax_panel_unpreparing,
};

/**
 * himax_register_panel_follower() - Register the panel follower
 * @ts: Himax touch screen data
 *
 * This function is used to register the panel follower. It will set the
 * pdata.is_panel_follower to true and register the panel follower.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_register_panel_follower(struct himax_ts_data *ts)
{
	struct device *dev = ts->dev;

	ts->pdata.is_panel_follower = true;
	ts->pdata.panel_follower.funcs = &himax_panel_follower_funcs;

	if (device_can_wakeup(dev)) {
		dev_warn(ts->dev, "Can't wakeup if following panel");
		device_set_wakeup_capable(dev, false);
	}

	return drm_panel_add_follower(dev, &ts->pdata.panel_follower);
}

/**
 * himax_initial_power_up() - Initial power up of the Himax touch screen
 * @ts: Himax touch screen data
 *
 * This function checks if the device is a panel follower and calls
 * himax_register_panel_follower() if it is. Otherwise, it calls
 * __himax_initial_power_up().
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_initial_power_up(struct himax_ts_data *ts)
{
	if (drm_is_panel_follower(ts->dev))
		return himax_register_panel_follower(ts);
	else
		return __himax_initial_power_up(ts);
}

/**
 * himax_switch_mode_inspection() - Switch the inspection mode
 * @ts: Himax touch screen data
 * @mode: Inspection mode
 *
 * This function is used to switch the inspection mode for self test.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_switch_mode_inspection(struct himax_ts_data *ts, int mode)
{
	int ret;
	union himax_dword_data data;

	/* Stop previous Handshaking first */
	data.dword = cpu_to_le32(HIMAX_DSRAM_DATA_HANDSHAKING_RELEASE);
	ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_RAWDATA, data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: write handshaking release failed\n", __func__);
		return ret;
	}

	/* Switch Mode */
	switch (mode) {
	case HIMAX_INSPECT_SORTING:
		data.dword = cpu_to_le32(HIMAX_PWD_SORTING_START);
		break;
	case HIMAX_INSPECT_OPEN:
		data.dword = cpu_to_le32(HIMAX_PWD_OPEN_START);
		break;
	case HIMAX_INSPECT_MICRO_OPEN:
		data.dword = cpu_to_le32(HIMAX_PWD_OPEN_START);
		break;
	case HIMAX_INSPECT_SHORT:
		data.dword = cpu_to_le32(HIMAX_PWD_SHORT_START);
		break;

	case HIMAX_INSPECT_RAWDATA:
		data.dword = cpu_to_le32(HIMAX_PWD_RAWDATA_START);
		break;

	case HIMAX_INSPECT_ABS_NOISE:
		data.dword = cpu_to_le32(HIMAX_PWD_NOISE_START);
		break;
	default:
		dev_err(ts->dev, "%s: Wrong mode=%d\n", __func__, mode);
		return -HIMAX_INSPECT_ESWITCHMODE;
	}
	/* Assign new sorting mode */
	ret = himax_mcu_assign_sorting_mode(ts, data.byte);
	if (ret < 0) {
		dev_err(ts->dev, "%s: assign sorting mode failed\n", __func__);
		return ret;
	}

	return 0;
}

/**
 * himax_switch_data_type() - Switch the data type
 * @ts: Himax touch screen data
 * @type: Data type
 *
 * This function is used to switch the data type for self test.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_switch_data_type(struct himax_ts_data *ts, u32 type)
{
	int ret;
	u32 datatype;

	dev_info(ts->dev, "%s: Expected type = %s\n", __func__, g_himax_inspection_mode[type]);

	switch (type) {
	case HIMAX_INSPECT_SORTING:
		datatype = g_hx_data_type[HIMAX_DATA_SORTING];
		break;
	case HIMAX_INSPECT_OPEN:
		datatype = g_hx_data_type[HIMAX_DATA_OPEN];
		break;
	case HIMAX_INSPECT_MICRO_OPEN:
		datatype = g_hx_data_type[HIMAX_DATA_MICRO_OPEN];
		break;
	case HIMAX_INSPECT_SHORT:
		datatype = g_hx_data_type[HIMAX_DATA_SHORT];
		break;
	case HIMAX_INSPECT_RAWDATA:
		datatype = g_hx_data_type[HIMAX_DATA_RAWDATA];
		break;
	case HIMAX_INSPECT_ABS_NOISE:
		datatype = g_hx_data_type[HIMAX_DATA_NOISE];
		break;
	case HIMAX_INSPECT_BACK_NORMAL:
		datatype = g_hx_data_type[HIMAX_DATA_BACK_NORMAL];
		break;
	default:
		dev_err(ts->dev, "%s: Wrong type=%d\n", __func__, type);
		return -HIMAX_INSPECT_ESWITCHDATA;
	}
	ret = himax_mcu_diag_register_set(ts, datatype);
	if (ret < 0)
		dev_err(ts->dev, "%s: set data type failed\n", __func__);

	return ret;
}

/**
 * himax_bank_search_set() - Set the bank search
 * @ts: Himax touch screen data
 * @checktype: Inspection mode
 *
 * This function is used to set the bank search for self test. This register
 * is combined with other function, bank search used LSB 8 bits. So need to
 * read the register first, then set the bank search and write it back.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_bank_search_set(struct himax_ts_data *ts, u32 checktype)
{
	int ret;
	union himax_dword_data data;

	ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_BANK_SEARCH, data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read bank search failed\n", __func__);
		return ret;
	}

	switch (checktype) {
	case HIMAX_INSPECT_RAWDATA:
		data.byte[0] = HIMAX_BANK_SEARCH_RAWDATA;
		break;
	case HIMAX_INSPECT_ABS_NOISE:
		data.byte[0] = HIMAX_BANK_SEARCH_NOISE;
		break;
	default:
		data.byte[0] = HIMAX_BANK_SEARCH_OPENSHORT;
	}

	ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_BANK_SEARCH, data.byte, 4);
	if (ret < 0)
		dev_err(ts->dev, "%s: write bank search failed\n", __func__);

	return ret;
}

/**
 * himax_set_N_frame() - Set the N frame to skip for self test
 * @ts: Himax touch screen data
 * @n_frame: N frame value
 * @checktype: Inspection mode
 *
 * This function is used to set the N frame to skip for self test. It will
 * set the bank search first, then write the N frame to the register. The
 * N frame is used to skip the frame for self test when collecting the data
 * after switch the mode.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_set_N_frame(struct himax_ts_data *ts, u32 n_frame, u32 checktype)
{
	int ret;
	union himax_dword_data data;

	ret = himax_bank_search_set(ts, checktype);
	if (ret < 0) {
		dev_err(ts->dev, "%s: set bank search failed\n", __func__);
		return ret;
	}

	data.dword = cpu_to_le32(n_frame);
	ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_SET_NFRAME, data.byte, 4);
	if (ret < 0)
		dev_err(ts->dev, "%s: write N frame failed\n", __func__);

	return ret;
}

/**
 * himax_check_mode() - Check the inspection mode
 * @ts: Himax touch screen data
 * @checktype: Inspection mode
 *
 * This function is used to check the inspection mode for self test. It will
 * read the sorting mode register and compare it with the expected value.
 * If the mode is not matched, it will return
 * HIMAX_INSPECT_CHANGE_MODE_REQUIRED, otherwise return 0 indicate the mode
 * is matched.
 *
 * Return: 0 on mode matched, HIMAX_INSPECT_CHANGE_MODE_REQUIRED when mode
 * change required, negative error code on failure
 */
static int himax_check_mode(struct himax_ts_data *ts, u32 checktype)
{
	int ret;
	const u16 passwd_mask = GENMASK(15, 0);
	u16 wait_pwd;
	union himax_dword_data data;

	switch (checktype) {
	case HIMAX_INSPECT_SORTING:
		wait_pwd = cpu_to_le16(HIMAX_PWD_SORTING_END);
		break;
	case HIMAX_INSPECT_OPEN:
		wait_pwd = cpu_to_le16(HIMAX_PWD_OPEN_END);
		break;
	case HIMAX_INSPECT_MICRO_OPEN:
		wait_pwd = cpu_to_le16(HIMAX_PWD_OPEN_END);
		break;
	case HIMAX_INSPECT_SHORT:
		wait_pwd = cpu_to_le16(HIMAX_PWD_SHORT_END);
		break;
	case HIMAX_INSPECT_RAWDATA:
		wait_pwd = cpu_to_le16(HIMAX_PWD_RAWDATA_END);
		break;

	case HIMAX_INSPECT_ABS_NOISE:
		wait_pwd = cpu_to_le16(HIMAX_PWD_NOISE_END);
		break;

	default:
		dev_err(ts->dev, "%s: Wrong type = %u\n", __func__, checktype);
		return -HIMAX_INSPECT_ESWITCHMODE;
	}

	ret = himax_mcu_check_sorting_mode(ts, data.byte);
	if (ret)
		return ret;

	if ((le32_to_cpu(data.dword) & passwd_mask) == wait_pwd) {
		dev_info(ts->dev, "%s: Current sorting mode: %s\n", __func__,
			 g_himax_inspection_mode[checktype]);
		return 0;
	} else {
		return HIMAX_INSPECT_CHANGE_MODE_REQUIRED;
	}
}

/**
 * himax_wait_sorting_mode() - Wait the inspection mode
 * @ts: Himax touch screen data
 * @checktype: Inspection mode
 *
 * This function is used to wait the inspection mode for self test. It will
 * read current sorting mode for most 10 times each interval 50ms to check
 * if the target mode is reached.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_wait_sorting_mode(struct himax_ts_data *ts, u32 checktype)
{
	const u16 passwd_mask = GENMASK(15, 0);
	u16 wait_pwd;
	u32 retry_cnt;
	const u32 retry_limit = 10;
	union himax_dword_data data;

	switch (checktype) {
	case HIMAX_INSPECT_SORTING:
		wait_pwd = cpu_to_le16(HIMAX_PWD_SORTING_END);
		break;
	case HIMAX_INSPECT_OPEN:
		wait_pwd = cpu_to_le16(HIMAX_PWD_OPEN_END);
		break;
	case HIMAX_INSPECT_MICRO_OPEN:
		wait_pwd = cpu_to_le16(HIMAX_PWD_OPEN_END);
		break;
	case HIMAX_INSPECT_SHORT:
		wait_pwd = cpu_to_le16(HIMAX_PWD_SHORT_END);
		break;
	case HIMAX_INSPECT_RAWDATA:
		wait_pwd = cpu_to_le16(HIMAX_PWD_RAWDATA_END);
		break;
	case HIMAX_INSPECT_ABS_NOISE:
		wait_pwd = cpu_to_le16(HIMAX_PWD_NOISE_END);
		break;

	default:
		dev_warn(ts->dev, "%s: No target type = %u\n", __func__, checktype);
		return -HIMAX_INSPECT_ESWITCHMODE;
	}
	dev_info(ts->dev, "%s: NowType: %s, Expected = 0x%04X\n", __func__,
		 g_himax_inspection_mode[checktype], wait_pwd);
	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		if (himax_mcu_check_sorting_mode(ts, data.byte))
			return -HIMAX_INSPECT_ESWITCHMODE;
		if ((le32_to_cpu(data.dword) & passwd_mask) == wait_pwd)
			return 0;
		usleep_range(50000, 50100);
	}

	return -HIMAX_INSPECT_ESWITCHMODE;
}

/**
 * himax_self_test() - Self test work function
 * @work: Work structure
 *
 * This function is used to perform the self test. It will check current
 * mode, switch mode if required, wait the sorting mode, switch data type,
 * set N frame, and get the data. The switch mode step may take more time
 * by circustance, so it will retry 3 times if it failed. Wheather the
 * self test is finished or failed, it will unlock the hid_ioctl_lock.
 *
 * Return: None
 */
static void himax_self_test(struct work_struct *work)
{
	int ret, n_frame;
	int switch_mode_cnt = 0;
	const int switch_mode_retry_limit = 3;
	struct himax_ts_data *ts =
		container_of(work, struct himax_ts_data, work_self_test.work);
	u32 checktype = ts->hid_req_cfg.self_test_type;

	ret = himax_check_mode(ts, checktype);
	if (ret < 0) {
		ts->hid_req_cfg.handshake_get = HIMAX_HID_SELF_TEST_ERROR;
		goto END;
	}

	if (ret == HIMAX_INSPECT_CHANGE_MODE_REQUIRED) {
		dev_info(ts->dev, "%s: Need Change Mode, target = %s\n", __func__,
			 g_himax_inspection_mode[checktype]);
SWITCH_MODE:
		ret = hx83102j_sense_off(ts, true);
		if (ret) {
			ts->hid_req_cfg.handshake_get = HIMAX_HID_SELF_TEST_ERROR;
			goto END;
		}

		ret = himax_switch_mode_inspection(ts, checktype);
		if (ret) {
			dev_err(ts->dev, "%s: switch mode failed!\n", __func__);
			ts->hid_req_cfg.handshake_get = HIMAX_HID_SELF_TEST_ERROR;
			goto END;
		}

		if (checktype == HIMAX_INSPECT_ABS_NOISE)
			n_frame = cpu_to_le32(HIMAX_NFRAME_NOISE);
		else
			n_frame = cpu_to_le32(HIMAX_NFRAME_OTHER);

		ret = himax_set_N_frame(ts, n_frame, checktype);
		if (ret) {
			dev_err(ts->dev, "%s: set N frame failed!\n", __func__);
			ts->hid_req_cfg.handshake_get = HIMAX_HID_SELF_TEST_ERROR;
			goto END;
		}
		ret = hx83102j_sense_on(ts, true);
		if (ret) {
			dev_err(ts->dev, "%s: sense on failed!\n", __func__);
			ts->hid_req_cfg.handshake_get = HIMAX_HID_SELF_TEST_ERROR;
			goto END;
		}
	}

	ret = himax_wait_sorting_mode(ts, checktype);
	if (ret) {
		if (ret == -HIMAX_INSPECT_ESWITCHMODE &&
		    switch_mode_cnt < switch_mode_retry_limit) {
			switch_mode_cnt++;
			himax_mcu_ic_reset(ts, false);
			goto SWITCH_MODE;
		}
		dev_err(ts->dev, "%s: Wait sorting mode timeout\n", __func__);
		ts->hid_req_cfg.handshake_get = HIMAX_HID_SELF_TEST_ERROR;
		goto END;
	}
	ret = himax_switch_data_type(ts, checktype);
	if (ret) {
		dev_err(ts->dev, "%s: switch data type failed\n", __func__);
		ts->hid_req_cfg.handshake_get = HIMAX_HID_SELF_TEST_ERROR;
		goto END;
	}

	ts->hid_req_cfg.handshake_get = HIMAX_HID_SELF_TEST_FINISH;
END:
	mutex_unlock(&ts->hid_ioctl_lock);
}

/**
 * himax_get_data() - Get the self test result data
 * @ts: Himax touch screen data
 * @data: Data buffer
 *
 * This function is used to get the self test result data.
 *
 * Return: HIMAX_INSPECT_OK on success, -HIMAX_INSPECT_EGETRAW on failure
 */
static int himax_get_data(struct himax_ts_data *ts, u8 *data)
{
	int get_raw_rlst;

	get_raw_rlst = himax_mcu_get_DSRAM_data(ts, data);
	if (!get_raw_rlst)
		return HIMAX_INSPECT_OK;
	else
		return -HIMAX_INSPECT_EGETRAW;
}

/**
 * himax_platform_deinit() - Deinitialize the platform related settings
 * @ts: Pointer to the himax_ts_data structure
 *
 * This function deinitializes the platform related settings, frees the
 * xfer_buf.
 *
 * Return: None
 */
static void himax_platform_deinit(struct himax_ts_data *ts)
{
	struct himax_platform_data *pdata = &ts->pdata;

	if (ts->xfer_buf_sz) {
		kfree(ts->xfer_buf);
		ts->xfer_buf = NULL;
		ts->xfer_buf_sz = 0;
	}
	himax_power_deconfig(pdata);
}

/**
 * himax_platform_init - Initialize the platform related settings
 * @ts: Pointer to the himax_ts_data structure
 *
 * This function initializes the platform related settings.
 * The xfer_buf is used for interrupt data receive. gpio reset pin is set to
 * active and then deactivate to reset the IC.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_platform_init(struct himax_ts_data *ts)
{
	int ret;
	struct himax_platform_data *pdata = &ts->pdata;

	ts->xfer_buf_sz = 0;
	ts->xfer_buf = kzalloc(HIMAX_HX83102J_FULL_STACK_SZ, GFP_KERNEL);
	if (!ts->xfer_buf)
		return -ENOMEM;
	ts->xfer_buf_sz = HIMAX_HX83102J_FULL_STACK_SZ;

	gpiod_set_value(pdata->gpiod_rst, 1);
	ret = himax_power_set(ts, true);
	if (ret) {
		dev_err(ts->dev, "%s: gpio power config failed\n", __func__);
		return ret;
	}

	usleep_range(2000, 2100);
	gpiod_set_value(pdata->gpiod_rst, 0);

	return 0;
}

/**
 * himax_spi_drv_probe - Probe function for the SPI driver
 * @spi: Pointer to the spi_device structure
 *
 * This function is called when the SPI driver is probed. It initializes the
 * himax_ts_data structure and assign the settings from spi device to
 * himax_ts_data. The buffer for SPI transfer is allocate here. The SPI
 * transfer settings also setup before any communication starts.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_spi_drv_probe(struct spi_device *spi)
{
	int ret;
	struct himax_ts_data *ts;
	static struct himax_platform_data *pdata;

	dev_info(&spi->dev, "%s: Himax SPI driver probe\n", __func__);
	ts = devm_kzalloc(&spi->dev, sizeof(struct himax_ts_data), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	if (spi->master->flags & SPI_MASTER_HALF_DUPLEX) {
		dev_err(ts->dev, "%s: Full duplex not supported by host\n", __func__);
		return -EIO;
	}
	pdata = &ts->pdata;
	ts->dev = &spi->dev;
	if (!spi->irq) {
		dev_err(ts->dev, "%s: no IRQ?\n", __func__);
		return -EINVAL;
	}
	ts->himax_irq = spi->irq;
	pdata->gpiod_rst = devm_gpiod_get(ts->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pdata->gpiod_rst)) {
		dev_err(ts->dev, "%s: gpio-rst value is not valid\n", __func__);
		return -EIO;
	}
#if defined(CONFIG_OF)
	if (himax_parse_dt(spi->dev.of_node, pdata) < 0) {
		dev_err(ts->dev, "%s:  parse OF data failed!\n", __func__);
		ts->dev = NULL;
		return -ENODEV;
	}
#endif

	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_3;
	spi->cs_setup.value = HIMAX_SPI_CS_SETUP_TIME;

	ts->spi = spi;
	/*
	 * The max_transfer_size is used to allocate the buffer for SPI transfer.
	 * The size should be given by the SPI master driver, but if not available
	 * then use the HIMAX_MAX_TP_EV_STACK_SZ as default. Which is the least size for
	 * each TP event data.
	 */
	if (spi->master->max_transfer_size)
		ts->spi_xfer_max_sz = spi->master->max_transfer_size(spi);
	else
		ts->spi_xfer_max_sz = HIMAX_MAX_TP_EV_STACK_SZ;

	ts->spi_xfer_max_sz = min(ts->spi_xfer_max_sz, HIMAX_BUS_RW_MAX_LEN);
	/* SPI full-duplex rx_buf and tx_buf should be equal */
	ts->xfer_rx_data = devm_kzalloc(ts->dev, ts->spi_xfer_max_sz, GFP_KERNEL);
	if (!ts->xfer_rx_data)
		return -ENOMEM;

	ts->xfer_tx_data = devm_kzalloc(ts->dev, ts->spi_xfer_max_sz, GFP_KERNEL);
	if (!ts->xfer_tx_data)
		return -ENOMEM;

	spin_lock_init(&ts->irq_lock);
	mutex_init(&ts->rw_lock);
	mutex_init(&ts->reg_lock);
	mutex_init(&ts->hid_ioctl_lock);
	mutex_init(&ts->zf_update_lock);
	dev_set_drvdata(&spi->dev, ts);
	spi_set_drvdata(spi, ts);

	ts->probe_finish = false;
	ts->initialized = false;
	ts->ic_boot_done = false;

	ret = himax_initial_power_up(ts);
	if (ret) {
		dev_err(ts->dev, "%s: initial power up failed\n", __func__);
		return -ENODEV;
	}

	return ret;
}

/**
 * himax_spi_drv_remove - Remove function for the SPI driver
 * @spi: Pointer to the spi_device structure
 *
 * This function is called when the SPI driver is removed. It deinitializes the
 * himax_ts_data structure and free the resources allocated for the SPI
 * communication.
 */
static void himax_spi_drv_remove(struct spi_device *spi)
{
	struct himax_ts_data *ts = spi_get_drvdata(spi);

	if (ts->probe_finish) {
		if (ts->ic_boot_done) {
			himax_int_enable(ts, false);

			if (ts->hid_probed)
				himax_hid_remove(ts);
		}
		power_supply_unreg_notifier(&ts->power_notif);
		cancel_delayed_work_sync(&ts->work_pwr);
		destroy_workqueue(ts->himax_pwr_wq);

		himax_chip_deinit(ts);
		himax_platform_deinit(ts);
	}
}

/**
 * himax_shutdown - Shutdown the touch screen
 * @spi: Himax touch screen spi device
 *
 * This function is used to shutdown the touch screen. It will disable the
 * interrupt, set the reset pin to activate state. Then remove the hid device.
 *
 * Return: None
 */
static void himax_shutdown(struct spi_device *spi)
{
	struct himax_ts_data *ts = spi_get_drvdata(spi);

	if (!ts->initialized) {
		dev_err(ts->dev, "%s: init not ready, skip!\n", __func__);
		return;
	}

	himax_int_enable(ts, false);
	gpiod_set_value(ts->pdata.gpiod_rst, 1);
	himax_power_deconfig(&ts->pdata);
	himax_hid_remove(ts);
}

#if defined(CONFIG_OF)
static const struct of_device_id himax_table[] = {
	{ .compatible = "himax,hx83102j" },
	{},
};
MODULE_DEVICE_TABLE(of, himax_table);
#endif

static struct spi_driver himax_hid_over_spi_driver = {
	.driver = {
		.name =		"hx83102j",
		.owner =	THIS_MODULE,
#if defined(CONFIG_OF)
		.of_match_table = of_match_ptr(himax_table),
#endif
	},
	.probe =	himax_spi_drv_probe,
	.remove =	himax_spi_drv_remove,
	.shutdown =	himax_shutdown,
};

static int __init himax_ic_init(void)
{
	return spi_register_driver(&himax_hid_over_spi_driver);
}

static void __exit himax_ic_exit(void)
{
	spi_unregister_driver(&himax_hid_over_spi_driver);
}

module_init(himax_ic_init);
module_exit(himax_ic_exit);

MODULE_VERSION("1.3.4");
MODULE_DESCRIPTION("Himax HX83102J SPI driver for HID");
MODULE_AUTHOR("Himax, Inc.");
MODULE_LICENSE("GPL");
