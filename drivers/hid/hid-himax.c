// SPDX-License-Identifier: GPL-2.0
/*
 * Himax hx83102j SPI Driver Code for HID.
 *
 * Copyright (C) 2024 Himax Corporation.
 */

#include "hid-himax.h"

static int himax_chip_init(struct himax_ts_data *ts);
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
		dev_warn(ts->dev, "%s: hid device already exist!\n", __func__);
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
 * himax_ts_operation() - Process the touch interrupt data
 * @ts: Himax touch screen data
 *
 * This function is used to process the touch interrupt data. It will
 * call the himax_touch_get() to get the touch data.
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
 *
 * Return: void
 */
static void himax_ts_work(struct himax_ts_data *ts)
{
	if (himax_ts_operation(ts) == HIMAX_TS_GET_DATA_FAIL) {
		dev_info(ts->dev, "%s: Now reset the Touch chip\n", __func__);
		himax_mcu_ic_reset(ts, true);
	}
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
		/* Copy the base RD from firmware table */
		memcpy((void *)ts->hid_rd_data.rd_data,
		       &ts->himax_fw_data[ts->fw_info_table.addr_hid_rd_desc],
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

/* load firmware data from flash, parse HID info and register HID */
/**
 * himax_load_config() - Load the firmware from the flash
 * @ts: Himax touch screen data
 *
 * This function is used to load the firmware from the flash. It will read
 * the firmware from the flash and parse the HID info. If the HID info is
 * valid, it will initialize the HID report descriptor and register the HID
 * device. If the HID device is probed, it will initialize the report data
 * and enable the interrupt.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_load_config(struct himax_ts_data *ts)
{
	int ret;
	s32 i;
	s32 page_sz = (s32)HIMAX_HX83102J_PAGE_SIZE;
	s32 flash_sz = (s32)HIMAX_HX83102J_FLASH_SIZE;
	bool fw_load_status = false;
	const u32 fw_bin_header_sz = 1024;

	ts->ic_boot_done = false;

	ts->himax_fw_data = devm_kzalloc(ts->dev, HIMAX_HX83102J_FLASH_SIZE, GFP_KERNEL);
	if (!ts->himax_fw_data)
		return -ENOMEM;

	for (i = 0; i < flash_sz; i += page_sz) {
		ret = himax_mcu_register_read(ts, i, ts->himax_fw_data + i,
					      (flash_sz - i) > page_sz ? page_sz : (flash_sz - i));
		if (ret < 0) {
			dev_err(ts->dev, "%s: read FW from flash fail!\n", __func__);
			return ret;
		}
	}
	/* Search mapping table in 1k header */
	fw_load_status = himax_mcu_bin_desc_get((unsigned char *)ts->himax_fw_data,
						ts, fw_bin_header_sz);
	if (!fw_load_status) {
		dev_err(ts->dev, "%s: FW load status fail!\n", __func__);
		return -EINVAL;
	}

	if (ts->fw_info_table.addr_hid_desc != 0) {
		memcpy(&ts->hid_desc,
		       &ts->himax_fw_data[ts->fw_info_table.addr_hid_desc],
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
	}

	if (himax_hid_rd_init(ts)) {
		dev_err(ts->dev, "%s: hid rd init fail\n", __func__);
		goto err_hid_rd_init_failed;
	}

	himax_hid_register(ts);
	if (!ts->hid_probed) {
		goto err_hid_probe_failed;
	} else {
		if (himax_hid_report_data_init(ts)) {
			dev_err(ts->dev, "%s: report data init fail\n", __func__);
			goto err_report_data_init_failed;
		}
	}

	ts->himax_fw_data = NULL;
	ts->ic_boot_done = true;
	himax_int_enable(ts, true);

	return 0;

err_report_data_init_failed:
	himax_hid_remove(ts);
	ts->hid_probed = false;
err_hid_probe_failed:
err_hid_rd_init_failed:

	return -EINVAL;
}

/**
 * himax_chip_init() - Initialize the Himax touch screen
 * @ts: Himax touch screen data
 *
 * This function is used to initialize the Himax touch screen. It will
 * initialize interrupt lock, register the interrupt, and disable the
 * interrupt. If later part of initialization succeed, then interrupt will
 * be enabled.
 * It will also load the firmware from the flash, parse the HID info, and
 * register the HID device by calling the himax_load_config().
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_chip_init(struct himax_ts_data *ts)
{
	int ret;

	if (himax_ts_register_interrupt(ts)) {
		dev_err(ts->dev, "%s: register interrupt failed\n", __func__);
		return -EIO;
	}
	himax_int_enable(ts, false);
	ret = himax_load_config(ts);
	if (ret < 0)
		return ret;
	ts->initialized = true;

	return 0;
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
	dev_set_drvdata(&spi->dev, ts);
	spi_set_drvdata(spi, ts);

	ts->probe_finish = false;
	ts->initialized = false;
	ts->ic_boot_done = false;

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
	if (ret < 0)
		return ret;
	ts->probe_finish = true;

	return ret;
	himax_platform_deinit(ts);
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
