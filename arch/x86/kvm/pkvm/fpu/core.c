// SPDX-License-Identifier: GPL-2.0-only
#include <asm/fpu/api.h>
#include <asm/fpu/types.h>

/* The FPU state configuration data for kernel and user space */
struct fpu_state_config fpu_kernel_cfg __ro_after_init;
struct fpu_state_config fpu_user_cfg __ro_after_init;
