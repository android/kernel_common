# SPDX-License-Identifier: GPL-2.0
#
# Makefile for Kernel-based Virtual Machine module, HYP part
#

incdir := $(src)/include
subdir-asflags-y := -I$(incdir)
subdir-ccflags-y := -I$(incdir)

# ANDROID: Add -I for tracing
subdir-asflags-y += -I$(incdir)/module
subdir-ccflags-y += -I$(incdir)/module

obj-$(CONFIG_KVM) += vhe/ nvhe/ pgtable.o
