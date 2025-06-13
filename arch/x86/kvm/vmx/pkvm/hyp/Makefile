# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I $(srctree)/arch/x86/kvm
ccflags-y += -I $(srctree)/arch/x86/kvm/vmx/pkvm/include
ccflags-y += -fno-stack-protector
ccflags-y += -D__DISABLE_EXPORTS
ccflags-y += -D__PKVM_HYP__
ccflags-y += -include $(srctree)/arch/x86/kvm/pkvm/def.h

asflags-y += -D__DISABLE_EXPORTS
asflags-y += -D__PKVM_HYP__
asflags-y += -I $(srctree)/arch/x86/kvm
asflags-y += -I $(srctree)/arch/x86/kvm/vmx/pkvm/include
asflags-y += -include $(srctree)/arch/x86/kvm/pkvm/def.h

pkvm-hyp-y	:= vmx_asm.o vmexit.o memory.o page_alloc.o early_alloc.o pgtable.o mmu.o pkvm.o \
		   init_finalise.o ept.o idt.o irq.o vmx.o vmsr.o mem_protect.o lapic.o \
		   iommu.o iommu_debug.o ptdev.o iommu_spgt.o io_emulate.o pci.o trace.o

pkvm		:= ../../../pkvm
pkvm-hyp-y	+= $(pkvm)/cpu.o $(pkvm)/pkvm.o $(pkvm)/x86.o $(pkvm)/vmx/vmx.o $(pkvm)/vmx/vmenter.o \
		   $(pkvm)/cpuid.o $(pkvm)/lapic.o

lib-dir		:= lib
lib2-dir	:= ../../../../../../lib
pkvm-hyp-y	+= $(lib-dir)/memset_64.o
pkvm-hyp-y	+= $(lib-dir)/memcpy_64.o
pkvm-hyp-y	+= $(lib2-dir)/find_bit.o
pkvm-hyp-y	+= $(lib2-dir)/hweight.o
pkvm-hyp-y	+= $(lib2-dir)/ctype.o $(lib2-dir)/string.o
pkvm-hyp-$(CONFIG_MITIGATION_RETPOLINE)	+= $(lib-dir)/retpoline.o
pkvm-hyp-$(CONFIG_LIST_HARDENED)	+= $(lib-dir)/list_debug.o

pkvm-obj 	:= $(patsubst %.o,%.pkvm.o,$(pkvm-hyp-y))
obj-$(CONFIG_PKVM_INTEL)	+= pkvm.o

AFLAGS_$(pkvm)/vmx/vmenter.pkvm.o	+= -iquote $(obj)/$(pkvm)

$(obj)/$(pkvm)/vmx/vmenter.pkvm.o: $(obj)/$(pkvm)/kvm-asm-offsets.h
$(obj)/$(pkvm)/kvm-asm-offsets.s: $(obj)/../../../kvm-asm-offsets.c FORCE
	$(call if_changed_dep,cc_s_c)
$(obj)/$(pkvm)/kvm-asm-offsets.h: $(obj)/$(pkvm)/kvm-asm-offsets.s FORCE
	$(call filechk,offsets,__KVM_ASM_OFFSETS_H__)

targets += $(pkvm-obj) pkvm.tmp.o pkvm.lds $(lib-dir)/retpoline.pkvm.S
clean-files += $(pkvm)/kvm-asm-offsets.h
cmd_deps	:= $(src)/compiling_cmds.sh

ifdef CONFIG_MITIGATION_RETPOLINE
$(obj)/$(lib-dir)/retpoline.pkvm.S: $(srctree)/arch/x86/lib/retpoline.S $(cmd_deps) FORCE
	$(call if_changed,patch_retpoline)
$(obj)/$(lib-dir)/retpoline.pkvm.o: $(obj)/$(lib-dir)/retpoline.pkvm.S FORCE
	$(call if_changed_rule,as_o_S)
endif

$(obj)/%.pkvm.o: $(src)/%.c FORCE
	$(call if_changed_rule,cc_o_c)
$(obj)/%.pkvm.o: $(src)/%.S FORCE
	$(call if_changed_rule,as_o_S)

$(obj)/pkvm.lds: $(src)/pkvm.lds.S FORCE
	$(call if_changed_dep,cpp_lds_S)

LDFLAGS_pkvm.tmp.o := -r -T
$(obj)/pkvm.tmp.o: $(obj)/pkvm.lds $(addprefix $(obj)/,$(pkvm-obj)) FORCE
	$(call if_changed,ld)

$(obj)/pkvm.o: $(obj)/pkvm.tmp.o $(cmd_deps) FORCE
	$(call if_changed,pkvmcopy)

quiet_cmd_pkvmcopy = PKVMPCOPY $@
cmd_pkvmcopy = $(BASH) $(src)/compiling_cmds.sh rename_syms $< $@

quiet_cmd_patch_retpoline = PATCH_RETPOLINE $@
cmd_patch_retpoline = $(BASH) $(src)/compiling_cmds.sh patch_retpoline $< $@

# Remove ftrace and Shadow Call Stack.
# This is equivalent to the 'notrace' and '__noscs' annotations.
#
# NOTE: do not remove CC_FLAGS_CFI. Since CFI code chunks don't depend
# on the host kernel's global data, we can keep CFI coverage enabled
# for the hypervisor as well, and we _should_ keep it enabled, for the
# following reasons (apart from its security advantages):
# 1. To avoid problems with the boot time kernel patching which assumes
#    that if CFI is enabled, it covers the entire kernel text (see
#    apply_seal_endbr()).
# 2. To keep the calling convention compatible in cases when hypervisor
#    functions are called from host kernel code (before deprivilege).
KBUILD_CFLAGS := $(filter-out $(CC_FLAGS_FTRACE) $(CC_FLAGS_SCS), $(KBUILD_CFLAGS))
