# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_MTK_APUSYS_SUPPORT) += mtk_apu_mdw.o
obj-$(CONFIG_MTK_APUSYS_SUPPORT) += mtk_apu_mem.o
obj-$(CONFIG_MTK_APUSYS_SUPPORT) += mtk_apu_mvpu.o
obj-$(CONFIG_MTK_APUSYS_SUPPORT) += power/

APUSYS_PATH="$(srctree)/$(DEVICE_MODULES_REL_DIR)/drivers/soc/mediatek/apusys"

# midware
ccflags-y += -I$(APUSYS_PATH)/midware/2.0/
ccflags-y += -I$(APUSYS_PATH)/midware/2.0/rv/
ccflags-y += -I$(APUSYS_PATH)/midware/2.0/ext/
ccflags-y += -I$(APUSYS_PATH)/midware/2.0/cmd/
mtk_apu_mdw-objs += midware/2.0/mdw_mem_drv.o
mtk_apu_mdw-objs += midware/2.0/mdw_mem_rsc.o
mtk_apu_mdw-objs += midware/2.0/mdw_drv.o
mtk_apu_mdw-objs += midware/2.0/mdw_ctx.o
mtk_apu_mdw-objs += midware/2.0/mdw_dev.o
mtk_apu_mdw-objs += midware/2.0/mdw_ioctl.o
mtk_apu_mdw-objs += midware/2.0/mdw_mem.o
mtk_apu_mdw-objs += midware/2.0/mdw_mem_dma.o
mtk_apu_mdw-objs += midware/2.0/mdw_mem_aram.o
mtk_apu_mdw-objs += midware/2.0/mdw_mem_pool.o
mtk_apu_mdw-objs += midware/2.0/mdw_cmd.o
mtk_apu_mdw-objs += midware/2.0/cmd/mdw_cmd_v2.o
mtk_apu_mdw-objs += midware/2.0/cmd/mdw_cmd_v3.o
mtk_apu_mdw-objs += midware/2.0/cmd/mdw_cmd_v4.o
mtk_apu_mdw-objs += midware/2.0/cmd/mdw_cmd_cmn.o
mtk_apu_mdw-objs += midware/2.0/mdw_cmn.o
mtk_apu_mdw-objs += midware/2.0/mdw_hs.o
mtk_apu_mdw-objs += midware/2.0/mdw_sysfs.o
mtk_apu_mdw-objs += midware/2.0/mdw_dbg.o
mtk_apu_mdw-objs += midware/2.0/mdw_import.o
mtk_apu_mdw-objs += midware/2.0/mdw_util.o
mtk_apu_mdw-objs += midware/2.0/rv/mdw_rv.o
mtk_apu_mdw-objs += midware/2.0/rv/mdw_rv_cmd_v2.o
mtk_apu_mdw-objs += midware/2.0/rv/mdw_rv_cmd_v3.o
mtk_apu_mdw-objs += midware/2.0/rv/mdw_rv_cmd_v4.o
mtk_apu_mdw-objs += midware/2.0/rv/mdw_rv_dev.o
mtk_apu_mdw-objs += midware/2.0/rv/mdw_rv_tag.o
mtk_apu_mdw-objs += midware/2.0/ext/mdw_ext.o
mtk_apu_mdw-objs += midware/2.0/ext/mdw_ext_ioctl.o
mtk_apu_mdw-objs += midware/2.0/ext/mdw_ext_cmd.o
mtk_apu_mdw-objs += midware/2.0/ext/mdw_ext_hs.o

# apummu
ccflags-y += -I$(srctree)/$(src)/
mtk_apu_mem-objs += apu_mem/apu_mem_export.o
mtk_apu_mem-objs += apu_mem/apummu/common/apummu_boot.o
mtk_apu_mem-objs += apu_mem/apummu/common/apummu_drv.o
mtk_apu_mem-objs += apu_mem/apummu/common/apummu_dbg.o
mtk_apu_mem-objs += apu_mem/apummu/common/apummu_export.o
mtk_apu_mem-objs += apu_mem/apummu/common/apummu_import.o
mtk_apu_mem-objs += apu_mem/apummu/common/apummu_mem.o
mtk_apu_mem-objs += apu_mem/apummu/common/apummu_remote.o
mtk_apu_mem-objs += apu_mem/apummu/common/apummu_remote_cmd.o
mtk_apu_mem-objs += apu_mem/apummu/common/apummu_mgt.o
mtk_apu_mem-objs += apu_mem/apummu/platform/apummu_device.o
mtk_apu_mem-objs += apu_mem/apummu/platform/apummu_plat.o

# mvpu
ccflags-y += -I$(srctree)/$(DEVICE_MODULES_REL_DIR)/drivers/soc/mediatek/apusys/mvpu/common/
ccflags-y += -I$(srctree)/$(DEVICE_MODULES_REL_DIR)/drivers/soc/mediatek/apusys/mvpu/platform/v2/
ccflags-y += -I$(srctree)/$(DEVICE_MODULES_REL_DIR)/drivers/soc/mediatek/apusys/mvpu/platform/v2/2.5/
mtk_apu_mvpu-objs += mvpu/common/mvpu_driver.o
mtk_apu_mvpu-objs += mvpu/common/mvpu_plat.o
mtk_apu_mvpu-objs += mvpu/common/mvpu_sysfs.o
mtk_apu_mvpu-objs += mvpu/common/mvpu_config.o
subdir-ccflags-y += -DMVPU_V25_PLAT_DATA
mtk_apu_mvpu-objs += mvpu/platform/v2/2.5/mvpu25_ipi.o
mtk_apu_mvpu-objs += mvpu/platform/v2/2.5/mvpu25_sec.o
mtk_apu_mvpu-objs += mvpu/platform/v2/2.5/mvpu25_platdata.o
mtk_apu_mvpu-objs += mvpu/platform/v2/2.5/mvpu25_valid.o
mtk_apu_mvpu-objs += mvpu/platform/v2/2.5/mvpu25_handler.o
