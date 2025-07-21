#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/gcma.h>
#include "gcma_sysfs.h"

extern struct kobject *vendor_mm_kobj;
static struct kobject gcma_kobj;

static atomic64_t gcma_stats[NUM_OF_GCMA_STAT];

void gcma_stat_inc(enum gcma_stat_type type)
{
	atomic64_inc(&gcma_stats[type]);
}

void gcma_stat_dec(enum gcma_stat_type type)
{
	atomic64_dec(&gcma_stats[type]);
}

void gcma_stat_add(enum gcma_stat_type type, unsigned long delta)
{
	atomic64_add(delta, &gcma_stats[type]);
}

void gcma_stat_sub(enum gcma_stat_type type, unsigned long delta)
{
	atomic64_sub(delta, &gcma_stats[type]);
}

u64 gcma_stat_get(enum gcma_stat_type type)
{
	return (u64)atomic64_read(&gcma_stats[type]);
}
EXPORT_SYMBOL_GPL(gcma_stat_get);

/*
 * This all compiles without CONFIG_SYSFS, but is a waste of space.
 */

#define GCMA_ATTR_RO(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

static ssize_t allocated_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)atomic64_read(&gcma_stats[ALLOCATED_PAGE]));
}
GCMA_ATTR_RO(allocated);

static ssize_t stored_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)atomic64_read(&gcma_stats[STORED_PAGE]));
}
GCMA_ATTR_RO(stored);

static ssize_t loaded_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)atomic64_read(&gcma_stats[LOADED_PAGE]));
}
GCMA_ATTR_RO(loaded);

static ssize_t evicted_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)atomic64_read(&gcma_stats[EVICTED_PAGE]));
}
GCMA_ATTR_RO(evicted);

static ssize_t cached_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)atomic64_read(&gcma_stats[CACHED_PAGE]));
}
GCMA_ATTR_RO(cached);

static ssize_t discarded_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)atomic64_read(&gcma_stats[DISCARDED_PAGE]));
}
GCMA_ATTR_RO(discarded);

static ssize_t total_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)atomic64_read(&gcma_stats[TOTAL_PAGE]));
}
GCMA_ATTR_RO(total);

static struct attribute *gcma_attrs[] = {
	&allocated_attr.attr,
	&stored_attr.attr,
	&loaded_attr.attr,
	&evicted_attr.attr,
	&cached_attr.attr,
	&discarded_attr.attr,
	&total_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(gcma);

static void gcma_kobj_release(struct kobject *obj)
{
	/* Never released the static objects */
}

static struct kobj_type gcma_ktype = {
	.release = gcma_kobj_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = gcma_groups,
};

static int __init gcma_sysfs_init(void)
{
	return kobject_init_and_add(&gcma_kobj, &gcma_ktype, mm_kobj, "gcma");
}
subsys_initcall(gcma_sysfs_init);
