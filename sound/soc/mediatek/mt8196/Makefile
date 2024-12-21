# SPDX-License-Identifier: GPL-2.0

# MENU Governor Predict(low power) include path
ifeq ($(CONFIG_MTK_ACAO_SUPPORT),y)
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/mcdi/include/
endif

# adsp include path
ccflags-y += -I$(srctree)/drivers/misc/mediatek/audio_ipi/include
ccflags-y += -I$(srctree)/sound/soc/mediatek/audio_dsp
ccflags-y += -I$(srctree)/drivers/misc/mediatek/adsp/include

# spm include path
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include

# pbm include path
ccflags-y += -I$(srctree)/drivers/misc/mediatek/pbm

# common include path
subdir-ccflags-y += -I$(srctree)/sound/soc/mediatek/common

# platform driver
obj-$(CONFIG_SND_SOC_MT8196) += snd-soc-mt8196-afe.o
snd-soc-mt8196-afe-objs += \
	mt8196-afe-pcm.o \
	mt8196-afe-clk.o \
	mt8196-afe-gpio.o \
	mt8196-dai-adda.o \
	mt8196-afe-control.o \
	mt8196-dai-i2s.o \
	mt8196-dai-tdm.o \
	mt8196-afe-cm.o

# machine driver
obj-$(CONFIG_SND_SOC_MT8196_MT6681) += mt8196-mt6681.o
