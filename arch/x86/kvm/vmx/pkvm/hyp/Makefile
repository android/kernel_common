# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I $(srctree)/arch/x86/kvm
ccflags-y += -I $(srctree)/arch/x86/kvm/vmx/pkvm/include
ccflags-y += -fno-stack-protector
ccflags-y += -D__DISABLE_EXPORTS
ccflags-y += -D__PKVM_HYP__

pkvm-hyp-y	:= vmx_asm.o vmexit.o memory.o page_alloc.o early_alloc.o pgtable.o mmu.o pkvm.o \
		   init_finalise.o ept.o idt.o irq.o nested.o vmx.o vmsr.o mem_protect.o lapic.o \
		   iommu.o iommu_debug.o trace.o

ifndef CONFIG_PKVM_INTEL_DEBUG
lib-dir		:= lib
lib2-dir	:= ../../../../../../lib
pkvm-hyp-y	+= $(lib-dir)/memset_64.o
pkvm-hyp-y	+= $(lib-dir)/memcpy_64.o
pkvm-hyp-y	+= $(lib2-dir)/find_bit.o
pkvm-hyp-y	+= $(lib2-dir)/hweight.o
pkvm-hyp-$(CONFIG_MITIGATION_RETPOLINE)	+= $(lib-dir)/retpoline.o
pkvm-hyp-$(CONFIG_LIST_HARDENED)	+= $(lib-dir)/list_debug.o
endif

pkvm-obj 	:= $(patsubst %.o,%.pkvm.o,$(pkvm-hyp-y))
obj-$(CONFIG_PKVM_INTEL)	+= pkvm.o
targets += $(pkvm-obj) pkvm.tmp.o pkvm.lds

$(obj)/%.pkvm.o: $(src)/%.c FORCE
	$(call if_changed_rule,cc_o_c)
$(obj)/%.pkvm.o: $(src)/%.S FORCE
	$(call if_changed_rule,as_o_S)

$(obj)/pkvm.lds: $(src)/pkvm.lds.S FORCE
	$(call if_changed_dep,cpp_lds_S)

LDFLAGS_pkvm.tmp.o := -r -T
$(obj)/pkvm.tmp.o: $(obj)/pkvm.lds $(addprefix $(obj)/,$(pkvm-obj)) FORCE
	$(call if_changed,ld)

$(obj)/pkvm.o: $(obj)/pkvm.tmp.o FORCE
	$(call if_changed,pkvmcopy)

quiet_cmd_pkvmcopy = PKVMPCOPY $@
ifdef CONFIG_PKVM_INTEL_DEBUG
      cmd_pkvmcopy = $(OBJCOPY) --prefix-symbols= $< $@
else
      cmd_pkvmcopy = $(OBJCOPY) --prefix-symbols=__pkvm_ --remove-section=.retpoline_sites --remove-section=.return_sites $< $@
endif

# Remove ftrace, Shadow Call Stack, and CFI CFLAGS.
# This is equivalent to the 'notrace', '__noscs', and '__nocfi' annotations.
KBUILD_CFLAGS := $(filter-out $(CC_FLAGS_FTRACE) $(CC_FLAGS_SCS) $(CC_FLAGS_CFI), $(KBUILD_CFLAGS))
