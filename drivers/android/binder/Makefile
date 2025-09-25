# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -I$(src)	# needed for trace events
ccflags-y += -Wmissing-prototypes -Wmissing-declarations

obj-$(CONFIG_ANDROID_BINDER_IPC_RUST) += rust_binder.o
rust_binder-$(CONFIG_ANDROID_BINDER_IPC_RUST) := \
	rust_binder_main.o	\
	rust_binderfs.o		\
	rust_binder_events.o	\
	rust_binder_hooks.o	\
	page_range_helper.o

obj-$(CONFIG_ANDROID_BINDER_IPC_RUST_DUMMY) += rust_binder.o
rust_binder-$(CONFIG_ANDROID_BINDER_IPC_RUST_DUMMY) := dummy.o
