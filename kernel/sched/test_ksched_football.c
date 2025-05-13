// SPDX-License-Identifier: GPL-2.0+
/*
 * Module-based test case for RT scheduling invariant
 *
 * A reimplementation of my old sched_football test
 * found in LTP:
 *   https://github.com/linux-test-project/ltp/blob/master/testcases/realtime/func/sched_football/sched_football.c
 *
 * Similar to that test, this tries to validate the RT
 * scheduling invariant, that the across N available cpus, the
 * top N priority tasks always running.
 *
 * This is done via having N offensive players that are
 * medium priority, which constantly are trying to increment the
 * ball_pos counter.
 *
 * Blocking this are N defensive players that are higher
 * priority which just spin on the cpu, preventing the medium
 * priority tasks from running.
 *
 * To complicate this, there are also N defensive low priority
 * tasks. These start first and each acquire one of N mutexes.
 * The high priority defense tasks will later try to grab the
 * mutexes and block, opening a window for the offensive tasks
 * to run and increment the ball. If priority inheritance or
 * proxy execution is used, the low priority defense players
 * should be boosted to the high priority levels, and will
 * prevent the mid priority offensive tasks from running.
 *
 * Copyright © International Business Machines  Corp., 2007, 2008
 * Copyright (C) Google, 2023, 2024
 *
 * Authors: John Stultz <jstultz@google.com>
 */

#define MODULE_NAME "ksched_football"
#define pr_fmt(fmt) MODULE_NAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched/rt.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/smp.h>
#include <linux/slab.h>
#include <uapi/linux/sched/types.h>
#include <linux/rtmutex.h>

MODULE_AUTHOR("John Stultz <jstultz@google.com>");
MODULE_DESCRIPTION("Test case for RT scheduling invariant");
MODULE_LICENSE("GPL");

atomic_t players_ready;
atomic_t ball_pos;
unsigned long players_per_team;
bool game_over;

#define DEF_GAME_TIME		10
#define CHECKIN_TIMEOUT		30

#define REF_PRIO		20
#define FAN_PRIO		15
#define DEF_HI_PRIO		10
#define OFF_PRIO		5
#define DEF_MID_PRIO		3
#define DEF_LOW_PRIO		2

#ifdef CONFIG_SCHED_PROXY_EXEC
struct mutex *mutex_low_list;
struct mutex *mutex_mid_list;
#define test_lock_init(x)	mutex_init(x)
#define TEST_LOCK_SIZE		sizeof(struct mutex)
#define test_lock(x)		mutex_lock(x)
#define test_unlock(x)		mutex_unlock(x)
#else
struct rt_mutex *mutex_low_list;
struct rt_mutex *mutex_mid_list;
#define test_lock_init(x)	rt_mutex_init(x)
#define TEST_LOCK_SIZE		sizeof(struct rt_mutex)
#define test_lock(x)		rt_mutex_lock(x)
#define test_unlock(x)		rt_mutex_unlock(x)
#endif

static struct task_struct *create_fifo_thread(int (*threadfn)(void *data),
					      void *data, char *name, int prio)
{
	struct task_struct *kth;
	struct sched_attr attr = {
		.size		= sizeof(struct sched_attr),
		.sched_policy	= SCHED_FIFO,
		.sched_nice	= 0,
		.sched_priority	= prio,
	};
	int ret;

	kth = kthread_create(threadfn, data, name);
	if (IS_ERR(kth)) {
		pr_warn("%s: Error, kthread_create %s failed\n", __func__,
			name);
		return kth;
	}
	ret = sched_setattr_nocheck(kth, &attr);
	if (ret) {
		kthread_stop(kth);
		pr_warn("%s: Error, failed to set SCHED_FIFO for %s\n",
			__func__, name);
		return ERR_PTR(ret);
	}

	wake_up_process(kth);
	return kth;
}

static int spawn_players(int (*threadfn)(void *data), char *name, int prio)
{
	unsigned long current_players, start, i;
	struct task_struct *kth;

	current_players = atomic_read(&players_ready);
	/* Create players_per_team threads */
	for (i = 0; i < players_per_team; i++) {
		kth = create_fifo_thread(threadfn, (void *)i, name, prio);
		if (IS_ERR(kth))
			return -1;
	}

	start = jiffies;
	/* Wait for players_per_team threads to check in */
	while (atomic_read(&players_ready) < current_players + players_per_team) {
		msleep(1);
		if (jiffies - start > CHECKIN_TIMEOUT * HZ) {
			pr_err("%s: Error, %s players took too long to checkin "
			       "(only %ld of %ld checked in)\n", __func__, name,
			       (long)atomic_read(&players_ready),
			       current_players + players_per_team);
			return -1;
		}
	}
	return 0;
}

static int defense_low_thread(void *arg)
{
	long tnum = (long)arg;

	atomic_inc(&players_ready);
	test_lock(&mutex_low_list[tnum]);
	while (!READ_ONCE(game_over)) {
		if (kthread_should_stop())
			break;
		schedule();
	}
	test_unlock(&mutex_low_list[tnum]);
	return 0;
}

static int defense_mid_thread(void *arg)
{
	long tnum = (long)arg;

	atomic_inc(&players_ready);
	test_lock(&mutex_mid_list[tnum]);
	test_lock(&mutex_low_list[tnum]);
	while (!READ_ONCE(game_over)) {
		if (kthread_should_stop())
			break;
		schedule();
	}
	test_unlock(&mutex_low_list[tnum]);
	test_unlock(&mutex_mid_list[tnum]);
	return 0;
}

static int offense_thread(void *arg)
{
	atomic_inc(&players_ready);
	while (!READ_ONCE(game_over)) {
		if (kthread_should_stop())
			break;
		schedule();
		atomic_inc(&ball_pos);
	}
	return 0;
}

static int defense_hi_thread(void *arg)
{
	long tnum = (long)arg;

	atomic_inc(&players_ready);
	test_lock(&mutex_mid_list[tnum]);
	while (!READ_ONCE(game_over)) {
		if (kthread_should_stop())
			break;
		schedule();
	}
	test_unlock(&mutex_mid_list[tnum]);
	return 0;
}

static int crazy_fan_thread(void *arg)
{
	atomic_inc(&players_ready);
	while (!READ_ONCE(game_over)) {
		if (kthread_should_stop())
			break;
		schedule();
		udelay(1000);
		msleep(2);
	}
	return 0;
}

struct completion referee_done;

static int referee_thread(void *arg)
{
	unsigned long game_time = (long)arg;
	unsigned long final_pos;

	WRITE_ONCE(game_over, false);
	pr_info("Started referee, game_time: %ld secs !\n", game_time);
	/* Create low  priority defensive team */
	if (spawn_players(defense_low_thread, "defense-low-thread", DEF_LOW_PRIO))
		goto out;

	if (spawn_players(defense_mid_thread, "defense-mid-thread", DEF_MID_PRIO))
		goto out;

	/* Create mid priority offensive team */
	if (spawn_players(offense_thread, "offense-thread", OFF_PRIO))
		goto out;

	/* Create high priority defensive team */
	if (spawn_players(defense_hi_thread, "defense-hi-thread", DEF_HI_PRIO))
		goto out;

	/* Create high priority crazy fan threads */
	if (spawn_players(crazy_fan_thread, "crazy-fan-thread", FAN_PRIO))
		goto out;
	pr_info("All players checked in! Starting game.\n");
	atomic_set(&ball_pos, 0);
	msleep(game_time * 1000);
	final_pos = atomic_read(&ball_pos);
	WRITE_ONCE(game_over, true);
	pr_info("Final ball_pos: %ld\n",  final_pos);
	WARN_ON(final_pos != 0);
out:
	pr_info("Game Over!\n");
	WRITE_ONCE(game_over, true);
	complete(&referee_done);
	return 0;
}

DEFINE_MUTEX(run_lock);

static int play_game(unsigned long game_time)
{
	struct task_struct *kth;
	int i, ret = -1;

#ifdef CONFIG_SCHED_PROXY_EXEC
	/* Avoid running test if PROXY_EXEC is built in, but off via cmdline */
	if (!sched_proxy_exec()) {
		pr_warn("CONFIG_SCHED_PROXY_EXEC=y but disabled via boot arg, skipping ksched_football tests, as they can hang");
		return -1;
	}
#endif

	if (!mutex_trylock(&run_lock)) {
		pr_err("Game already running\n");
		return -1;
	}

	players_per_team = num_online_cpus();

	mutex_low_list = kmalloc_array(players_per_team, TEST_LOCK_SIZE, GFP_ATOMIC);
	if (!mutex_low_list)
		goto out;

	mutex_mid_list = kmalloc_array(players_per_team, TEST_LOCK_SIZE, GFP_ATOMIC);
	if (!mutex_mid_list)
		goto out_mid_list;

	for (i = 0; i < players_per_team; i++) {
		test_lock_init(&mutex_low_list[i]);
		test_lock_init(&mutex_mid_list[i]);
	}

	atomic_set(&players_ready, 0);
	init_completion(&referee_done);
	kth = create_fifo_thread(referee_thread, (void *)game_time, "referee-thread", REF_PRIO);
	if (IS_ERR(kth))
		goto out_create_fifo;
	wait_for_completion(&referee_done);
	msleep(2000);
	ret = 0;

out_create_fifo:
	kfree(mutex_mid_list);
out_mid_list:
	kfree(mutex_low_list);
out:
	mutex_unlock(&run_lock);
	return ret;
}

static int game_length = DEF_GAME_TIME;

static ssize_t start_game_show(struct kobject *kobj, struct kobj_attribute *attr,
			       char *buf)
{
	return sysfs_emit(buf, "%d\n", game_length);
}

static ssize_t start_game_store(struct kobject *kobj, struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long len;
	int ret;

	ret = kstrtol(buf, 10, &len);
	if (ret < 0)
		return ret;

	if (len < DEF_GAME_TIME) {
		pr_warn("Game length can't be less then %i\n", DEF_GAME_TIME);
		len = DEF_GAME_TIME;
	}
	play_game(len);
	game_length = len;

	return count;
}

static struct kobj_attribute start_game_attribute =
	__ATTR(start_game, 0664, start_game_show, start_game_store);

static struct attribute *attrs[] = {
	&start_game_attribute.attr,
	NULL,   /* need to NULL terminate the list of attributes */
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static struct kobject *ksched_football_kobj;

static int __init test_ksched_football_init(void)
{
	int retval;

#ifdef CONFIG_SCHED_PROXY_EXEC
	/* Avoid running test if PROXY_EXEC is built in, but off via cmdline */
	if (!sched_proxy_exec()) {
		pr_warn("CONFIG_SCHED_PROXY_EXEC=y but disabled via boot arg, skipping ksched_football tests, as they can hang");
		return -1;
	}
#endif

	ksched_football_kobj = kobject_create_and_add("ksched_football", kernel_kobj);
	if (!ksched_football_kobj)
		return -ENOMEM;

	/* Create the files associated with this kobject */
	retval = sysfs_create_group(ksched_football_kobj, &attr_group);
	if (retval)
		kobject_put(ksched_football_kobj);

	return play_game(DEF_GAME_TIME);
}
module_init(test_ksched_football_init);
