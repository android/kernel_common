# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I $(srctree)/arch/x86/kvm
ccflags-y += -I $(srctree)/arch/x86/kvm/vmx/pkvm/include

pkvm-obj		:= pkvm_host.o pkvm.o

obj-$(CONFIG_PKVM_INTEL)	+= $(pkvm-obj)
obj-$(CONFIG_PKVM_INTEL)	+= hyp/

always-y := pkvm_constants.h pkvm-constants.s

define rule_gen_hyp_constants
        $(call filechk,offsets,__PKVM_CONSTANTS_H__)
endef

CFLAGS_pkvm-constants.o = -I $(src)/include
CFLAGS_pkvm-constants.o += -I $(srctree)/virt/kvm/pkvm
$(obj)/pkvm-constants.s: $(src)/pkvm_constants.c FORCE
	        $(call if_changed_dep,cc_s_c)

$(obj)/pkvm_constants.h: $(obj)/pkvm-constants.s FORCE
	        $(call if_changed_rule,gen_hyp_constants)

obj-intel-pkvm := $(addprefix $(obj)/, $(pkvm-obj))
$(obj-intel-pkvm): $(obj)/pkvm_constants.h
