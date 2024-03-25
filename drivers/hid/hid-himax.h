/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Himax hx83102j SPI Driver Code for HID.
 *
 * Copyright (C) 2024 Himax Corporation.
 */

#ifndef __HID_HIMAX_83102J_H__
#define __HID_HIMAX_83102J_H__

#include <linux/delay.h>
#include <linux/hid.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

/* Constants */
#define HIMAX_BUS_RETRY					3
/* SPI bus read header length */
#define HIMAX_BUS_R_HLEN				3U
/* SPI bus write header length */
#define HIMAX_BUS_W_HLEN				2U
/* TP SRAM address size and data size */
#define HIMAX_REG_SZ					4U
/* TP MAX TX/RX number */
#define HIMAX_MAX_RX					144U
#define HIMAX_MAX_TX					48U
/* TP FW main code size */
#define HIMAX_FW_MAIN_CODE_SZ				0x20000U
/* TP max event data size */
#define HIMAX_MAX_TP_EVENT_SZ				128U
/* TP max interrupt data size : event size + raw data size(rx * tx 16bits) */
#define HIMAX_MAX_TP_EV_STACK_SZ			(HIMAX_BUS_R_HLEN + \
							 HIMAX_MAX_TP_EVENT_SZ + \
							 (HIMAX_MAX_RX * HIMAX_MAX_TX) * 2)
/* SPI Max read/write size for max possible usage, write FW main code */
#define HIMAX_BUS_RW_MAX_LEN				(HIMAX_FW_MAIN_CODE_SZ + \
							 HIMAX_BUS_W_HLEN + HIMAX_REG_SZ)
/* SPI CS setup time */
#define HIMAX_SPI_CS_SETUP_TIME				300
/* HIDRAW report header size */
#define HIMAX_HID_REPORT_HDR_SZ				2U
/* hx83102j IC parameters */
#define HIMAX_HX83102J_DSRAM_SZ				73728U
#define HIMAX_HX83102J_FLASH_SIZE			261120U
#define HIMAX_HX83102J_MAX_RX_NUM			50U
#define HIMAX_HX83102J_MAX_TX_NUM			32U
#define HIMAX_HX83102J_PAGE_SIZE			128U
#define HIMAX_HX83102J_SAFE_MODE_PASSWORD		0x9527
#define HIMAX_HX83102J_STACK_SIZE			128U
#define HIMAX_HX83102J_FULL_STACK_SZ \
	(HIMAX_HX83102J_STACK_SIZE + \
	 (2 + HIMAX_HX83102J_MAX_RX_NUM * HIMAX_HX83102J_MAX_TX_NUM + \
	 HIMAX_HX83102J_MAX_TX_NUM + HIMAX_HX83102J_MAX_RX_NUM) * 2)
#define HIMAX_HX83102J_REG_XFER_MAX			4096U
/* AHB register addresses */
#define HIMAX_AHB_ADDR_BYTE_0				0x00
#define HIMAX_AHB_ADDR_RDATA_BYTE_0			0x08
#define HIMAX_AHB_ADDR_ACCESS_DIRECTION			0x0c
#define HIMAX_AHB_ADDR_INCR4				0x0d
#define HIMAX_AHB_ADDR_CONTI				0x13
#define HIMAX_AHB_ADDR_EVENT_STACK			0x30
#define HIMAX_AHB_ADDR_PSW_LB				0x31
/* AHB register values/commands */
#define HIMAX_AHB_CMD_ACCESS_DIRECTION_READ		0x00
#define HIMAX_AHB_CMD_CONTI				0x31
#define HIMAX_AHB_CMD_INCR4				0x10
#define HIMAX_AHB_CMD_INCR4_ADD_4_BYTE			0x01
#define HIMAX_AHB_CMD_LEAVE_SAFE_MODE			0x0000
/* hx83102j-specific register/dsram flags/data */
#define HIMAX_HX83102J_REG_ADDR_TCON_RST		0x80020004
/* hardware register addresses */
#define HIMAX_REG_ADDR_SPI200_DATA			0x8000002c
#define HIMAX_REG_ADDR_CTRL_FW				0x9000005c
#define HIMAX_REG_ADDR_FW_STATUS			0x900000a8
#define HIMAX_REG_ADDR_ICID				0x900000d0
/* hardware reg data/flags */
#define HIMAX_REG_DATA_FW_STATE_RUNNING			0x05
#define HIMAX_REG_DATA_FW_STATE_SAFE_MODE		0x0c
#define HIMAX_REG_DATA_FW_RE_INIT			0x00
#define HIMAX_REG_DATA_FW_GO_SAFEMODE			0xa5
#define HIMAX_REG_DATA_FW_IN_SAFEMODE			0x87
#define HIMAX_REG_DATA_ICID				0x83102900
#define HIMAX_REG_DATA_TCON_RST				0x00000000
/* HIMAX SPI function select, 1st byte of any SPI command sequence */
#define HIMAX_SPI_FUNCTION_READ				0xf3
#define HIMAX_SPI_FUNCTION_WRITE			0xf2
/* Map code of FW 1k header */
#define HIMAX_TP_CONFIG_TABLE				0x00000a00
#define HIMAX_FW_CID					0x10000000
#define HIMAX_FW_VER					0x10000100
#define HIMAX_CFG_VER					0x10000600
#define HIMAX_HID_TABLE					0x30000100
#define HIMAX_FW_BIN_DESC				0x10000000

/**
 * enum himax_hidraw_id_function - HIDRAW report IDs
 * @HIMAX_ID_CONTACT_COUNT: Contact count report ID
 */
enum himax_hidraw_id_function {
	HIMAX_ID_CONTACT_COUNT = 0x03,
};

/**
 * enum himax_touch_report_status - ts operation return code for touch report
 * @HIMAX_TS_GET_DATA_FAIL: Get touch data fail
 * @HIMAX_TS_SUCCESS: Get touch data success
 */
enum himax_touch_report_status {
	HIMAX_TS_GET_DATA_FAIL = -4,
	HIMAX_TS_SUCCESS = 0,
};

/**
 * struct himax_fw_address_table - address/offset in firmware image
 * @addr_fw_ver_major: Address to Major version of firmware
 * @addr_fw_ver_minor: Address to Minor version of firmware
 * @addr_cfg_ver_major: Address to Major version of config
 * @addr_cfg_ver_minor: Address to Minor version of config
 * @addr_cid_ver_major: Address to Major version of Customer ID
 * @addr_cid_ver_minor: Address to Minor version of Customer ID
 * @addr_cfg_table: Address to Configuration table
 * @addr_hid_table: Address to HID tables start offset
 * @addr_hid_desc: Address to HID descriptor table
 * @addr_hid_rd_desc: Address to HID report descriptor table
 */
struct himax_fw_address_table {
	u32 addr_fw_ver_major;
	u32 addr_fw_ver_minor;
	u32 addr_cfg_ver_major;
	u32 addr_cfg_ver_minor;
	u32 addr_cid_ver_major;
	u32 addr_cid_ver_minor;
	u32 addr_cfg_table;
	u32 addr_hid_table;
	u32 addr_hid_desc;
	u32 addr_hid_rd_desc;
};

/**
 * struct himax_hid_rd_data - HID report descriptor data
 * @rd_data: Point to report description data
 * @rd_length: Length of report description data
 */
struct himax_hid_rd_data {
	u8 *rd_data;
	u32 rd_length;
};

/**
 * union himax_dword_data - 4 bytes data union
 * @dword: 1 dword data
 * @word: 2 words data in word array
 * @byte: 4 bytes data in byte array
 */
union himax_dword_data {
	u32 dword;
	u16 word[2];
	u8 byte[4];
};

/**
 * struct himax_ic_data - IC information holder
 * @stylus_ratio: Stylus ratio
 * @rx_num: Number of RX
 * @tx_num: Number of TX
 * @button_num: Number of buttons
 * @max_point: Maximum touch point
 * @icid: IC ID
 * @interrupt_is_edge: Interrupt is edge otherwise level
 * @stylus_function: Stylus function available or not
 * @stylus_v2: Is stylus version 2
 */
struct himax_ic_data {
	u8 stylus_ratio;
	u32 rx_num;
	u32 tx_num;
	u32 button_num;
	u32 max_point;
	u32 icid;
	bool interrupt_is_edge;
	bool stylus_function;
	bool stylus_v2;
};

/**
 * struct himax_hid_desc - HID descriptor
 * @desc_length: Length of HID descriptor
 * @bcd_version: BCD version
 * @report_desc_length: Length of report descriptor
 * @max_input_length: Maximum input length
 * @max_output_length: Maximum output length
 * @max_fragment_length: Maximum fragment length
 * @vendor_id: Vendor ID
 * @product_id: Product ID
 * @version_id: Version ID
 * @flags: Flags
 * @reserved: Reserved
 *
 * This structure is used to hold the HID descriptor.
 * It directly maps to a sequence of bytes in firmware image,
 * thus need to be packed.
 */
struct himax_hid_desc {
	u16 desc_length;
	u16 bcd_version;
	u16 report_desc_length;
	u16 max_input_length;
	u16 max_output_length;
	u16 max_fragment_length;
	u16 vendor_id;
	u16 product_id;
	u16 version_id;
	u16 flags;
	u32 reserved;
} __packed;

/**
 * struct himax_platform_data - Platform data holder
 * @vccd_supply: VCCD supply
 * @gpiod_rst: GPIO reset
 *
 * This structure is used to hold the platform related data.
 */
struct himax_platform_data {
	struct regulator *vccd_supply;
	struct gpio_desc *gpiod_rst;
};

/**
 * struct himax_ts_data - Touchscreen data holder
 * @xfer_buf: Interrupt data buffer
 * @xfer_rx_data: SPI Transfer receive data buffer
 * @xfer_tx_data: SPI Transfer transmit data buffer
 * @himax_fw_data: Firmware data holder from flash
 * @himax_irq: IRQ number
 * @spi_xfer_max_sz: Size of SPI controller max transfer size
 * @xfer_buf_sz: Size of interrupt data buffer
 * @irq_state: IRQ state
 * @irq_lock: Spin lock for irq
 * @initialized: Indicate the driver is initialized
 * @probe_finish: Indicate the driver probe is finished
 * @ic_boot_done: Indicate the IC boot is done
 * @hid_probed: Indicate the HID device is probed
 * @touch_data_sz: Size of each interrupt data from IC
 * @dev: Device pointer
 * @spi: SPI device pointer
 * @hid: HID device pointer
 * @reg_lock: Mutex lock for reg access
 * @rw_lock: Mutex lock for read/write action
 * @ic_data: IC information holder
 * @pdata: Platform data holder
 * @fw_info_table: Firmware information address table of firmware image
 * @hid_desc: HID descriptor
 * @hid_rd_data: HID report descriptor data
 */
struct himax_ts_data {
	u8 *xfer_buf;
	u8 *xfer_rx_data;
	u8 *xfer_tx_data;
	u8 *himax_fw_data;
	s32 himax_irq;
	u32 spi_xfer_max_sz;
	u32 xfer_buf_sz;
	atomic_t irq_state;
	/* lock for irq_save */
	spinlock_t irq_lock;
	bool initialized;
	bool probe_finish;
	bool ic_boot_done;
	bool hid_probed;
	int touch_data_sz;
	struct device *dev;
	struct spi_device *spi;
	struct hid_device *hid;
	/* lock for register operation */
	struct mutex reg_lock;
	/* lock for bus read/write action */
	struct mutex rw_lock;
	struct himax_ic_data ic_data;
	struct himax_platform_data pdata;
	struct himax_fw_address_table fw_info_table;
	struct himax_hid_desc hid_desc;
	struct himax_hid_rd_data hid_rd_data;
};
#endif
