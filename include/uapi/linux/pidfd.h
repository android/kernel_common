/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */

#ifndef _UAPI_LINUX_PIDFD_H
#define _UAPI_LINUX_PIDFD_H

#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/ioctl.h>

/* Flags for pidfd_open().  */
#define PIDFD_NONBLOCK	O_NONBLOCK
#define PIDFD_THREAD	O_EXCL

/* Flags for pidfd_send_signal(). */
#define PIDFD_SIGNAL_THREAD		(1UL << 0)
#define PIDFD_SIGNAL_THREAD_GROUP	(1UL << 1)
#define PIDFD_SIGNAL_PROCESS_GROUP	(1UL << 2)

#define PIDFS_IOCTL_MAGIC 0xFF

#define PIDFD_GET_CGROUP_NAMESPACE            _IO(PIDFS_IOCTL_MAGIC, 1)
#define PIDFD_GET_IPC_NAMESPACE               _IO(PIDFS_IOCTL_MAGIC, 2)
#define PIDFD_GET_MNT_NAMESPACE               _IO(PIDFS_IOCTL_MAGIC, 3)
#define PIDFD_GET_NET_NAMESPACE               _IO(PIDFS_IOCTL_MAGIC, 4)
#define PIDFD_GET_PID_NAMESPACE               _IO(PIDFS_IOCTL_MAGIC, 5)
#define PIDFD_GET_PID_FOR_CHILDREN_NAMESPACE  _IO(PIDFS_IOCTL_MAGIC, 6)
#define PIDFD_GET_TIME_NAMESPACE              _IO(PIDFS_IOCTL_MAGIC, 7)
#define PIDFD_GET_TIME_FOR_CHILDREN_NAMESPACE _IO(PIDFS_IOCTL_MAGIC, 8)
#define PIDFD_GET_USER_NAMESPACE              _IO(PIDFS_IOCTL_MAGIC, 9)
#define PIDFD_GET_UTS_NAMESPACE               _IO(PIDFS_IOCTL_MAGIC, 10)

/*
 * The concept of process and threads in userland and the kernel is a confusing
 * one - within the kernel every thread is a 'task' with its own individual PID,
 * however from userland's point of view threads are grouped by a single PID,
 * which is that of the 'thread group leader', typically the first thread
 * spawned.
 *
 * To cut the Gideon knot, for internal kernel usage, we refer to
 * PIDFD_SELF_THREAD to refer to the current thread (or task from a kernel
 * perspective), and PIDFD_SELF_THREAD_GROUP to refer to the current thread
 * group leader...
 */
#define PIDFD_SELF_THREAD		-10000 /* Current thread. */
#define PIDFD_SELF_THREAD_GROUP		-20000 /* Current thread group leader. */

/*
 * ...and for userland we make life simpler - PIDFD_SELF refers to the current
 * thread, PIDFD_SELF_PROCESS refers to the process thread group leader.
 *
 * For nearly all practical uses, a user will want to use PIDFD_SELF.
 */
#define PIDFD_SELF		PIDFD_SELF_THREAD
#define PIDFD_SELF_PROCESS	PIDFD_SELF_THREAD_GROUP

#endif /* _UAPI_LINUX_PIDFD_H */
