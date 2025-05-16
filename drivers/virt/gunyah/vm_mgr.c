// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "gunyah_vm_mgr: " fmt

#include <linux/anon_inodes.h>
#include <linux/compat.h>
#include <linux/file.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/xarray.h>

#include <uapi/linux/gunyah.h>
#include <trace/hooks/gunyah.h>

#include "rsc_mgr.h"
#include "vm_mgr.h"

#define GUNYAH_VM_ADDRSPACE_LABEL 0
// "To" extent for memory private to guest
#define GUNYAH_VM_MEM_EXTENT_GUEST_PRIVATE_LABEL 0
// "From" extent for memory shared with guest
#define GUNYAH_VM_MEM_EXTENT_HOST_SHARED_LABEL 1
// "To" extent for memory shared with the guest
#define GUNYAH_VM_MEM_EXTENT_GUEST_SHARED_LABEL 3
// "From" extent for memory private to guest
#define GUNYAH_VM_MEM_EXTENT_HOST_PRIVATE_LABEL 2

#define BOOT_CONTEXT_REG_MASK	GUNYAH_VM_BOOT_CONTEXT_REG(0xff, 0xff)

#define GUNYAH_EVENT_CREATE_VM 0
#define GUNYAH_EVENT_DESTROY_VM 1

static DEFINE_XARRAY(gunyah_vm_functions);
static DEFINE_XARRAY(gunyah_auth_vm_mgr);

static inline int gunyah_vm_fill_boot_context(struct gunyah_vm *ghvm)
{
	unsigned long reg_set, reg_index, id;
	void *entry;
	int ret;

	xa_for_each(&ghvm->boot_context, id, entry) {
		reg_set = (id >> GUNYAH_VM_BOOT_CONTEXT_REG_SHIFT) & 0xff;
		reg_index = id & 0xff;
		ret = gunyah_rm_vm_set_boot_context(ghvm->rm, ghvm->vmid,
						reg_set, reg_index,
						*(u64 *)entry);
		if (ret)
			return ret;
	}

	return 0;
}

int gunyah_auth_vm_mgr_register(struct gunyah_auth_vm_mgr *auth_vm)
{
	if (!auth_vm->vm_attach || !auth_vm->vm_detach)
		return -EINVAL;

	return xa_err(xa_store(&gunyah_auth_vm_mgr, auth_vm->type, auth_vm, GFP_KERNEL));
}
EXPORT_SYMBOL_GPL(gunyah_auth_vm_mgr_register);

void gunyah_auth_vm_mgr_unregister(struct gunyah_auth_vm_mgr *auth_vm)
{
	/* Expecting unregister to only come when unloading a module */
	WARN_ON(auth_vm->mod && module_refcount(auth_vm->mod));
	xa_erase(&gunyah_auth_vm_mgr, auth_vm->type);
}
EXPORT_SYMBOL_GPL(gunyah_auth_vm_mgr_unregister);

static struct gunyah_auth_vm_mgr *gunyah_get_auth_vm_mgr(u32 auth_type)
{
	struct gunyah_auth_vm_mgr *auth_vm;

	auth_vm = xa_load(&gunyah_auth_vm_mgr, auth_type);
	if (!auth_vm || !try_module_get(auth_vm->mod))
		auth_vm = ERR_PTR(-ENOENT);

	return auth_vm;
}

static void gunyah_put_auth_vm_mgr(struct gunyah_vm *ghvm)
{
	struct gunyah_auth_vm_mgr *auth_vm;

	auth_vm = xa_load(&gunyah_auth_vm_mgr, ghvm->auth);
	if (!auth_vm)
		return;

	auth_vm->vm_detach(ghvm);
	module_put(auth_vm->mod);
}

static long gunyah_vm_set_auth_type(struct gunyah_vm *ghvm,
				struct gunyah_auth_desc *auth_desc)
{
	struct gunyah_auth_vm_mgr *auth_vm;

	auth_vm = gunyah_get_auth_vm_mgr(auth_desc->type);
	if (IS_ERR(auth_vm))
		return PTR_ERR(auth_vm);

	/* The auth mgr should be populating the auth_vm_mgr_ops*/
	return auth_vm->vm_attach(ghvm, auth_desc);
}

static int gunyah_generic_pre_vm_configure(struct gunyah_vm *ghvm)
{
	/*
	 * VMs use dtb mem parcel as the config image parcel as they
	 * don't have any explicit auth image/metadata
	 */

	ghvm->config_image.parcel.start = gunyah_gpa_to_gfn(ghvm->dtb.config.guest_phys_addr);
	ghvm->config_image.parcel.pages = gunyah_gpa_to_gfn(ghvm->dtb.config.size);

	ghvm->config_image.image_offset = 0;
	ghvm->config_image.image_size = 0;
	ghvm->config_image.dtb_offset = ghvm->dtb.config.guest_phys_addr -
				gunyah_gfn_to_gpa(ghvm->config_image.parcel.start);
	ghvm->config_image.dtb_size = ghvm->dtb.config.size;
	return 0;
}

static int gunyah_generic_pre_vm_init(struct gunyah_vm *ghvm)
{
	int ret;

	ret = gunyah_setup_demand_paging(ghvm, 0, ULONG_MAX);
	if (ret) {
		dev_warn(ghvm->parent,
			"Failed to set up demand paging: %d\n", ret);
		return ret;
	}

	ret = gunyah_rm_vm_set_address_layout(
		ghvm->rm, ghvm->vmid, GUNYAH_RM_RANGE_ID_IMAGE,
		gunyah_gfn_to_gpa(ghvm->config_image.parcel.start),
		gunyah_gfn_to_gpa(ghvm->config_image.parcel.pages));
	if (ret) {
		dev_warn(ghvm->parent,
			"Failed to set location of the config image mem parcel: %d\n", ret);
		return ret;
	}

	return ret;
}

static int gunyah_generic_pre_vm_start(struct gunyah_vm *ghvm)
{
	int ret;

	ret = gunyah_vm_parcel_to_paged(ghvm, &ghvm->config_image.parcel.parcel,
				ghvm->config_image.parcel.start,
				ghvm->config_image.parcel.pages);
	if (ret)
		return ret;

	if (ghvm->auth != GUNYAH_RM_VM_AUTH_NONE)
		return ret;

	ret = gunyah_vm_fill_boot_context(ghvm);
	if (ret) {
		dev_warn(ghvm->parent, "Failed to setup boot context: %d\n",
			 ret);
		return ret;
	}
	return ret;
}

void gunyah_generic_vm_start_fail(struct gunyah_vm *ghvm)
{
	/**
	 * need to rollback parcel_to_paged because RM is still
	 * tracking the parcel
	 */
	gunyah_vm_mm_erase_range(ghvm,
		ghvm->config_image.parcel.start,
		ghvm->config_image.parcel.pages);
}

static struct gunyah_auth_vm_mgr_ops generic_vm_ops = {
	.pre_vm_configure = gunyah_generic_pre_vm_configure,
	.pre_vm_init = gunyah_generic_pre_vm_init,
	.pre_vm_start = gunyah_generic_pre_vm_start,
	.vm_start_fail = gunyah_generic_vm_start_fail,
};

static void gunyah_vm_put_function(struct gunyah_vm_function *fn)
{
	module_put(fn->mod);
}

static struct gunyah_vm_function *gunyah_vm_get_function(u32 type)
{
	struct gunyah_vm_function *fn;

	fn = xa_load(&gunyah_vm_functions, type);
	if (!fn) {
		request_module("ghfunc:%d", type);

		fn = xa_load(&gunyah_vm_functions, type);
	}

	if (!fn || !try_module_get(fn->mod))
		fn = ERR_PTR(-ENOENT);

	return fn;
}

static void
gunyah_vm_remove_function_instance(struct gunyah_vm_function_instance *inst)
	__must_hold(&inst->ghvm->fn_lock)
{
	inst->fn->unbind(inst);
	list_del(&inst->vm_list);
	gunyah_vm_put_function(inst->fn);
	kfree(inst->argp);
	kfree(inst);
}

static void gunyah_vm_remove_functions(struct gunyah_vm *ghvm)
{
	struct gunyah_vm_function_instance *inst, *iiter;

	mutex_lock(&ghvm->fn_lock);
	list_for_each_entry_safe(inst, iiter, &ghvm->functions, vm_list) {
		gunyah_vm_remove_function_instance(inst);
	}
	mutex_unlock(&ghvm->fn_lock);
}

static long gunyah_vm_add_function_instance(struct gunyah_vm *ghvm,
					    struct gunyah_fn_desc *f)
{
	struct gunyah_vm_function_instance *inst;
	void __user *argp;
	long r = 0;

	if (f->arg_size > GUNYAH_FN_MAX_ARG_SIZE) {
		dev_err_ratelimited(ghvm->parent, "%s: arg_size > %d\n",
				    __func__, GUNYAH_FN_MAX_ARG_SIZE);
		return -EINVAL;
	}

	inst = kzalloc(sizeof(*inst), GFP_KERNEL);
	if (!inst)
		return -ENOMEM;

	inst->arg_size = f->arg_size;
	if (inst->arg_size) {
		inst->argp = kzalloc(inst->arg_size, GFP_KERNEL);
		if (!inst->argp) {
			r = -ENOMEM;
			goto free;
		}

		argp = u64_to_user_ptr(f->arg);
		if (copy_from_user(inst->argp, argp, f->arg_size)) {
			r = -EFAULT;
			goto free_arg;
		}
	}

	inst->fn = gunyah_vm_get_function(f->type);
	if (IS_ERR(inst->fn)) {
		r = PTR_ERR(inst->fn);
		goto free_arg;
	}

	inst->ghvm = ghvm;
	inst->rm = ghvm->rm;

	mutex_lock(&ghvm->fn_lock);
	r = inst->fn->bind(inst);
	if (r < 0) {
		mutex_unlock(&ghvm->fn_lock);
		gunyah_vm_put_function(inst->fn);
		goto free_arg;
	}

	list_add(&inst->vm_list, &ghvm->functions);
	mutex_unlock(&ghvm->fn_lock);

	return r;
free_arg:
	kfree(inst->argp);
free:
	kfree(inst);
	return r;
}

static long gunyah_vm_rm_function_instance(struct gunyah_vm *ghvm,
					   struct gunyah_fn_desc *f)
{
	struct gunyah_vm_function_instance *inst, *iter;
	void __user *user_argp;
	void *argp __free(kfree) = NULL;
	long r = 0;

	if (f->arg_size) {
		argp = kzalloc(f->arg_size, GFP_KERNEL);
		if (!argp)
			return -ENOMEM;

		user_argp = u64_to_user_ptr(f->arg);
		if (copy_from_user(argp, user_argp, f->arg_size))
			return -EFAULT;
	}

	r = mutex_lock_interruptible(&ghvm->fn_lock);
	if (r)
		return r;

	r = -ENOENT;
	list_for_each_entry_safe(inst, iter, &ghvm->functions, vm_list) {
		if (inst->fn->type == f->type &&
		    inst->fn->compare(inst, argp, f->arg_size)) {
			gunyah_vm_remove_function_instance(inst);
			r = 0;
		}
	}

	mutex_unlock(&ghvm->fn_lock);
	return r;
}

int gunyah_vm_function_register(struct gunyah_vm_function *fn)
{
	if (!fn->bind || !fn->unbind)
		return -EINVAL;

	return xa_err(xa_store(&gunyah_vm_functions, fn->type, fn, GFP_KERNEL));
}
EXPORT_SYMBOL_GPL(gunyah_vm_function_register);

void gunyah_vm_function_unregister(struct gunyah_vm_function *fn)
{
	/* Expecting unregister to only come when unloading a module */
	WARN_ON(fn->mod && module_refcount(fn->mod));
	xa_erase(&gunyah_vm_functions, fn->type);
}
EXPORT_SYMBOL_GPL(gunyah_vm_function_unregister);

static bool gunyah_vm_resource_ticket_populate_noop(
	struct gunyah_vm_resource_ticket *ticket, struct gunyah_resource *ghrsc)
{
	return true;
}
static void gunyah_vm_resource_ticket_unpopulate_noop(
	struct gunyah_vm_resource_ticket *ticket, struct gunyah_resource *ghrsc)
{
}

int gunyah_vm_add_resource_ticket(struct gunyah_vm *ghvm,
				  struct gunyah_vm_resource_ticket *ticket)
{
	struct gunyah_vm_resource_ticket *iter;
	struct gunyah_resource *ghrsc, *rsc_iter;
	int ret = 0;

	mutex_lock(&ghvm->resources_lock);
	list_for_each_entry(iter, &ghvm->resource_tickets, vm_list) {
		if (iter->resource_type == ticket->resource_type &&
		    iter->label == ticket->label) {
			ret = -EEXIST;
			goto out;
		}
	}

	if (!try_module_get(ticket->owner)) {
		ret = -ENODEV;
		goto out;
	}

	list_add(&ticket->vm_list, &ghvm->resource_tickets);
	INIT_LIST_HEAD(&ticket->resources);

	list_for_each_entry_safe(ghrsc, rsc_iter, &ghvm->resources, list) {
		if (ghrsc->type == ticket->resource_type &&
		    ghrsc->rm_label == ticket->label) {
			if (ticket->populate(ticket, ghrsc))
				list_move(&ghrsc->list, &ticket->resources);
		}
	}
out:
	mutex_unlock(&ghvm->resources_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gunyah_vm_add_resource_ticket);

static void
__gunyah_vm_remove_resource_ticket(struct gunyah_vm *ghvm,
				   struct gunyah_vm_resource_ticket *ticket)
{
	struct gunyah_resource *ghrsc, *iter;

	list_for_each_entry_safe(ghrsc, iter, &ticket->resources, list) {
		ticket->unpopulate(ticket, ghrsc);
		list_move(&ghrsc->list, &ghvm->resources);
	}

	module_put(ticket->owner);
	list_del(&ticket->vm_list);
}

void gunyah_vm_remove_resource_ticket(struct gunyah_vm *ghvm,
				      struct gunyah_vm_resource_ticket *ticket)
{
	mutex_lock(&ghvm->resources_lock);
	__gunyah_vm_remove_resource_ticket(ghvm, ticket);
	mutex_unlock(&ghvm->resources_lock);
}
EXPORT_SYMBOL_GPL(gunyah_vm_remove_resource_ticket);

static void gunyah_vm_add_resource(struct gunyah_vm *ghvm,
				   struct gunyah_resource *ghrsc)
{
	struct gunyah_vm_resource_ticket *ticket;

	mutex_lock(&ghvm->resources_lock);
	list_for_each_entry(ticket, &ghvm->resource_tickets, vm_list) {
		if (ghrsc->type == ticket->resource_type &&
		    ghrsc->rm_label == ticket->label) {
			if (ticket->populate(ticket, ghrsc))
				list_add(&ghrsc->list, &ticket->resources);
			else
				list_add(&ghrsc->list, &ghvm->resources);
			/* unconditonal -- we prevent multiple identical
			 * resource tickets so there will not be some other
			 * ticket elsewhere in the list if populate() failed.
			 */
			goto found;
		}
	}
	list_add(&ghrsc->list, &ghvm->resources);
found:
	mutex_unlock(&ghvm->resources_lock);
}

static void gunyah_vm_clean_resources(struct gunyah_vm *ghvm)
{
	struct gunyah_vm_resource_ticket *ticket, *titer;
	struct gunyah_resource *ghrsc, *riter;

	mutex_lock(&ghvm->resources_lock);
	if (!list_empty(&ghvm->resource_tickets)) {
		dev_warn(ghvm->parent, "Dangling resource tickets:\n");
		list_for_each_entry_safe(ticket, titer, &ghvm->resource_tickets,
					 vm_list) {
			dev_warn(ghvm->parent, "  %pS\n", ticket->populate);
			__gunyah_vm_remove_resource_ticket(ghvm, ticket);
		}
	}

	list_for_each_entry_safe(ghrsc, riter, &ghvm->resources, list) {
		gunyah_rm_free_resource(ghrsc);
	}
	mutex_unlock(&ghvm->resources_lock);
}

static int _gunyah_vm_io_handler_compare(const struct rb_node *node,
					 const struct rb_node *parent)
{
	struct gunyah_vm_io_handler *n =
		container_of(node, struct gunyah_vm_io_handler, node);
	struct gunyah_vm_io_handler *p =
		container_of(parent, struct gunyah_vm_io_handler, node);

	if (n->addr < p->addr)
		return -1;
	if (n->addr > p->addr)
		return 1;
	if ((n->len && !p->len) || (!n->len && p->len))
		return 0;
	if (n->len < p->len)
		return -1;
	if (n->len > p->len)
		return 1;
	/* one of the io handlers doesn't have datamatch and the other does.
	 * For purposes of comparison, that makes them identical since the
	 * one that doesn't have datamatch will cover the same handler that
	 * does.
	 */
	if (n->datamatch != p->datamatch)
		return 0;
	if (n->data < p->data)
		return -1;
	if (n->data > p->data)
		return 1;
	return 0;
}

static int gunyah_vm_io_handler_compare(struct rb_node *node,
					const struct rb_node *parent)
{
	return _gunyah_vm_io_handler_compare(node, parent);
}

static int gunyah_vm_io_handler_find(const void *key,
				     const struct rb_node *node)
{
	const struct gunyah_vm_io_handler *k = key;

	return _gunyah_vm_io_handler_compare(&k->node, node);
}

static struct gunyah_vm_io_handler *
gunyah_vm_mgr_find_io_hdlr(struct gunyah_vm *ghvm, u64 addr, u64 len, u64 data)
{
	struct gunyah_vm_io_handler key = {
		.addr = addr,
		.len = len,
		.datamatch = true,
		.data = data,
	};
	struct rb_node *node;

	node = rb_find(&key, &ghvm->mmio_handler_root,
		       gunyah_vm_io_handler_find);
	if (!node)
		return NULL;

	return container_of(node, struct gunyah_vm_io_handler, node);
}

int gunyah_vm_mmio_write(struct gunyah_vm *ghvm, u64 addr, u32 len, u64 data)
{
	struct gunyah_vm_io_handler *io_hdlr = NULL;
	int ret;

	down_read(&ghvm->mmio_handler_lock);
	io_hdlr = gunyah_vm_mgr_find_io_hdlr(ghvm, addr, len, data);
	if (!io_hdlr || !io_hdlr->ops || !io_hdlr->ops->write) {
		ret = -ENOENT;
		goto out;
	}

	ret = io_hdlr->ops->write(io_hdlr, addr, len, data);

out:
	up_read(&ghvm->mmio_handler_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gunyah_vm_mmio_write);

int gunyah_vm_add_io_handler(struct gunyah_vm *ghvm,
			     struct gunyah_vm_io_handler *io_hdlr)
{
	struct rb_node *found;

	if (io_hdlr->datamatch &&
	    (!io_hdlr->len || io_hdlr->len > sizeof(io_hdlr->data)))
		return -EINVAL;

	down_write(&ghvm->mmio_handler_lock);
	found = rb_find_add(&io_hdlr->node, &ghvm->mmio_handler_root,
			    gunyah_vm_io_handler_compare);
	up_write(&ghvm->mmio_handler_lock);

	return found ? -EEXIST : 0;
}
EXPORT_SYMBOL_GPL(gunyah_vm_add_io_handler);

void gunyah_vm_remove_io_handler(struct gunyah_vm *ghvm,
				 struct gunyah_vm_io_handler *io_hdlr)
{
	down_write(&ghvm->mmio_handler_lock);
	rb_erase(&io_hdlr->node, &ghvm->mmio_handler_root);
	up_write(&ghvm->mmio_handler_lock);
}
EXPORT_SYMBOL_GPL(gunyah_vm_remove_io_handler);

static int gunyah_vm_rm_notification_status(struct gunyah_vm *ghvm, void *data)
{
	struct gunyah_rm_vm_status_payload *payload = data;

	if (le16_to_cpu(payload->vmid) != ghvm->vmid)
		return NOTIFY_OK;

	/* All other state transitions are synchronous to a corresponding RM call */
	if (payload->vm_status == GUNYAH_RM_VM_STATUS_RESET) {
		down_write(&ghvm->status_lock);
		ghvm->vm_status = payload->vm_status;
		up_write(&ghvm->status_lock);
		wake_up(&ghvm->vm_status_wait);
	}

	return NOTIFY_DONE;
}

static int gunyah_vm_rm_notification_exited(struct gunyah_vm *ghvm, void *data)
{
	struct gunyah_rm_vm_exited_payload *payload = data;

	if (le16_to_cpu(payload->vmid) != ghvm->vmid)
		return NOTIFY_OK;

	down_write(&ghvm->status_lock);
	ghvm->vm_status = GUNYAH_RM_VM_STATUS_EXITED;
	ghvm->exit_info.type = le16_to_cpu(payload->exit_type);
	ghvm->exit_info.reason_size = le32_to_cpu(payload->exit_reason_size);
	memcpy(&ghvm->exit_info.reason, payload->exit_reason,
	       min(GUNYAH_VM_MAX_EXIT_REASON_SIZE,
		   ghvm->exit_info.reason_size));
	up_write(&ghvm->status_lock);
	wake_up(&ghvm->vm_status_wait);

	return NOTIFY_DONE;
}

static int gunyah_vm_rm_notification(struct notifier_block *nb,
				     unsigned long action, void *data)
{
	struct gunyah_vm *ghvm = container_of(nb, struct gunyah_vm, nb);

	switch (action) {
	case GUNYAH_RM_NOTIFICATION_VM_STATUS:
		return gunyah_vm_rm_notification_status(ghvm, data);
	case GUNYAH_RM_NOTIFICATION_VM_EXITED:
		return gunyah_vm_rm_notification_exited(ghvm, data);
	default:
		return NOTIFY_OK;
	}
}

static void gunyah_uevent_notify_change(unsigned int type, struct gunyah_vm *ghvm)
{
	struct kobj_uevent_env *env;

	env = kzalloc(sizeof(*env), GFP_KERNEL_ACCOUNT);
	if (!env)
		return;

	if (type == GUNYAH_EVENT_CREATE_VM)
		add_uevent_var(env, "EVENT=create");
	else if (type == GUNYAH_EVENT_DESTROY_VM) {
		add_uevent_var(env, "EVENT=destroy");
		add_uevent_var(env, "vm_exit=%d", ghvm->exit_info.type);
	}

	add_uevent_var(env, "vm_id=%hu", ghvm->vmid);
	env->envp[env->envp_idx++] = NULL;
	kobject_uevent_env(&ghvm->parent->kobj, KOBJ_CHANGE, env->envp);
	kfree(env);
}

static void gunyah_vm_stop(struct gunyah_vm *ghvm)
{
	int ret;

	if (ghvm->vm_status == GUNYAH_RM_VM_STATUS_RUNNING) {
		ret = gunyah_rm_vm_stop(ghvm->rm, ghvm->vmid);
		if (ret)
			dev_warn(ghvm->parent, "Failed to stop VM: %d\n", ret);
	}

	wait_event(ghvm->vm_status_wait,
		   ghvm->vm_status != GUNYAH_RM_VM_STATUS_RUNNING);
}

static inline void setup_extent_ticket(struct gunyah_vm *ghvm,
				       struct gunyah_vm_resource_ticket *ticket,
				       u32 label)
{
	ticket->resource_type = GUNYAH_RESOURCE_TYPE_MEM_EXTENT;
	ticket->label = label;
	ticket->populate = gunyah_vm_resource_ticket_populate_noop;
	ticket->unpopulate = gunyah_vm_resource_ticket_unpopulate_noop;
	gunyah_vm_add_resource_ticket(ghvm, ticket);
}

static __must_check struct gunyah_vm *gunyah_vm_alloc(struct gunyah_rm *rm)
{
	struct gunyah_vm *ghvm;

	ghvm = kzalloc(sizeof(*ghvm), GFP_KERNEL);
	if (!ghvm)
		return ERR_PTR(-ENOMEM);

	ghvm->parent = gunyah_rm_get(rm);
	ghvm->vmid = GUNYAH_VMID_INVAL;
	ghvm->rm = rm;

	mmgrab(current->mm);
	ghvm->mm_s = current->mm;
	init_rwsem(&ghvm->status_lock);
	init_waitqueue_head(&ghvm->vm_status_wait);
	kref_init(&ghvm->kref);
	ghvm->vm_status = GUNYAH_RM_VM_STATUS_NO_STATE;

	INIT_LIST_HEAD(&ghvm->functions);
	mutex_init(&ghvm->fn_lock);
	mutex_init(&ghvm->resources_lock);
	INIT_LIST_HEAD(&ghvm->resources);
	INIT_LIST_HEAD(&ghvm->resource_tickets);
	xa_init(&ghvm->boot_context);

	init_rwsem(&ghvm->mmio_handler_lock);
	ghvm->mmio_handler_root = RB_ROOT;

	mt_init(&ghvm->mm);
	mt_init(&ghvm->bindings);
	init_rwsem(&ghvm->bindings_lock);

	ghvm->addrspace_ticket.resource_type = GUNYAH_RESOURCE_TYPE_ADDR_SPACE;
	ghvm->addrspace_ticket.label = GUNYAH_VM_ADDRSPACE_LABEL;
	ghvm->addrspace_ticket.populate =
		gunyah_vm_resource_ticket_populate_noop;
	ghvm->addrspace_ticket.unpopulate =
		gunyah_vm_resource_ticket_unpopulate_noop;
	gunyah_vm_add_resource_ticket(ghvm, &ghvm->addrspace_ticket);

	setup_extent_ticket(ghvm, &ghvm->host_private_extent_ticket,
			    GUNYAH_VM_MEM_EXTENT_HOST_PRIVATE_LABEL);
	setup_extent_ticket(ghvm, &ghvm->host_shared_extent_ticket,
			    GUNYAH_VM_MEM_EXTENT_HOST_SHARED_LABEL);
	setup_extent_ticket(ghvm, &ghvm->guest_private_extent_ticket,
			    GUNYAH_VM_MEM_EXTENT_GUEST_PRIVATE_LABEL);
	setup_extent_ticket(ghvm, &ghvm->guest_shared_extent_ticket,
			    GUNYAH_VM_MEM_EXTENT_GUEST_SHARED_LABEL);

	ghvm->auth = GUNYAH_RM_VM_AUTH_NONE;
	ghvm->auth_vm_mgr_ops = &generic_vm_ops;

	return ghvm;
}

static long gunyah_vm_set_boot_context(struct gunyah_vm *ghvm,
				       struct gunyah_vm_boot_context *boot_ctx)
{
	u8 reg_set, reg_index; /* to check values are reasonable */
	u64 *value;
	int ret;

	if (boot_ctx->reg & ~BOOT_CONTEXT_REG_MASK)
		return -EINVAL;

	reg_set = (boot_ctx->reg >> GUNYAH_VM_BOOT_CONTEXT_REG_SHIFT) & 0xff;
	reg_index = boot_ctx->reg & 0xff;

	switch (reg_set) {
	case REG_SET_X:
		if (reg_index > 31)
			return -EINVAL;
		break;
	case REG_SET_PC:
		if (reg_index)
			return -EINVAL;
		break;
	case REG_SET_SP:
		if (reg_index > 2)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	ret = down_read_interruptible(&ghvm->status_lock);
	if (ret)
		return ret;

	if (ghvm->vm_status != GUNYAH_RM_VM_STATUS_NO_STATE) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * allocate memory for the value because xarray supports [0, LONG_MAX]
	 * for values and we want [0, ULONG_MAX]
	 */
	value = kmalloc(sizeof(*value), GFP_KERNEL);
	if (!value) {
		ret = -ENOMEM;
		goto out;
	}
	*value = boot_ctx->value;

	ret = xa_err(xa_store(&ghvm->boot_context, boot_ctx->reg,
			      value, GFP_KERNEL));
	if (ret)
		kfree(value);
out:
	up_read(&ghvm->status_lock);
	return ret;
}

static int gunyah_vm_start(struct gunyah_vm *ghvm)
{
	struct gunyah_rm_hyp_resources *resources;
	struct gunyah_resource *ghrsc;
	int ret, i, n;
	u16 vmid = 0;

	down_write(&ghvm->status_lock);
	if (ghvm->vm_status != GUNYAH_RM_VM_STATUS_NO_STATE) {
		up_write(&ghvm->status_lock);
		return 0;
	}

	ghvm->nb.notifier_call = gunyah_vm_rm_notification;
	ret = gunyah_rm_notifier_register(ghvm->rm, &ghvm->nb);
	if (ret)
		goto err;

	ret = gunyah_vm_pre_alloc_vmid(ghvm);
	if (ret)
		vmid = ret;

	ret = gunyah_rm_alloc_vmid(ghvm->rm, vmid);
	if (ret < 0)
		goto err_rm_notifier_unregister;

	ghvm->vmid = vmid ? vmid : ret;
	gunyah_uevent_notify_change(GUNYAH_EVENT_CREATE_VM, ghvm);

	ret = gunyah_vm_pre_vm_configure(ghvm);
	if (ret)
		goto err_dealloc_vmid;

	if (ghvm->fw.config.size > 0) {
		ghvm->fw.parcel.start = gunyah_gpa_to_gfn(ghvm->fw.config.guest_phys_addr);
		ghvm->fw.parcel.pages = gunyah_gpa_to_gfn(ghvm->fw.config.size);
		ret = gunyah_share_parcel(ghvm, &ghvm->fw.parcel,
				&ghvm->fw.parcel.start,
				&ghvm->fw.parcel.pages);
		if (ret) {
			dev_warn(ghvm->parent,
				"Failed to share parcel for the fw: %d\n", ret);
			goto err_dealloc_vmid;
		}
	}

	ghvm->vm_status = GUNYAH_RM_VM_STATUS_LOAD;

	ret = gunyah_share_parcel(ghvm, &ghvm->config_image.parcel,
					&ghvm->config_image.parcel.start,
					&ghvm->config_image.parcel.pages);
	if (ret) {
		dev_warn(ghvm->parent,
			 "Failed to allocate parcel for the config image: %d\n", ret);
		goto err;
	}

	ret = gunyah_rm_vm_configure(ghvm->rm, ghvm->vmid, ghvm->auth,
				     ghvm->config_image.parcel.parcel.mem_handle,
				     ghvm->config_image.image_offset,
				     ghvm->config_image.image_size,
				     ghvm->config_image.dtb_offset,
				     ghvm->config_image.dtb_size);
	if (ret) {
		dev_warn(ghvm->parent, "Failed to configure VM: %d\n", ret);
		goto err;
	}

	ret = gunyah_vm_authenticate(ghvm);
	if (ret)
		goto err;

	if (ghvm->fw.config.size > 0) {
		ret = gunyah_rm_vm_set_firmware_mem(ghvm->rm, ghvm->vmid, &ghvm->fw.parcel.parcel,
			ghvm->fw.config.guest_phys_addr - (ghvm->fw.parcel.start << PAGE_SHIFT),
			ghvm->fw.config.size);
		if (ret) {
			pr_warn("Failed to configure firmware\n");
			goto err;
		}
	}

	ret = gunyah_vm_pre_vm_init(ghvm);
	if (ret)
		goto err;

	ret = gunyah_rm_vm_init(ghvm->rm, ghvm->vmid);
	if (ret) {
		ghvm->vm_status = GUNYAH_RM_VM_STATUS_INIT_FAILED;
		dev_warn(ghvm->parent, "Failed to initialize VM: %d\n", ret);
		goto err;
	}
	ghvm->vm_status = GUNYAH_RM_VM_STATUS_READY;

	ret = gunyah_rm_get_hyp_resources(ghvm->rm, ghvm->vmid, &resources);
	if (ret) {
		dev_warn(ghvm->parent,
			 "Failed to get hypervisor resources for VM: %d\n",
			 ret);
		goto err;
	}

	for (i = 0, n = le32_to_cpu(resources->n_entries); i < n; i++) {
		ghrsc = gunyah_rm_alloc_resource(ghvm->rm,
						 &resources->entries[i]);
		if (!ghrsc) {
			ret = -ENOMEM;
			goto err;
		}

		gunyah_vm_add_resource(ghvm, ghrsc);
	}

	ret = gunyah_vm_pre_vm_start(ghvm);
	if (ret)
		goto err;

	ret = gunyah_rm_vm_start(ghvm->rm, ghvm->vmid);
	if (ret) {
		dev_warn(ghvm->parent, "Failed to start VM: %d\n", ret);
		goto err;
	}

	ghvm->vm_status = GUNYAH_RM_VM_STATUS_RUNNING;
	up_write(&ghvm->status_lock);
	return ret;
err_dealloc_vmid:
	ret = gunyah_rm_dealloc_vmid(ghvm->rm, ghvm->vmid);
	if (ret)
		dev_warn(ghvm->parent,
			 "Failed to deallocate vmid: %d\n", ret);
err_rm_notifier_unregister:
	gunyah_rm_notifier_unregister(ghvm->rm, &ghvm->nb);
err:
	/* gunyah_vm_free will handle releasing resources and reclaiming memory */
	gunyah_vm_start_fail(ghvm);
	up_write(&ghvm->status_lock);
	return ret;
}

static int gunyah_vm_ensure_started(struct gunyah_vm *ghvm)
{
	int ret;

	ret = down_read_interruptible(&ghvm->status_lock);
	if (ret)
		return ret;

	/* Unlikely because VM is typically started */
	if (unlikely(ghvm->vm_status == GUNYAH_RM_VM_STATUS_NO_STATE)) {
		up_read(&ghvm->status_lock);
		ret = gunyah_vm_start(ghvm);
		if (ret)
			return ret;
		ret = down_read_interruptible(&ghvm->status_lock);
		if (ret)
			return ret;
	}

	/* Unlikely because VM is typically running */
	if (unlikely(ghvm->vm_status != GUNYAH_RM_VM_STATUS_RUNNING))
		ret = -ENODEV;

	up_read(&ghvm->status_lock);
	return ret;
}

static long gunyah_vm_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	struct gunyah_vm *ghvm = filp->private_data;
	void __user *argp = (void __user *)arg;
	long r;
	bool lend = false;

	switch (cmd) {
	case GUNYAH_VM_SET_DTB_CONFIG: {
		struct gunyah_vm_dtb_config dtb_config;

		if (copy_from_user(&dtb_config, argp, sizeof(dtb_config)))
			return -EFAULT;

		if (overflows_type(dtb_config.guest_phys_addr + dtb_config.size,
				   u64))
			return -EOVERFLOW;

		ghvm->dtb.config = dtb_config;

		r = 0;
		break;
	}
	case GH_VM_ANDROID_SET_FW_CONFIG: {
		struct gunyah_vm_firmware_config fw_config;

		if (copy_from_user(&fw_config, argp, sizeof(fw_config)))
			return -EFAULT;

		if (overflows_type(fw_config.guest_phys_addr + fw_config.size,
							u64))
			return -EOVERFLOW;

		ghvm->fw.config = fw_config;
		/* Set new auth type only if type was not set until now */
		if (ghvm->auth == GUNYAH_RM_VM_AUTH_NONE)
			ghvm->auth = GUNYAH_RM_VM_AUTH_QCOM_ANDROID_PVM;

		r = 0;
		break;
	}
	case GUNYAH_VM_START: {
		r = gunyah_vm_ensure_started(ghvm);
		break;
	}
	case GUNYAH_VM_ADD_FUNCTION: {
		struct gunyah_fn_desc f;

		if (copy_from_user(&f, argp, sizeof(f)))
			return -EFAULT;

		r = gunyah_vm_add_function_instance(ghvm, &f);
		break;
	}
	case GUNYAH_VM_REMOVE_FUNCTION: {
		struct gunyah_fn_desc f;

		if (copy_from_user(&f, argp, sizeof(f)))
			return -EFAULT;

		r = gunyah_vm_rm_function_instance(ghvm, &f);
		break;
	}
	case GH_VM_ANDROID_LEND_USER_MEM:
		lend = true;
		fallthrough;
	case GH_VM_SET_USER_MEM_REGION: {
		struct gunyah_userspace_memory_region region;

		/* only allow owner task to add memory */
		if (ghvm->mm_s != current->mm)
			return -EPERM;
		if (copy_from_user(&region, argp, sizeof(region)))
			return -EFAULT;

		if (region.flags & ~(GUNYAH_MEM_ALLOW_READ |
					GUNYAH_MEM_ALLOW_WRITE |
					GUNYAH_MEM_ALLOW_EXEC))
			return -EINVAL;

		r = gunyah_vm_binding_alloc(ghvm, &region, lend);
		break;
	}
	case GH_VM_RECLAIM_REGION: {
		struct gunyah_address_range range;

		/* only allow owner task to remove memory */
		if (ghvm->mm_s != current->mm)
			return -EPERM;
		if (copy_from_user(&range, argp, sizeof(range)))
			return -EFAULT;
		if (!PAGE_ALIGNED(range.size) || !PAGE_ALIGNED(range.guest_phys_addr))
			return -EINVAL;

		r = gunyah_vm_reclaim_range(ghvm,
					    gunyah_gpa_to_gfn(range.guest_phys_addr),
					    gunyah_gpa_to_gfn(range.size) - 1);
		break;
	}
	case GH_VM_ANDROID_MAP_CMA_MEM: {
		struct gunyah_map_cma_mem_args cma_mem;

		/* only allow owner task to add memory */
		if (ghvm->mm_s != current->mm)
			return -EPERM;

		if (copy_from_user(&cma_mem, argp, sizeof(cma_mem)))
			return -EFAULT;

		r = gunyah_vm_binding_cma_alloc(ghvm, &cma_mem);
		break;
	}
	case GUNYAH_VM_SET_BOOT_CONTEXT: {
		struct gunyah_vm_boot_context boot_ctx;

		if (copy_from_user(&boot_ctx, argp, sizeof(boot_ctx)))
			return -EFAULT;

		return gunyah_vm_set_boot_context(ghvm, &boot_ctx);
	}
	case GH_VM_ANDROID_SET_AUTH_TYPE: {
		struct gunyah_auth_desc auth_desc;

		if (copy_from_user(&auth_desc, argp, sizeof(auth_desc)))
			return -EFAULT;

		return gunyah_vm_set_auth_type(ghvm, &auth_desc);
	}
	default:
		r = -ENOTTY;
		break;
	}

	return r;
}

int __must_check gunyah_vm_get(struct gunyah_vm *ghvm)
{
	return kref_get_unless_zero(&ghvm->kref);
}
EXPORT_SYMBOL_GPL(gunyah_vm_get);

static void _gunyah_vm_put(struct kref *kref)
{
	struct gunyah_vm *ghvm = container_of(kref, struct gunyah_vm, kref);
	struct gunyah_vm_binding *b;
	unsigned long index = 0;
	void *entry;
	int ret;

	/**
	 * We might race with a VM exit notification, but that's ok:
	 * gh_rm_vm_stop() will just return right away.
	 */
	if (ghvm->vm_status == GUNYAH_RM_VM_STATUS_RUNNING)
		gunyah_vm_stop(ghvm);

	gunyah_vm_remove_functions(ghvm);

	/**
	 * If this fails, we're going to lose the memory for good and is
	 * BUG_ON-worthy, but not unrecoverable (we just lose memory).
	 * This call should always succeed though because the VM is in not
	 * running and RM will let us reclaim all the memory.
	 */
	WARN_ON(gunyah_vm_reclaim_range(ghvm, 0, U64_MAX));
	WARN_ON(!mtree_empty(&ghvm->mm));
	mtree_destroy(&ghvm->mm);

	/* clang-format off */
	gunyah_vm_remove_resource_ticket(ghvm, &ghvm->addrspace_ticket);
	gunyah_vm_remove_resource_ticket(ghvm, &ghvm->host_shared_extent_ticket);
	gunyah_vm_remove_resource_ticket(ghvm, &ghvm->host_private_extent_ticket);
	gunyah_vm_remove_resource_ticket(ghvm, &ghvm->guest_shared_extent_ticket);
	gunyah_vm_remove_resource_ticket(ghvm, &ghvm->guest_private_extent_ticket);
	/* clang-format on */

	ret = gunyah_vm_pre_vm_reset(ghvm);
	if (ret)
		dev_err(ghvm->parent, "Failed pre reset the vm: %d\n", ret);

	gunyah_vm_clean_resources(ghvm);

	if (ghvm->vm_status == GUNYAH_RM_VM_STATUS_EXITED ||
	    ghvm->vm_status == GUNYAH_RM_VM_STATUS_READY ||
	    ghvm->vm_status == GUNYAH_RM_VM_STATUS_INIT_FAILED) {
		ret = gunyah_rm_vm_reset(ghvm->rm, ghvm->vmid);
		/* clang-format off */
		if (!ret)
			wait_event(ghvm->vm_status_wait,
				   ghvm->vm_status == GUNYAH_RM_VM_STATUS_RESET);
		else
			dev_err(ghvm->parent, "Failed to reset the vm: %d\n", ret);

		ret = gunyah_vm_post_vm_reset(ghvm);
		if (ret)
			dev_err(ghvm->parent, "Failed post reset the vm: %d\n", ret);
		/* clang-format on */
	}

	WARN_ON(gunyah_reclaim_parcels(ghvm, 0, ULONG_MAX));
	down_write(&ghvm->bindings_lock);
	mt_for_each(&ghvm->bindings, b, index, ULONG_MAX) {
		mtree_erase(&ghvm->bindings, gunyah_gpa_to_gfn(b->guest_phys_addr));
		kfree(b);
	}
	up_write(&ghvm->bindings_lock);
	WARN_ON(!mtree_empty(&ghvm->bindings));
	mtree_destroy(&ghvm->bindings);
	gunyah_uevent_notify_change(GUNYAH_EVENT_DESTROY_VM, ghvm);

	if (ghvm->vm_status > GUNYAH_RM_VM_STATUS_NO_STATE) {
		gunyah_rm_notifier_unregister(ghvm->rm, &ghvm->nb);

		ret = gunyah_rm_dealloc_vmid(ghvm->rm, ghvm->vmid);
		if (ret)
			dev_warn(ghvm->parent,
				 "Failed to deallocate vmid: %d\n", ret);
	}

	xa_for_each(&ghvm->boot_context, index, entry)
		kfree(entry);

	gunyah_put_auth_vm_mgr(ghvm);
	xa_destroy(&ghvm->boot_context);
	gunyah_rm_put(ghvm->rm);
	mmdrop(ghvm->mm_s);
	kfree(ghvm);
}

void gunyah_vm_put(struct gunyah_vm *ghvm)
{
	kref_put(&ghvm->kref, _gunyah_vm_put);
}
EXPORT_SYMBOL_GPL(gunyah_vm_put);

static int gunyah_vm_release(struct inode *inode, struct file *filp)
{
	struct gunyah_vm *ghvm = filp->private_data;

	trace_android_rvh_gh_vm_release(ghvm->vmid, ghvm);
	gunyah_vm_put(ghvm);
	return 0;
}

static const struct file_operations gunyah_vm_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gunyah_vm_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.release = gunyah_vm_release,
	.llseek = noop_llseek,
};

static long gunyah_dev_ioctl_create_vm(struct gunyah_rm *rm, unsigned long arg)
{
	struct gunyah_vm *ghvm;
	struct file *file;
	int fd, err;

	/* arg reserved for future use. */
	if (arg)
		return -EINVAL;

	ghvm = gunyah_vm_alloc(rm);
	if (IS_ERR(ghvm))
		return PTR_ERR(ghvm);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		err = fd;
		goto err_destroy_vm;
	}

	file = anon_inode_getfile("gunyah-vm", &gunyah_vm_fops, ghvm, O_RDWR);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_put_fd;
	}

	fd_install(fd, file);

	return fd;

err_put_fd:
	put_unused_fd(fd);
err_destroy_vm:
	gunyah_rm_put(ghvm->rm);
	kfree(ghvm);
	return err;
}

long gunyah_dev_vm_mgr_ioctl(struct gunyah_rm *rm, unsigned int cmd,
			     unsigned long arg)
{
	switch (cmd) {
	case GUNYAH_CREATE_VM:
		return gunyah_dev_ioctl_create_vm(rm, arg);
	default:
		return -ENOTTY;
	}
}
