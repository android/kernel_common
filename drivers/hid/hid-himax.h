/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Himax hx83102j SPI Driver Code for HID.
 *
 * Copyright (C) 2024 Himax Corporation.
 */

#ifndef __HID_HIMAX_83102J_H__
#define __HID_HIMAX_83102J_H__

#include <drm/drm_panel.h>
#include <linux/crc32poly.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/hid.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
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
/* Clear 4 bytes data */
#define HIMAX_DATA_CLEAR				0x00000000
/* boot update start delay */
#define HIMAX_DELAY_BOOT_UPDATE_MS			2000
#define HIMAX_DELAY_PWR_INIT_CHECK_MS			3000
#define HIMAX_DELAY_PWR_CHECK_MS			1100
#define HIMAX_TP_INFO_STR_LEN				12U
#define HIMAX_ZF_PARTITION_AMOUNT_OFFSET		12
#define HIMAX_ZF_PARTITION_DESC_SZ			16U
/* heatmap header sizes */
#define HIMAX_HEAT_MAP_HEADER_SZ			3U
#define HIMAX_HEAT_MAP_HID_HDR_SZ			12U
#define HIMAX_HEAT_MAP_DATA_HDR_SZ			8U
#define HIMAX_HEAT_MAP_INFO_SZ \
	(HIMAX_HEAT_MAP_HID_HDR_SZ + HIMAX_HEAT_MAP_DATA_HDR_SZ)
#define HIMAX_HID_ID_SZ					1U
#define HIMAX_HID_REG_RW_SZ				1U
/*
 * HIDRAW REG R/W command max length:
 * [READ/WRITE:1][0xFFFFFFFF][REG_TYPE:1][REG_ADDR:1|4][REG_DATA:1~256]
 */
#define HIMAX_HID_REG_SZ_MAX				(1 + 4 + 1 + 4 + 256)
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
/* DSRAM flag addresses */
#define HIMAX_DSRAM_ADDR_RAWDATA			0x10000000
#define HIMAX_DSRAM_ADDR_VENDOR				0x10007000
#define HIMAX_DSRAM_ADDR_FW_VER				0x10007004
#define HIMAX_DSRAM_ADDR_CUS_INFO			0x10007008
#define HIMAX_DSRAM_ADDR_PROJ_INFO			0x10007014
#define HIMAX_DSRAM_ADDR_CFG				0x10007084
#define HIMAX_DSRAM_ADDR_INT_IS_EDGE			0x10007088
#define HIMAX_DSRAM_ADDR_MKEY				0x100070e8
#define HIMAX_DSRAM_ADDR_BANK_SEARCH			0x100070f4
#define HIMAX_DSRAM_ADDR_RXNUM_TXNUM			0x100070f4
#define HIMAX_DSRAM_ADDR_MAXPT_XYRVS			0x100070f8
#define HIMAX_DSRAM_ADDR_X_Y_RES			0x100070fc
#define HIMAX_DSRAM_ADDR_STYLUS_FUNCTION		0x1000719c
#define HIMAX_DSRAM_ADDR_STYLUS_VERSION			0x100071fc
#define HIMAX_DSRAM_ADDR_SET_NFRAME			0x10007294
#define HIMAX_DSRAM_ADDR_2ND_FLASH_RELOAD		0x100072c0
#define HIMAX_DSRAM_ADDR_STYLUS_CMD			0x10007428
#define HIMAX_DSRAM_ADDR_STYLUS_INFO			0x10007430
#define HIMAX_DSRAM_ADDR_FLASH_RELOAD			0x10007f00
#define HIMAX_DSRAM_ADDR_SORTING_MODE_EN		0x10007f04
#define HIMAX_DSRAM_ADDR_USB_DETECT			0x10007f38
#define HIMAX_DSRAM_ADDR_DBG_MSG			0x10007f40
#define HIMAX_DSRAM_ADDR_AP_NOTIFY_FW_SUSPEND		0x10007fd0
#define HIMAX_DSRAM_ADDR_NEG_NOISE_SUP			0x10007fd8
/* dsram flag data */
#define HIMAX_DSRAM_DATA_AP_NOTIFY_FW_SUSPEND		0xa55aa55a
#define HIMAX_DSRAM_DATA_AP_NOTIFY_FW_RESUME		0x00000000
#define HIMAX_DSRAM_DATA_DISABLE_FLASH_RELOAD		0x00009aa9
#define HIMAX_DSRAM_DATA_FW_RELOAD_DONE			0x000072c0
#define HIMAX_DSRAM_DATA_USB_ATTACH			0xa55aa55a
#define HIMAX_DSRAM_DATA_USB_DETACH			0x00000000
#define HIMAX_DSRAM_DATA_HANDSHAKING_RELEASE		0x00000000
#define HIMAX_DSRAM_DATA_NEG_NOISE			0x7f0c0000
#define HIMAX_DSRAM_DATA_SAFE_MODE_RELEASE		0x00000000
/* hx83102j-specific register/dsram flags/data */
#define HIMAX_HX83102J_DSRAM_ADDR_RAW_OUT_SEL		0x100072ec
#define HIMAX_HX83102J_REG_ADDR_HW_CRC			0x80010000
#define HIMAX_HX83102J_REG_ADDR_TCON_RST		0x80020004
#define HIMAX_HX83102J_REG_DATA_HW_CRC			0x0000ecce
#define HIMAX_HX83102J_REG_DATA_HW_CRC_DISABLE		0x00000000
/* hardware register addresses */
#define HIMAX_REG_ADDR_SPI200_DATA			0x8000002c
#define HIMAX_REG_ADDR_RELOAD_STATUS			0x80050000
#define HIMAX_REG_ADDR_RELOAD_CRC32_RESULT		0x80050018
#define HIMAX_REG_ADDR_RELOAD_ADDR_FROM			0x80050020
#define HIMAX_REG_ADDR_RELOAD_ADDR_CMD_BEAT		0x80050028
#define HIMAX_REG_ADDR_SYSTEM_RESET			0x90000018
#define HIMAX_REG_ADDR_RELOAD_TO_ACTIVE			0x90000048
#define HIMAX_REG_ADDR_CTRL_FW				0x9000005c
#define HIMAX_REG_ADDR_FW_STATUS			0x900000a8
#define HIMAX_REG_ADDR_ICID				0x900000d0
#define HIMAX_REG_ADDR_RESET_FLAG			0x900000e4
#define HIMAX_REG_ADDR_DD_STATUS			0x900000e8
/* hardware reg data/flags */
#define HIMAX_REG_DATA_FW_STATE_RUNNING			0x05
#define HIMAX_REG_DATA_FW_STATE_SAFE_MODE		0x0c
#define HIMAX_REG_DATA_FW_RE_INIT			0x00
#define HIMAX_REG_DATA_FW_GO_SAFEMODE			0xa5
#define HIMAX_REG_DATA_FW_IN_SAFEMODE			0x87
#define HIMAX_REG_DATA_ICID				0x83102900
#define HIMAX_REG_DATA_RELOAD_DONE			0x01
#define HIMAX_REG_DATA_RELOAD_PASSWORD			0x99
#define HIMAX_REG_DATA_SYSTEM_RESET			0x00000055
#define HIMAX_REG_DATA_TCON_RST				0x00000000
/* HIMAX SPI function select, 1st byte of any SPI command sequence */
#define HIMAX_SPI_FUNCTION_READ				0xf3
#define HIMAX_SPI_FUNCTION_WRITE			0xf2
/* HIDRAW commands */
#define HIMAX_HID_FW_UPDATE_BL_CMD			0x77
#define HIMAX_HID_FW_UPDATE_MAIN_CMD			0x55
/* Bank search data types */
#define HIMAX_BANK_SEARCH_RAWDATA			8
#define HIMAX_BANK_SEARCH_NOISE				8
#define HIMAX_BANK_SEARCH_OPENSHORT			0
/* Inspection dataType */
#define HIMAX_DATA_TYPE_SORTING				0x0a
#define HIMAX_DATA_TYPE_OPEN				0x0b
#define HIMAX_DATA_TYPE_MICRO_OPEN			0x0c
#define HIMAX_DATA_TYPE_SHORT				0x0a
#define HIMAX_DATA_TYPE_RAWDATA				0x0a
#define HIMAX_DATA_TYPE_NOISE				0x0f
#define HIMAX_DATA_TYPE_BACK_NORMAL			0x00
#define HIMAX_HID_RAW_DATA_TYPE_DELTA			0x09
#define HIMAX_HID_RAW_DATA_TYPE_RAW			0x0a
#define HIMAX_HID_RAW_DATA_TYPE_BASELINE		0x0b
#define HIMAX_HID_RAW_DATA_TYPE_NORMAL			0x00
/* N frame parameters */
#define	HIMAX_NFRAME_NOISE				60
#define HIMAX_NFRAME_OTHER				2
/* Self test start-end passwords */
#define	HIMAX_PWD_OPEN_START				0x7777
#define	HIMAX_PWD_OPEN_END				0x8888
#define	HIMAX_PWD_SHORT_START				0x1111
#define	HIMAX_PWD_SHORT_END				0x3333
#define	HIMAX_PWD_RAWDATA_START				0x0000
#define	HIMAX_PWD_RAWDATA_END				0x9999
#define	HIMAX_PWD_NOISE_START				0x0000
#define	HIMAX_PWD_NOISE_END				0x9999
#define	HIMAX_PWD_SORTING_START				0xaaaa
#define	HIMAX_PWD_SORTING_END				0xcccc
/* DSRAM data handshake passwords */
#define	HIMAX_SRAM_PASSWRD_START			0x5aa5
#define	HIMAX_SRAM_PASSWRD_END				0xa55a
/* Map code of FW 1k header */
#define HIMAX_TP_CONFIG_TABLE				0x00000a00
#define HIMAX_FW_CID					0x10000000
#define HIMAX_FW_VER					0x10000100
#define HIMAX_CFG_VER					0x10000600
#define HIMAX_HID_TABLE					0x30000100
#define HIMAX_FW_BIN_DESC				0x10000000
/* time function generalize */
#define HIMAX_TIME_VAR					timespec64
#define HIMAX_TIME_VAR_FINE				tv_nsec
#define HIMAX_TIME_VAR_FINE_UNIT			(1000 * 1000)
#define HIMAX_TIME_FUNC					ktime_get_ts64
/* Firmware update error code for HIDRAW */
#define HIMAX_FWUP_NO_ERROR				0x77
#define HIMAX_FWUP_BL_READY				0xb1
#define HIMAX_FWUP_FLASH_PROG_ERROR			0xb5
#define HIMAX_BOOT_UPGRADE_FWNAME			"himax_i2chid"
#define HIMAX_FW_EXT_NAME				".bin"

/**
 * enum himax_hidraw_id_function - HIDRAW report IDs
 * @HIMAX_ID_CONTACT_COUNT: Contact count report ID
 * @HIMAX_ID_CFG: Configuration report ID
 * @HIMAX_ID_REG_RW: Register read/write report ID
 * @HIMAX_ID_TOUCH_MONITOR_SEL: Touch monitor data type select report ID
 * @HIMAX_ID_TOUCH_MONITOR: Touch monitor report ID
 * @HIMAX_ID_FW_UPDATE: Firmware update report ID
 * @HIMAX_ID_FW_UPDATE_HANDSHAKING: Firmware update handshaking report ID
 * @HIMAX_ID_SELF_TEST: Self test report ID
 * @HIMAX_ID_USI_COLOR: USI color report ID
 * @HIMAX_ID_USI_WIDTH: USI width report ID
 * @HIMAX_ID_USI_STYLE: USI style report ID
 * @HIMAX_ID_USI_BUTTONS: USI buttons report ID
 * @HIMAX_ID_USI_FIRMWARE: USI stylus firmware version report ID
 * @HIMAX_ID_USI_PROTOCOL: USI protocol version report ID
 * @HIMAX_ID_INPUT_RD_DE: Input report data disable report ID
 * @HIMAX_ID_WINDOWS_BLOB_VALID: Windows blob validation report ID
 */
enum himax_hidraw_id_function {
	HIMAX_ID_CONTACT_COUNT = 0x03,
	HIMAX_ID_CFG = 0x05,
	HIMAX_ID_REG_RW,
	HIMAX_ID_TOUCH_MONITOR_SEL = 0x07,
	HIMAX_ID_TOUCH_MONITOR,
	HIMAX_ID_FW_UPDATE = 0x0a,
	HIMAX_ID_FW_UPDATE_HANDSHAKING,
	HIMAX_ID_SELF_TEST,
	HIMAX_ID_USI_COLOR = 0x11,
	HIMAX_ID_USI_WIDTH,
	HIMAX_ID_USI_STYLE,
	HIMAX_ID_USI_BUTTONS = 0x15,
	HIMAX_ID_USI_FIRMWARE,
	HIMAX_ID_USI_PROTOCOL,
	HIMAX_ID_USI_TRANSDUCER = 0x19,
	HIMAX_ID_INPUT_RD_DE = 0x31,
	HIMAX_ID_WINDOWS_BLOB_VALID = 0x44,
};

/**
 * enum himax_hid_self_test_type - Self test parameters for HIDRAW
 * @HIMAX_HID_SELF_TEST_SHORT: Panel ADC node Short test
 * @HIMAX_HID_SELF_TEST_OPEN: Panel ADC node Open test
 * @HIMAX_HID_SELF_TEST_MICRO_OPEN: Panel ADC node Micro Open test
 * @HIMAX_HID_SELF_TEST_RAWDATA: Panel ADC node Rawdata test
 * @HIMAX_HID_SELF_TEST_NOISE: Panel ADC Noise test
 * @HIMAX_HID_SELF_TEST_RESET: Reset self test process, return to normal mode
 */
enum himax_hid_self_test_type {
	HIMAX_HID_SELF_TEST_SHORT = 0x11,
	HIMAX_HID_SELF_TEST_OPEN,
	HIMAX_HID_SELF_TEST_MICRO_OPEN,
	HIMAX_HID_SELF_TEST_RAWDATA = 0x21,
	HIMAX_HID_SELF_TEST_NOISE,
	HIMAX_HID_SELF_TEST_RESET = 0x01,
};

/**
 * enum himax_hid_self_test_status - Self test return status for HIDRAW
 * @HIMAX_HID_SELF_TEST_FINISH: report self test has finished
 * @HIMAX_HID_SELF_TEST_ERROR: report self test has error
 */
enum himax_hid_self_test_status {
	HIMAX_HID_SELF_TEST_FINISH = 0xff,
	HIMAX_HID_SELF_TEST_ERROR = 0xef
};

/**
 * enum himax_hid_inspection_type - Inspection modes of self test
 * @HIMAX_INSPECT_OPEN: Open mode
 * @HIMAX_INSPECT_MICRO_OPEN: Micro Open mode
 * @HIMAX_INSPECT_SHORT: Short mode
 * @HIMAX_INSPECT_ABS_NOISE: Absolute Noise mode
 * @HIMAX_INSPECT_RAWDATA: Rawdata mode
 * @HIMAX_INSPECT_SORTING: Sorting mode
 * @HIMAX_INSPECT_BACK_NORMAL: Normal mode
 */
enum himax_hid_inspection_type {
	HIMAX_INSPECT_OPEN,
	HIMAX_INSPECT_MICRO_OPEN,
	HIMAX_INSPECT_SHORT,
	HIMAX_INSPECT_ABS_NOISE,
	HIMAX_INSPECT_RAWDATA,
	HIMAX_INSPECT_SORTING,
	HIMAX_INSPECT_BACK_NORMAL
};

/**
 * enum himax_data_type - Data type index of self test mapping of actual types
 * @HIMAX_DATA_SORTING: Sorting data index
 * @HIMAX_DATA_OPEN: Open data index
 * @HIMAX_DATA_MICRO_OPEN: Micro Open data index
 * @HIMAX_DATA_SHORT: Short data index
 * @HIMAX_DATA_RAWDATA: Rawdata data index
 * @HIMAX_DATA_NOISE: Noise data index
 * @HIMAX_DATA_BACK_NORMAL: Normal data index
 * @HIMAX_DATA_TYPE_MAX: enum amount for data allocate
 */
enum himax_data_type {
	HIMAX_DATA_SORTING,
	HIMAX_DATA_OPEN,
	HIMAX_DATA_MICRO_OPEN,
	HIMAX_DATA_SHORT,
	HIMAX_DATA_RAWDATA,
	HIMAX_DATA_NOISE,
	HIMAX_DATA_BACK_NORMAL,
	HIMAX_DATA_TYPE_MAX
};

/**
 * enum himax_inspection_result - Inspection result of self test
 * @HIMAX_INSPECT_OK: Self test pass
 * @HIMAX_INSPECT_CHANGE_MODE_REQUIRED: Change mode required
 * @HIMAX_INSPECT_FAIL: Self test fail
 * @HIMAX_INSPECT_ENOMEM: Memory allocate errors
 * @HIMAX_INSPECT_ESCREEN: Abnormal screen state Invalid
 * @HIMAX_INSPECT_ESPECT: Out of specification
 * @HIMAX_INSPECT_EFILE: Criteria file loading error
 * @HIMAX_INSPECT_ESWITCHMODE: Switch mode error
 * @HIMAX_INSPECT_ESWITCHDATA: Switch data error
 * @HIMAX_INSPECT_EGETRAW: Get raw data errors
 */
enum himax_inspection_result {
	HIMAX_INSPECT_OK,
	HIMAX_INSPECT_CHANGE_MODE_REQUIRED,
	HIMAX_INSPECT_FAIL = 1 << 1,
	HIMAX_INSPECT_ENOMEM = 1 << 2,
	HIMAX_INSPECT_ESCREEN = 1 << 3,
	HIMAX_INSPECT_ESPECT = 1 << 4,
	HIMAX_INSPECT_EFILE = 1 << 5,
	HIMAX_INSPECT_ESWITCHMODE = 1 << 6,
	HIMAX_INSPECT_ESWITCHDATA = 1 << 7,
	HIMAX_INSPECT_EGETRAW = 1 << 8,
};

/**
 * enum himax_hid_raw_data_type - Raw data types indexes of raw data select
 * @HIMAX_HID_RAW_DATA_TYPE_DELTA_INDEX: Delta index
 * @HIMAX_HID_RAW_DATA_TYPE_RAW_INDEX: Raw index
 * @HIMAX_HID_RAW_DATA_TYPE_BASELINE_INDEX: Baseline index
 * @HIMAX_HID_RAW_DATA_TYPE_NORMAL_INDEX: Normal index
 * @HIMAX_HID_RAW_DATA_TYPE_MAX: Enum limit for raw data types
 */
enum himax_hid_raw_data_type {
	HIMAX_HID_RAW_DATA_TYPE_DELTA_INDEX,
	HIMAX_HID_RAW_DATA_TYPE_RAW_INDEX,
	HIMAX_HID_RAW_DATA_TYPE_BASELINE_INDEX,
	HIMAX_HID_RAW_DATA_TYPE_NORMAL_INDEX,
	HIMAX_HID_RAW_DATA_TYPE_MAX
};

/**
 * enum himax_hid_reg_action - HIDRAW register read/write action
 * @HIMAX_HID_REG_READ: Read action
 * @HIMAX_HID_REG_WRITE: Write action
 */
enum himax_hid_reg_action {
	HIMAX_HID_REG_READ,
	HIMAX_HID_REG_WRITE
};

/**
 * enum himax_hid_reg_types - Reg type parameters if hidraw reg action
 * @HIMAX_REG_TYPE_EXT_AHB: AHB register type in EXT type
 * @HIMAX_REG_TYPE_EXT_SRAM: SRAM register type in EXT type
 * @HIMAX_REG_TYPE_EXT_TYPE: Special address value to indicate using EXT type
 */
enum himax_hid_reg_types {
	HIMAX_REG_TYPE_EXT_AHB,
	HIMAX_REG_TYPE_EXT_SRAM,
	HIMAX_REG_TYPE_EXT_TYPE = 0xFFFFFFFF
};

/**
 * enum himax_hid_load_user_fw_status - HIDRAW return code for firmware update
 * @HIMAX_LOAD_FIRMWARE_ONGOING: Indicate firmware file is still loading
 * @HIMAX_LOAD_FIRMWARE_DONE: Indicate firmware file has been loaded
 */
enum himax_hid_load_user_fw_status {
	HIMAX_LOAD_FIRMWARE_ONGOING = 1,
	HIMAX_LOAD_FIRMWARE_DONE,
};

/**
 * enum himax_touch_report_status - ts operation return code for touch report
 * @HIMAX_TS_GET_DATA_FAIL: Get touch data fail
 * @HIMAX_TS_EXCP_EVENT: ESD event detected
 * @HIMAX_TS_ABNORMAL_PATTERN: Check interrupt data fail
 * @HIMAX_TS_SUCCESS: Get touch data success
 * @HIMAX_TS_EXCP_REC_OK: 1st report after ESD recovery
 * @HIMAX_TS_EXCP_REC_FAIL: error during ESD recovery
 * @HIMAX_TS_EXCP_NO_MATCH: checksum fail but no ESD event
 * @HIMAX_TS_REPORT_DATA: Report ts data to upper layer
 * @HIMAX_TS_IC_RUNNING: IC is busy, ignore the report
 * @HIMAX_TS_ZERO_EVENT_CNT: All data report is zero
 */
enum himax_touch_report_status {
	HIMAX_TS_GET_DATA_FAIL = -4,
	HIMAX_TS_EXCP_EVENT,
	HIMAX_TS_ABNORMAL_PATTERN,
	HIMAX_TS_SUCCESS = 0,
	HIMAX_TS_EXCP_REC_OK,
	HIMAX_TS_EXCP_REC_FAIL,
	HIMAX_TS_EXCP_NO_MATCH,
	HIMAX_TS_REPORT_DATA,
	HIMAX_TS_IC_RUNNING,
	HIMAX_TS_ZERO_EVENT_CNT,
};

/**
 * struct himax_zf_info - Zero flash update information
 * @sram_addr: SRAM address byte array buffer
 * @write_size: Write size of each chunk
 * @fw_addr: Offset in firmware file
 * @cfg_addr: target sram address
 */
struct himax_zf_info {
	u8 sram_addr[4];
	int write_size;
	u32 fw_addr;
	u32 cfg_addr;
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
 * struct himax_rd_feature_unit - Report descriptor feature unit
 * @id_tag: ID tag
 * @id: ID
 * @usage_tag: Usage tag
 * @usage: Usage
 * @report_cnt_tag: Report count tag
 * @report_cnt: Report count
 * @feature_tag: Feature tag
 *
 * This structure is used to map feature report descriptor array of HIDRAW.
 * For driver fast access the member of feature report descriptor.
 */
struct himax_rd_feature_unit {
	u8 id_tag;
	u8 id;
	u8 usage_tag;
	u8 usage;
	u8 report_cnt_tag;
	u16 report_cnt;
	u8 feature_tag[2];
} __packed;

/**
 * struct himax_hid_req_cfg - HIDRAW request configuration
 * @reg_data: Register data buffer, not include READ/WRITE, REG_ADDR
 * @processing_id: Store last ID of HIDRAW request
 * @self_test_type: Self test type
 * @handshake_set: Store the HID set parameter for last set request
 * @handshake_get: Store the HID get parameter for last get request
 * @current_size: Accumulated size of fw data
 * @reg_addr_sz: Register address size
 * @reg_data_sz: Register data size
 * @input_RD_de: Input report data disable switch
 * @reg_addr: Register address data
 * @fw: Firmware data holder from user space
 *
 * This structure is used to hold the HIDRAW request configuration.
 * Member is filled by user space function select.
 * reg_addr format:
 * HID register READ/WRITE format:
 * STANDARD TYPE
 * [READ/WRITE:1][REG_ADDR:4][REG_DATA:4] : 9 bytes
 *   1             2~5         6~9
 * EXT TYPE
 * [READ/WRITE:1][0xFFFFFFFF][REG_TYPE:1][REG_ADDR:1|4][REG_DATA:1~256]
 *   1             2~5         6           7|7~10        8~263|11~266
 */
struct himax_hid_req_cfg {
	u8 reg_data[HIMAX_HID_REG_SZ_MAX - HIMAX_HID_REG_RW_SZ - HIMAX_REG_SZ];
	u32 processing_id;
	u32 self_test_type;
	u32 handshake_set;
	u32 handshake_get;
	u32 current_size;
	u32 reg_addr_sz;
	u32 reg_data_sz;
	u32 input_RD_de;
	union himax_dword_data reg_addr;
	struct firmware *fw;
};

/**
 * struct himax_ic_data - IC information holder
 * @stylus_ratio: Stylus ratio
 * @vendor_cus_info: Vendor customer information
 * @vendor_proj_info: Vendor project information
 * @vendor_fw_ver: Vendor firmware version
 * @vendor_config_ver: Vendor config version
 * @vendor_touch_cfg_ver: Vendor touch config version
 * @vendor_display_cfg_ver: Vendor display config version
 * @vendor_cid_maj_ver: Vendor CID major version
 * @vendor_cid_min_ver: Vendor CID minor version
 * @vendor_panel_ver: Vendor panel version
 * @vendor_sensor_id: Vendor sensor ID
 * @bl_size: Bootloader size
 * @rx_num: Number of RX
 * @tx_num: Number of TX
 * @button_num: Number of buttons
 * @x_res: X resolution
 * @y_res: Y resolution
 * @max_point: Maximum touch point
 * @icid: IC ID
 * @interrupt_is_edge: Interrupt is edge otherwise level
 * @stylus_function: Stylus function available or not
 * @stylus_v2: Is stylus version 2
 * @enc16bits: indicate heatmap is encoded in 16 bits data or 12 bits data
 */
struct himax_ic_data {
	u8 stylus_ratio;
	u8 vendor_cus_info[12];
	u8 vendor_proj_info[12];
	int vendor_fw_ver;
	int vendor_config_ver;
	int vendor_touch_cfg_ver;
	int vendor_display_cfg_ver;
	int vendor_cid_maj_ver;
	int vendor_cid_min_ver;
	int vendor_panel_ver;
	int vendor_sensor_id;
	u32 bl_size;
	u32 rx_num;
	u32 tx_num;
	u32 button_num;
	u32 x_res;
	u32 y_res;
	u32 max_point;
	u32 icid;
	bool interrupt_is_edge;
	bool stylus_function;
	bool stylus_v2;
	bool enc16bits;
};

/**
 * struct himax_hid_fw_unit - HIDRAW firmware unit
 * @cmd: Command from user space indicate which part is trying to update
 * @bin_start_offset: Start offset of firmware in firmware image
 * @unit_sz: Size of firmware unit
 *
 * This structure is used to hold the HIDRAW firmware unit.
 * User program will use this structure to select which part of firmware
 * to update.
 */
struct himax_hid_fw_unit {
	u8 cmd;
	u16 bin_start_offset;
	u16 unit_sz;
} __packed;

/**
 * union himax_heatmap_rd - heatmap report descriptor
 * @heatmap_struct: heatmap report descriptor
 * @host_report_descriptor: heatmap report descriptor array
 * @_heatmap_struct: heatmap report descriptor structure
 * @_heatmap_struct.header: header of heatmap report descriptor
 * @_heatmap_struct.heatmap_info_desc: heatmap information descriptor
 * @_heatmap_struct.heatmap_data_hdr: heatmap data header
 * @_heatmap_struct.heatmap_data_cnt_tag: heatmap data count tag
 * @_heatmap_struct.heatmap_data_cnt: heatmap data count
 * @_heatmap_struct.heatmap_input_desc: heatmap input descriptor
 * @_heatmap_struct.end_collection: end collection tag
 *
 * This union is used to hold the heatmap report descriptor.
 * heatmap_struct disammble the report descriptor array to structure
 * for fast access thus need to be packed.
 */
union himax_heatmap_rd {
	struct __packed _heatmap_struct {
		u8 header[17];
		u8 heatmap_info_desc[29];
		u8 heatmap_data_hdr[9];
		u8 heatmap_data_cnt_tag;
		u16 heatmap_data_cnt;
		u8 heatmap_input_desc[2];
		u8 end_collection;
	} heatmap_struct;
	u8 host_report_descriptor[sizeof(struct _heatmap_struct)];
};

/**
 * union himax_host_ext_rd - extend the function of HIDRAW report descriptor
 * @rd_struct: HIDRAW report descriptor
 * @host_report_descriptor: HIDRAW report descriptor array
 * @_rd_struct: HIDRAW report descriptor structure
 * @_rd_struct.header: header of HIDRAW report descriptor
 * @_rd_struct.cfg: Enable ID_CFG feature
 * @_rd_struct.reg_rw: Enable ID_REG_RW feature
 * @_rd_struct.monitor_sel: Enable ID_TOUCH_MONITOR_SEL feature
 * @_rd_struct.monitor: Enable ID_TOUCH_MONITOR feature
 * @_rd_struct.fw_update: Enable ID_FW_UPDATE feature
 * @_rd_struct.fw_update_handshaking: Enable ID_FW_UPDATE_HANDSHAKING feature
 * @_rd_struct.self_test: Enable ID_SELF_TEST feature
 * @_rd_struct.input_rd_en: Enable ID_INPUT_RD_DE feature
 * @_rd_struct.windows_blob: Enable ID_WINDOWS_BLOB_VALIDATION feature
 * @_rd_struct.end_collection: end collection tag
 *
 * This union is used to extend the function of HIDRAW report descriptor.
 * User space could use these IDs to debug TPIC.
 */
union himax_host_ext_rd {
	struct __packed _rd_struct {
		u8 header[14];
		/* HIMAX_ID_CFG */
		struct himax_rd_feature_unit cfg;
		/* HIMAX_ID_REG_RW */
		struct himax_rd_feature_unit reg_rw;
		/* HIMAX_ID_TOUCH_MONITOR_SEL */
		struct himax_rd_feature_unit monitor_sel;
		/* HIMAX_ID_TOUCH_MONITOR */
		struct himax_rd_feature_unit monitor;
		/* HIMAX_ID_FW_UPDATE */
		struct himax_rd_feature_unit fw_update;
		/* HIMAX_ID_FW_UPDATE_HANDSHAKING */
		struct himax_rd_feature_unit fw_update_handshaking;
		/* HIMAX_ID_SELF_TEST */
		struct himax_rd_feature_unit self_test;
		/* HIMAX_ID_INPUT_RD_DE */
		struct himax_rd_feature_unit input_rd_en;
		/* HIMAX_HIMAX_ID_WINDOWS_BLOB_VALIDATION */
		struct himax_rd_feature_unit windows_blob;
		u8 end_collection;
	} rd_struct;
	u8 host_report_descriptor[sizeof(struct _rd_struct)];
};

/**
 * struct himax_bin_desc - Firmware binary descriptor
 * @passwd: Password to indicate the binary is valid
 * @cid: Customer ID
 * @panel_ver: Panel version
 * @fw_ver: Firmware version
 * @ic_sign: IC signature
 * @customer: Customer name
 * @project: Project name
 * @fw_major: Major version of firmware
 * @fw_minor: Minor version of firmware
 * @date: Generate date of firmware
 * @ic_sign_2: IC signature 2
 *
 * This structure is used to hold the firmware binary descriptor.
 * It directly maps to a sequence of bytes in firmware image,
 * thus need to be packed.
 */
struct himax_bin_desc {
	u16 passwd;
	u16 cid;
	u8 panel_ver;
	u16 fw_ver;
	u8 ic_sign;
	char customer[12];
	char project[12];
	char fw_major[12];
	char fw_minor[12];
	char date[12];
	char ic_sign_2[12];
} __packed;

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
 * struct himax_hid_info - IC information holder for HIDRAW function
 * @main_mapping: Main code mapping descriptor for fw update program
 * @bl_mapping: Bootloader mapping descriptor for fw update program
 * @fw_bin_desc: Firmware binary descriptor for fw update program
 * @vid: Vendor ID
 * @pid: Product ID
 * @cfg_info: Configuration information
 * @cfg_version: Configuration version
 * @disp_version: Display version
 * @rx: Number of RX
 * @tx: Number of TX
 * @y_res: Y resolution
 * @x_res: X resolution
 * @pt_num: Number of touch points
 * @mkey_num: Number of mkey
 * @debug_info: Debug information
 *
 * This structure is used to hold the IC config information for HIDRAW.
 * The format is binary fixed, thus need to be packed.
 */
struct himax_hid_info {
	struct himax_hid_fw_unit main_mapping[9];
	struct himax_hid_fw_unit bl_mapping;
	struct himax_bin_desc fw_bin_desc;
	u16 vid;
	u16 pid;
	u8 cfg_info[32];
	u8 cfg_version;
	u8 disp_version;
	u8 rx;
	u8 tx;
	u16 y_res;
	u16 x_res;
	u8 pt_num;
	u8 mkey_num;
	u8 debug_info[78];
} __packed;

/**
 * struct himax_usi_cmd - USI command to IC
 * @cmd_id: Command ID from user-space
 * @data: Command data from user-space
 *
 * This structure is used to hold the USI command from user-space using HIDRAW.
 */
struct himax_usi_cmd {
	u8 id;
	u8 data[7];
} __packed;

/**
 * struct himax_usi_info - USI stylus information holder
 * @pen_color: USI Pen color
 * @pen_color_locked: USI Pen color locked
 * @pen_width: USI Pen width
 * @pen_width_locked: USI Pen width locked
 * @pen_style: USI Pen style
 * @pen_style_locked: USI Pen style locked
 * @pen_buttons: USI Pen buttons
 * @pen_firmware_version: USI Pen firmware version
 * @pen_protocol_major: USI Pen protocol major
 * @pen_protocol_minor: USI Pen protocol minor
 * @pen_transducer: USI Pen transducer
 *
 * This structure is used to hold the USI stylus information from IC.
 * The format is binary fixed, thus need to be packed. The USI stylus
 * information will be used to report USI data to user-space application
 * using HIDRAW.
 */
struct himax_usi_info {
	u8 pen_color;
	u8 pen_color_locked;
	u8 pen_width;
	u8 pen_width_locked;
	u8 pen_style;
	u8 pen_style_locked;
	u8 pen_buttons[3];
	u8 pen_firmware_version[12];
	u8 pen_protocol_major;
	u8 pen_protocol_minor;
	u8 pen_transducer;
} __packed;

/**
 * struct himax_platform_data - Platform data holder
 * @is_panel_follower: Is panel follower enabled
 * @vccd_supply: VCCD supply
 * @panel_follower: DRM panel follower
 * @gpiod_rst: GPIO reset
 *
 * This structure is used to hold the platform related data.
 */
struct himax_platform_data {
	bool is_panel_follower;
	struct regulator *vccd_supply;
	struct drm_panel_follower panel_follower;
	struct gpio_desc *gpiod_rst;
};

/**
 * struct himax_ts_data - Touchscreen data holder
 * @latest_power_status: Latest power status
 * @usb_connected: USB connected status
 * @xfer_buf: Interrupt data buffer
 * @xfer_rx_data: SPI Transfer receive data buffer
 * @xfer_tx_data: SPI Transfer transmit data buffer
 * @heatmap_buf: Heatmap buffer
 * @zf_update_cfg_buffer: Zero flash update configuration buffer
 * @himax_irq: IRQ number
 * @excp_zero_event_count: Exception zero event count
 * @chip_max_dsram_size: Maximum size of DSRAM
 * @heatmap_data_size: Heatmap data size
 * @spi_xfer_max_sz: Size of SPI controller max transfer size
 * @xfer_buf_sz: Size of interrupt data buffer
 * @irq_state: IRQ state
 * @irq_lock: Spin lock for irq
 * @excp_reset_active: Indicate the exception reset is active
 * @initialized: Indicate the driver is initialized
 * @probe_finish: Indicate the driver probe is finished
 * @ic_boot_done: Indicate the IC boot is done
 * @hid_probed: Indicate the HID device is probed
 * @resume_succeeded: Indicate the resume is succeeded
 * @firmware_name: Firmware name
 * @touch_data_sz: Size of each interrupt data from IC
 * @himax_fw: Firmware data holder from user space
 * @dev: Device pointer
 * @spi: SPI device pointer
 * @hid: HID device pointer
 * @reg_lock: Mutex lock for reg access
 * @rw_lock: Mutex lock for read/write action
 * @hid_ioctl_lock: Mutex lock for hid ioctl action
 * @zf_update_lock: Mutex lock for zero-flash FW update
 * @ic_data: IC information holder
 * @pdata: Platform data holder
 * @fw_info_table: Firmware information address table of firmware image
 * @hid_desc: HID descriptor
 * @hid_rd_data: HID report descriptor data
 * @hid_info: HID information
 * @hid_req_cfg: HID request configuration
 * @fw_bin_desc: Firmware binary descriptor
 * @power_notif: Power change notifier
 * @himax_pwr_wq: Workqueue for power check
 * @work_pwr: Delayed work for power check
 * @initial_work: Delayed work for TP initialization
 * @himax_hidraw_wq: Workqueue for hidraw
 * @work_hid_update: Delayed work for hid update
 * @work_self_test: Delayed work for self test
 */
struct himax_ts_data {
	u8 latest_power_status;
	u8 *xfer_buf;
	u8 *xfer_rx_data;
	u8 *xfer_tx_data;
	u8 *heatmap_buf;
	u8 *zf_update_cfg_buffer;
	s32 himax_irq;
	s32 excp_zero_event_count;
	u32 chip_max_dsram_size;
	u32 heatmap_data_size;
	u32 spi_xfer_max_sz;
	u32 xfer_buf_sz;
	atomic_t irq_state;
	/* lock for irq_save */
	spinlock_t irq_lock;
	bool excp_reset_active;
	bool initialized;
	bool probe_finish;
	bool ic_boot_done;
	bool hid_probed;
	bool resume_succeeded;
	bool usb_connected;
	bool zf_update_flag;
	char firmware_name[64];
	int touch_data_sz;
	const struct firmware *himax_fw;
	struct device *dev;
	struct spi_device *spi;
	struct hid_device *hid;
	/* lock for register operation */
	struct mutex reg_lock;
	/* lock for bus read/write action */
	struct mutex rw_lock;
	/* lock for hidraw ioctl request */
	struct mutex hid_ioctl_lock;
	/* lock for zero-flash FW update */
	struct mutex zf_update_lock;
	struct himax_ic_data ic_data;
	struct himax_platform_data pdata;
	struct himax_fw_address_table fw_info_table;
	struct himax_hid_desc hid_desc;
	struct himax_hid_rd_data hid_rd_data;
	struct himax_hid_info hid_info;
	struct himax_hid_req_cfg hid_req_cfg;
	struct himax_bin_desc fw_bin_desc;
	struct notifier_block power_notif;
	struct workqueue_struct *himax_pwr_wq;
	struct delayed_work work_pwr;
	struct delayed_work initial_work;
	struct workqueue_struct *himax_hidraw_wq;
	struct delayed_work work_hid_update;
	struct delayed_work work_self_test;
};
#endif
