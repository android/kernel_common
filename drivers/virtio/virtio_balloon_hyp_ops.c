// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/virtio.h>
#include <linux/virtio_balloon.h>

struct virtio_balloon_hyp_ops *virtio_balloon_hyp_ops;
EXPORT_SYMBOL_GPL(virtio_balloon_hyp_ops);
