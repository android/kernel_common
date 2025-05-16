// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Google LLC.
 */

#include <linux/list_sort.h>

#include "mode_selection.h"
#include "class.h"
#include "bus.h"

/**
 * enum ms_state - Specific mode selection states
 * @MS_STATE_IDLE: The mode entry process has not started
 * @MS_STATE_INPROGRESS: The mode entry process is currently underway
 * @MS_STATE_FAILED: The mode driver reported the error
 */
enum ms_state {
	MS_STATE_IDLE = 0,
	MS_STATE_INPROGRESS,
	MS_STATE_FAILED,
};

/**
 * struct mode_state - State tracking for a specific Type-C mode
 * @altmode:
 * @priority: The mode priority. Lower values indicate a more preferred mode
 * @enter: Flag indicating if the driver is currently attempting to enter or
 * exit the mode
 * @state: The current mode selection state
 * @list: List head to link this mode state into a prioritized list
 */
struct mode_state {
	struct typec_altmode *altmode;
	u8 priority;
	enum ms_state state;
	struct list_head list;
};

struct altmode_selection {
	struct list_head mode_list;
	struct mutex mode_list_lock;
	struct delayed_work mode_selection_work;
	struct typec_partner *partner;

	unsigned int timeout;
	unsigned int delay;
};

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

static void mode_list_clean(struct altmode_selection *sel)
{
	struct mode_state *ms, *tmp;

	list_for_each_entry_safe(ms, tmp, &sel->mode_list, list) {
		list_del(&ms->list);
		kfree(ms);
	}
}

/**
 * mode_selection_next() - Process mode selection results and schedule next
 * action
 * @sel: pointer to the altmode_selection structure
 * @ms: pointer to active mode_state object that is on top in
 * mode_list.
 *
 * The mutex protecting mode_list must be held by the caller when invoking this
 * function.
 *
 * This function evaluates the outcome of the previous mode entry or exit
 * attempt. Based on this result, it determines the next mode to process and
 * schedules `mode_selection_work_fn()` if further actions are required.
 *
 * If the previous mode entry was successful, the mode selection sequence is
 * considered complete.
 *
 * If the previous mode entry failed, this function schedules
 * `mode_selection_work_fn()` to attempt exiting the mode that was partially
 * activated but not fully entered.
 *
 * If the previous operation was an exit (after a failed entry attempt),
 * the internal list of candidate modes is advanced to determine the next mode
 * to enter.
 */
static void mode_selection_next(struct altmode_selection *sel,
	struct mode_state *ms, const int error)

	__must_hold(&sel->mode_list_lock)
{
	if (ms->state == MS_STATE_FAILED) {
		list_del(&ms->list);
		kfree(ms);
	} else if (error) {
		ms->state = MS_STATE_FAILED;
		dev_dbg(&sel->partner->dev, "%s: entry error %pe\n",
				ms->altmode->desc, ERR_PTR(error));
	} else {
		dev_dbg(&sel->partner->dev, "%s altmode is active\n", ms->altmode->desc);
		mode_list_clean(sel);
	}

	if (!list_empty(&sel->mode_list))
		schedule_delayed_work(&sel->mode_selection_work,
			msecs_to_jiffies(sel->delay));
}

void typec_altmode_entry_complete(struct typec_altmode *altmode,
				const int error)
{
	const struct typec_altmode *pdev = typec_altmode_get_partner(altmode);
	struct altmode_selection *sel;
	struct mode_state *ms;

	if (!pdev)
		return;
	sel = to_typec_partner(pdev->dev.parent)->sel;

	mutex_lock(&sel->mode_list_lock);

	ms = list_first_entry_or_null(&sel->mode_list, struct mode_state, list);
	if (ms) {
		if (ms->altmode->svid == altmode->svid &&
			ms->state == MS_STATE_INPROGRESS) {
			cancel_delayed_work(&sel->mode_selection_work);
			mode_selection_next(sel, ms, error);
		}
	}

	mutex_unlock(&sel->mode_list_lock);
}
EXPORT_SYMBOL_GPL(typec_altmode_entry_complete);

/**
 * mode_selection_work_fn() - Activate entry into the upcoming mode
 * @work: work structure
 *
 * This function works in conjunction with `mode_selection_next()`.
 * It attempts to activate the next mode in the selection sequence.
 *
 * If the mode activation fails, `mode_selection_next()` will be called to
 * initiate a new selection cycle.
 *
 * Otherwise, the state is set to MS_STATE_INPROGRESS, and
 * `mode_selection_work_fn()` is scheduled for a subsequent entry after a timeout
 * period. The alternate mode driver is expected to call back with the actual
 * mode entry result. Upon this callback, `mode_selection_next()` will determine
 * the subsequent mode and re-schedule `mode_selection_work_fn()`.
 */
static void mode_selection_work_fn(struct work_struct *work)
{
	struct altmode_selection *sel = container_of(work, struct altmode_selection,
						  mode_selection_work.work);
	struct mode_state *ms;
	int error = 0;

	mutex_lock(&sel->mode_list_lock);

	ms = list_first_entry_or_null(
		&sel->mode_list, struct mode_state, list);
	if (ms) {
		if (ms->state == MS_STATE_IDLE) {
			error = ms->altmode->ops->activate(ms->altmode, 1);

			if (!error) {
				ms->state = MS_STATE_INPROGRESS;
				schedule_delayed_work(&sel->mode_selection_work,
					msecs_to_jiffies(sel->timeout));
				goto unlock_and_exit;
			}

			dev_dbg(&sel->partner->dev, "%s: activation error %pe\n",
					ms->altmode->desc, ERR_PTR(error));
			ms->state = MS_STATE_FAILED;
		} else if (ms->state == MS_STATE_FAILED)
			error = ms->altmode->ops->activate(ms->altmode, 0);
		else
			error = -ETIMEDOUT;

		mode_selection_next(sel, ms, error);
	}

unlock_and_exit:
	mutex_unlock(&sel->mode_list_lock);
}

static int compare_priorities(void *priv,
	const struct list_head *a, const struct list_head *b)
{
	struct mode_state *msa = container_of(a, struct mode_state, list);
	struct mode_state *msb = container_of(b, struct mode_state, list);

	if (msa->priority < msb->priority)
		return -1;
	return 1;
}

static int mode_add_to_list(struct device *dev, void *data)
{
	struct list_head *list = (struct list_head *)data;
	struct mode_state *ms;

	if (is_typec_altmode(dev)) {
		struct typec_altmode *altmode = to_typec_altmode(dev);
		const struct typec_altmode *pdev = typec_altmode_get_partner(altmode);

		if (pdev && altmode->ops && altmode->ops->activate) {
			ms = kzalloc(sizeof(struct mode_state), GFP_KERNEL);
			if (!ms)
				return -ENOMEM;

			ms->altmode = altmode;
			ms->priority = pdev->priority;
			INIT_LIST_HEAD(&ms->list);
			list_add_tail(&ms->list, list);
		}
	}

	return 0;
}

/**
 * typec_mode_selection_start() - Starts the mode selection process.
 * @partner: pointer to the partner structure
 *
 * This function populates mode_list with pointers to
 * `struct mode_state` instances. The sequence is prioritized
 * according to the port's settings.
 */
int typec_mode_selection_start(struct typec_partner *partner,
	const unsigned int delay, const unsigned int timeout)
{
	int ret;
	struct altmode_selection *sel;

	if (partner->sel)
		return -EALREADY;

	sel = kzalloc(sizeof(struct altmode_selection), GFP_KERNEL);
	if (!sel)
		return -ENOMEM;

	INIT_LIST_HEAD(&sel->mode_list);
	ret = device_for_each_child(
		&partner->dev, &sel->mode_list, mode_add_to_list);

	if (ret) {
		mode_list_clean(sel);
		return ret;
	}

	if (!list_empty(&sel->mode_list)) {
		partner->sel = sel;
		sel->partner = partner;
		sel->delay = delay;
		sel->timeout = timeout;

		list_sort(NULL, &sel->mode_list, compare_priorities);
		mutex_init(&sel->mode_list_lock);
		INIT_DELAYED_WORK(&sel->mode_selection_work, mode_selection_work_fn);
		schedule_delayed_work(&sel->mode_selection_work,
			msecs_to_jiffies(delay));
	}

	return 0;
}
EXPORT_SYMBOL_GPL(typec_mode_selection_start);

void typec_mode_selection_delete(struct typec_partner *partner)
{
	struct altmode_selection *sel = partner->sel;

	if (sel) {
		mutex_lock(&sel->mode_list_lock);
		mode_list_clean(sel);
		mutex_unlock(&sel->mode_list_lock);

		cancel_delayed_work_sync(&sel->mode_selection_work);
		mutex_destroy(&sel->mode_list_lock);
		kfree(sel);
		partner->sel = NULL;
	}
}
EXPORT_SYMBOL_GPL(typec_mode_selection_delete);
