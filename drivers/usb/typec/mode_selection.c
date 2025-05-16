// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Google LLC.
 */

#include "mode_selection.h"
#include "class.h"
#include "bus.h"

static int increment_duplicated_priority(struct device *dev, void *data)
{
	if (is_typec_altmode(dev)) {
		struct typec_altmode **alt_target = (struct typec_altmode **)data;
		struct typec_altmode *alt = to_typec_altmode(dev);

		if (alt != *alt_target && alt->priority == (*alt_target)->priority) {
			alt->priority++;
			*alt_target = alt;
			return 1;
		}
	}
	return 0;
}

static int find_duplicated_priority(struct device *dev, void *data)
{
	if (is_typec_altmode(dev)) {
		struct typec_altmode **alt_target = (struct typec_altmode **)data;
		struct typec_altmode *alt = to_typec_altmode(dev);

		if (alt != *alt_target && alt->priority == (*alt_target)->priority)
			return 1;
	}
	return 0;
}

int typec_mode_set_priority(struct typec_altmode *alt, const u8 priority)
{
	struct typec_port *port = to_typec_port(alt->dev.parent);
	const u8 old_priority = alt->priority;
	int res = 1;

	alt->priority = priority;
	while (res) {
		res = device_for_each_child(&port->dev, &alt, find_duplicated_priority);
		if (res) {
			alt->priority++;
			if (alt->priority == 0) {
				alt->priority = old_priority;
				return -EOVERFLOW;
			}
		}
	}

	res = 1;
	alt->priority = priority;
	while (res)
		res = device_for_each_child(&port->dev, &alt,
				increment_duplicated_priority);

	return 0;
}
