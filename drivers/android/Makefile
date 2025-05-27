# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -I$(src)			# needed for trace events

obj-$(CONFIG_ANDROID_BINDERFS)		+= binderfs.o
obj-$(CONFIG_ANDROID_BINDER_IPC)	+= binder.o binder_alloc.o
obj-$(CONFIG_ANDROID_BINDER_IPC_SELFTEST) += binder_alloc_selftest.o
obj-$(CONFIG_ANDROID_VENDOR_HOOKS) += vendor_hooks.o
obj-$(CONFIG_ANDROID_DEBUG_KINFO)	+= debug_kinfo.o

obj-$(CONFIG_ANDROID_BINDER_IPC_RUST)	+= rust_binder.o
rust_binder-objs := binder/rust_binder.o binder/rust_binderfs.o binder/rust_binder_events.o
rust_binder-objs += binder/rust_binder_hooks.o binder/page_range_helper.o
