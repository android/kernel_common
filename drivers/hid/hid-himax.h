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
#define HIMAX_TP_INFO_STR_LEN				12U
#define HIMAX_ZF_PARTITION_AMOUNT_OFFSET		12
#define HIMAX_ZF_PARTITION_DESC_SZ			16U
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
#define HIMAX_DSRAM_ADDR_VENDOR				0x10007000
#define HIMAX_DSRAM_ADDR_FW_VER				0x10007004
#define HIMAX_DSRAM_ADDR_CUS_INFO			0x10007008
#define HIMAX_DSRAM_ADDR_PROJ_INFO			0x10007014
#define HIMAX_DSRAM_ADDR_CFG				0x10007084
#define HIMAX_DSRAM_ADDR_INT_IS_EDGE			0x10007088
#define HIMAX_DSRAM_ADDR_MKEY				0x100070e8
#define HIMAX_DSRAM_ADDR_RXNUM_TXNUM			0x100070f4
#define HIMAX_DSRAM_ADDR_MAXPT_XYRVS			0x100070f8
#define HIMAX_DSRAM_ADDR_X_Y_RES			0x100070fc
#define HIMAX_DSRAM_ADDR_STYLUS_FUNCTION		0x1000719c
#define HIMAX_DSRAM_ADDR_STYLUS_VERSION			0x100071fc
#define HIMAX_DSRAM_ADDR_SET_NFRAME			0x10007294
#define HIMAX_DSRAM_ADDR_2ND_FLASH_RELOAD		0x100072c0
#define HIMAX_DSRAM_ADDR_FLASH_RELOAD			0x10007f00
#define HIMAX_DSRAM_ADDR_SORTING_MODE_EN		0x10007f04
#define HIMAX_DSRAM_ADDR_DBG_MSG			0x10007f40
#define HIMAX_DSRAM_ADDR_AP_NOTIFY_FW_SUSPEND		0x10007fd0
/* dsram flag data */
#define HIMAX_DSRAM_DATA_AP_NOTIFY_FW_SUSPEND		0xa55aa55a
#define HIMAX_DSRAM_DATA_AP_NOTIFY_FW_RESUME		0x00000000
#define HIMAX_DSRAM_DATA_DISABLE_FLASH_RELOAD		0x00009aa9
#define HIMAX_DSRAM_DATA_FW_RELOAD_DONE			0x000072c0
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
/* Map code of FW 1k header */
#define HIMAX_TP_CONFIG_TABLE				0x00000a00
#define HIMAX_FW_CID					0x10000000
#define HIMAX_FW_VER					0x10000100
#define HIMAX_CFG_VER					0x10000600
#define HIMAX_HID_TABLE					0x30000100
#define HIMAX_FW_BIN_DESC				0x10000000
#define HIMAX_BOOT_UPGRADE_FWNAME			"himax_i2chid"
#define HIMAX_FW_EXT_NAME				".bin"

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
 * @xfer_buf: Interrupt data buffer
 * @xfer_rx_data: SPI Transfer receive data buffer
 * @xfer_tx_data: SPI Transfer transmit data buffer
 * @zf_update_cfg_buffer: Zero flash update configuration buffer
 * @himax_irq: IRQ number
 * @excp_zero_event_count: Exception zero event count
 * @chip_max_dsram_size: Maximum size of DSRAM
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
 * @zf_update_lock: Mutex lock for zero-flash FW update
 * @ic_data: IC information holder
 * @pdata: Platform data holder
 * @fw_info_table: Firmware information address table of firmware image
 * @hid_desc: HID descriptor
 * @hid_rd_data: HID report descriptor data
 * @initial_work: Delayed work for TP initialization
 */
struct himax_ts_data {
	u8 *xfer_buf;
	u8 *xfer_rx_data;
	u8 *xfer_tx_data;
	u8 *zf_update_cfg_buffer;
	s32 himax_irq;
	s32 excp_zero_event_count;
	u32 chip_max_dsram_size;
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
	/* lock for zero-flash FW update */
	struct mutex zf_update_lock;
	struct himax_ic_data ic_data;
	struct himax_platform_data pdata;
	struct himax_fw_address_table fw_info_table;
	struct himax_hid_desc hid_desc;
	struct himax_hid_rd_data hid_rd_data;
	struct delayed_work initial_work;
};
#endif
