// SPDX-License-Identifier: GPL-2.0
/*
 * Himax hx83102j SPI Driver Code for HID.
 *
 * Copyright (C) 2024 Himax Corporation.
 */

#include "hid-himax.h"

static int himax_chip_init(struct himax_ts_data *ts);
static int himax_platform_init(struct himax_ts_data *ts);
static void himax_ts_work(struct himax_ts_data *ts);

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
 *
 * Return: None
 */
static void hx83102j_chip_init_data(struct himax_ts_data *ts)
{
	ts->chip_max_dsram_size = HIMAX_HX83102J_DSRAM_SZ;
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

	return 0;
}

/**
 * himax_bin_desc_data_get() - Parse descriptor data from firmware token
 * @ts: Himax touch screen data
 * @addr: Address of the data in firmware image
 * @descript_buf: token for parsing
 *
 * This function is used to parse the descriptor data from the firmware token. The
 * descriptors are mappings of information in the firmware image. The function will
 * check checksum of each token first, and then parse the token to get the related
 * data. The data includes CID version, FW version, CFG version, touch config table,
 * HID table, HID descriptor, and HID read descriptor.
 *
 * Return: true on success, false on failure
 */
static bool himax_bin_desc_data_get(struct himax_ts_data *ts, u32 addr, u8 *descript_buf)
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
		keep_on_flag = himax_bin_desc_data_get(ts, addr, fw_buf);
		if (keep_on_flag)
			mapping_count++;
		else
			break;
	}

	return mapping_count > 0;
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
 * Case not listed here will return -EINVAL.
 *
 * Return: The length of the data in the buf on success, negative error code
 */
static int himax_hid_get_raw_report(const struct hid_device *hid,
				    unsigned char reportnum, __u8 *buf,
				    size_t len, unsigned char report_type)
{
	int ret;
	struct himax_ts_data *ts;

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
	default:
		dev_err(ts->dev, "%s: Invalid report number\n", __func__);
		ret = -EINVAL;
		break;
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
 *
 * Return: None
 */
static void himax_hid_remove(struct himax_ts_data *ts)
{
	if (ts && ts->hid)
		hid_destroy_device(ts->hid);
	else
		return;

	ts->hid = NULL;
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
		offset = ts->hid_desc.max_input_length;
		if (ts->ic_data.stylus_function) {
			ret += himax_hid_report(ts,
						ts->xfer_buf + offset + HIMAX_HID_REPORT_HDR_SZ,
						ts->hid_desc.max_input_length -
						HIMAX_HID_REPORT_HDR_SZ);
			offset += ts->hid_desc.max_input_length;
		}
	}

	if (ret != 0)
		return HIMAX_TS_GET_DATA_FAIL;

	return HIMAX_TS_SUCCESS;
}

/**
 * himax_ts_work() - Work function for the touch screen
 * @ts: Himax touch screen data
 *
 * This function is used to handle interrupt bottom half work. It will
 * call the himax_ts_operation() to get the touch data, dispatch the data
 * to HID core. If the touch data is not valid, it will reset the TPIC.
 * It will also call the hx83102j_reload_to_active() after the reset action.
 *
 * Return: void
 */
static void himax_ts_work(struct himax_ts_data *ts)
{
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

	/* The rd_sz is taken from RD size in FW hid report table. */
	rd_sz = ts->hid_desc.report_desc_length;
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
		memcpy((void *)ts->hid_rd_data.rd_data,
		       &ts->himax_fw->data[ts->fw_info_table.addr_hid_rd_desc],
		       ts->hid_desc.report_desc_length);
		ts->hid_rd_data.rd_length = ts->hid_desc.report_desc_length;
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
 * If the size of the HID report data is not equal to the previous size, it
 * will free the previous allocated memory and allocate the new memory which
 * size is equal to the final size of touch_data_sz.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_hid_report_data_init(struct himax_ts_data *ts)
{
	ts->touch_data_sz = ts->hid_desc.max_input_length;
	if (ts->ic_data.stylus_function)
		ts->touch_data_sz += ts->hid_desc.max_input_length;
	if (ts->touch_data_sz != ts->xfer_buf_sz) {
		kfree(ts->xfer_buf);
		ts->xfer_buf_sz = 0;
		ts->xfer_buf = kzalloc(ts->touch_data_sz, GFP_KERNEL);
		if (!ts->xfer_buf)
			return -ENOMEM;
		ts->xfer_buf_sz = ts->touch_data_sz;
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
	dev_info(ts->dev, "%s: request file %s\n", __func__, ts->firmware_name);
	ret = request_firmware(&ts->himax_fw, ts->firmware_name, ts->dev);
	if (ret < 0) {
		dev_err(ts->dev, "%s: request firmware failed, error code = %d\n", __func__, ret);
		return;
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
	release_firmware(ts->himax_fw);
	ts->himax_fw = NULL;
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
	ts->initialized = true;

	return 0;
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
	ts->probe_finish = true;

	return 0;
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
