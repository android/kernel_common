/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2022 Google LLC
 * Author: Keir Fraser <keirf@google.com>
 */

#ifndef _LINUX_VIRTIO_BALLOON_H
#define _LINUX_VIRTIO_BALLOON_H

#include <uapi/linux/virtio_balloon.h>

struct page;

#ifdef CONFIG_VIRTIO_BALLOON_HYP_OPS

struct virtio_balloon_hyp_ops {
	bool (*page_relinquish_disallowed)(void);
	void (*page_relinquish)(struct page *page, unsigned int nr);
};

extern struct virtio_balloon_hyp_ops *virtio_balloon_hyp_ops;

static inline bool page_relinquish_disallowed(void)
{
	if (virtio_balloon_hyp_ops &&
	    virtio_balloon_hyp_ops->page_relinquish_disallowed)
		return virtio_balloon_hyp_ops->page_relinquish_disallowed();
	return false;
}

static inline void page_relinquish(struct page *page, unsigned int nr)
{
	if (virtio_balloon_hyp_ops &&
	    virtio_balloon_hyp_ops->page_relinquish_disallowed)
		return virtio_balloon_hyp_ops->page_relinquish(page, nr);
}

#else	/* !CONFIG_VIRTIO_BALLOON_HYP_OPS */

static inline bool page_relinquish_disallowed(void) { return false; }
static inline void page_relinquish(struct page *page, unsigned int nr) { }

#endif	/* CONFIG_VIRTIO_BALLOON_HYP_OPS */

#endif	/* _LINUX_VIRTIO_BALLOON_H */
