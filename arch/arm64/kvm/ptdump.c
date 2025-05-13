// SPDX-License-Identifier: GPL-2.0-only
/*
 * Debug helper used to dump the stage-2 pagetables of the system and their
 * associated permissions.
 *
 * Copyright (C) Google, 2024
 * Author: Sebastian Ene <sebastianene@google.com>
 */
#include <linux/debugfs.h>
#include <linux/kvm_host.h>
#include <linux/seq_file.h>

#include <asm/kvm_pkvm.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_pgtable.h>
#include <asm/ptdump.h>

#define MARKERS_LEN		2
#define KVM_PGTABLE_MAX_LEVELS	(KVM_PGTABLE_LAST_LEVEL + 1)
#define MAX_LOG_PAGES	10

struct kvm_ptdump_guest_state {
	struct kvm		*kvm;
	struct ptdump_pg_state	parser_state;
	struct addr_marker	ipa_marker[MARKERS_LEN];
	struct ptdump_pg_level	level[KVM_PGTABLE_MAX_LEVELS];
	struct ptdump_range	range[MARKERS_LEN];
	struct pkvm_ptdump_log_hdr	*log_pages;
};

static const struct ptdump_prot_bits stage2_pte_bits[] = {
	{
		.mask	= PTE_VALID,
		.val	= PTE_VALID,
		.set	= " ",
		.clear	= "F",
	}, {
		.mask	= KVM_PTE_LEAF_ATTR_LO_S2_S2AP_R | PTE_VALID,
		.val	= KVM_PTE_LEAF_ATTR_LO_S2_S2AP_R | PTE_VALID,
		.set	= "R",
		.clear	= " ",
	}, {
		.mask	= KVM_PTE_LEAF_ATTR_LO_S2_S2AP_W | PTE_VALID,
		.val	= KVM_PTE_LEAF_ATTR_LO_S2_S2AP_W | PTE_VALID,
		.set	= "W",
		.clear	= " ",
	}, {
		.mask	= KVM_PTE_LEAF_ATTR_HI_S2_XN | PTE_VALID,
		.val	= PTE_VALID,
		.set	= "X",
		.clear	= " ",
	}, {
		.mask	= KVM_PTE_LEAF_ATTR_LO_S2_AF | PTE_VALID,
		.val	= KVM_PTE_LEAF_ATTR_LO_S2_AF | PTE_VALID,
		.set	= "AF",
		.clear	= "  ",
	}, {
		.mask	= PTE_TABLE_BIT | PTE_VALID,
		.val	= PTE_VALID,
		.set	= "BLK",
		.clear	= "   ",
	}, {
		.mask	= KVM_PGTABLE_PROT_SW0 | PTE_VALID,
		.val	= KVM_PGTABLE_PROT_SW0 | PTE_VALID,
		.set	= "SW0",
		.clear	= "   ",
	}, {
		.mask	= KVM_PGTABLE_PROT_SW1 | PTE_VALID,
		.val	= KVM_PGTABLE_PROT_SW1 | PTE_VALID,
		.set	= "SW1",
		.clear	= "   ",
	},
};

static int kvm_ptdump_visitor(const struct kvm_pgtable_visit_ctx *ctx,
			      enum kvm_pgtable_walk_flags visit)
{
	struct ptdump_pg_state *st = ctx->arg;
	struct ptdump_state *pt_st = &st->ptdump;

	note_page(pt_st, ctx->addr, ctx->level, ctx->old);

	return 0;
}

static int kvm_ptdump_build_levels(struct ptdump_pg_level *level, u32 start_lvl)
{
	u32 i;
	u64 mask;

	if (WARN_ON_ONCE(start_lvl >= KVM_PGTABLE_LAST_LEVEL))
		return -EINVAL;

	mask = 0;
	for (i = 0; i < ARRAY_SIZE(stage2_pte_bits); i++)
		mask |= stage2_pte_bits[i].mask;

	for (i = start_lvl; i < KVM_PGTABLE_MAX_LEVELS; i++) {
		snprintf(level[i].name, sizeof(level[i].name), "%u", i);

		level[i].num	= ARRAY_SIZE(stage2_pte_bits);
		level[i].bits	= stage2_pte_bits;
		level[i].mask	= mask;
	}

	return 0;
}

#define PKVM_HANDLE(kvm) ((kvm) != NULL ? (kvm)->arch.pkvm.handle : 0)

static u32 ptdump_get_ranges(struct kvm *kvm)
{
	if (!is_protected_kvm_enabled())
		return kvm->arch.mmu.pgt->ia_bits;
	return kvm_call_hyp_nvhe(__pkvm_ptdump, PKVM_HANDLE(kvm), PKVM_PTDUMP_GET_RANGE);
}

static s8 ptdump_get_level(struct kvm *kvm)
{
	if (!is_protected_kvm_enabled())
		return kvm->arch.mmu.pgt->start_level;
	return kvm_call_hyp_nvhe(__pkvm_ptdump, PKVM_HANDLE(kvm), PKVM_PTDUMP_GET_LEVEL);
}

static struct kvm_ptdump_guest_state *kvm_ptdump_parser_create(struct kvm *kvm)
{
	struct kvm_ptdump_guest_state *st;
	int ret;
	u32 ia_bits = ptdump_get_ranges(kvm);
	s8 start_level = ptdump_get_level(kvm);

	st = kzalloc(sizeof(struct kvm_ptdump_guest_state), GFP_KERNEL_ACCOUNT);
	if (!st)
		return ERR_PTR(-ENOMEM);

	ret = kvm_ptdump_build_levels(&st->level[0], start_level);
	if (ret) {
		kfree(st);
		return ERR_PTR(ret);
	}

	st->ipa_marker[0].name		= kvm == NULL ? "Host IPA" : "Guest IPA";
	st->ipa_marker[1].start_address = BIT(ia_bits);
	st->range[0].end		= BIT(ia_bits);

	st->kvm				= kvm;
	st->parser_state = (struct ptdump_pg_state) {
		.marker		= &st->ipa_marker[0],
		.level		= -1,
		.pg_level	= &st->level[0],
		.ptdump.range	= &st->range[0],
		.start_address	= 0,
	};

	return st;
}

static int kvm_ptdump_guest_show(struct seq_file *m, void *unused)
{
	int ret;
	struct kvm_ptdump_guest_state *st = m->private;
	struct kvm *kvm = st->kvm;
	struct kvm_s2_mmu *mmu = &kvm->arch.mmu;
	struct ptdump_pg_state *parser_state = &st->parser_state;
	struct kvm_pgtable_walker walker = (struct kvm_pgtable_walker) {
		.cb	= kvm_ptdump_visitor,
		.arg	= parser_state,
		.flags	= KVM_PGTABLE_WALK_LEAF,
	};

	parser_state->seq = m;

	write_lock(&kvm->mmu_lock);
	ret = kvm_pgtable_walk(mmu->pgt, 0, BIT(mmu->pgt->ia_bits), &walker);
	write_unlock(&kvm->mmu_lock);

	return ret;
}

static int kvm_ptdump_guest_open(struct inode *m, struct file *file)
{
	struct kvm *kvm = m->i_private;
	struct kvm_ptdump_guest_state *st;
	int ret;

	if (!kvm_get_kvm_safe(kvm))
		return -ENOENT;

	st = kvm_ptdump_parser_create(kvm);
	if (IS_ERR(st)) {
		ret = PTR_ERR(st);
		goto err_with_kvm_ref;
	}

	ret = single_open(file, kvm_ptdump_guest_show, st);
	if (!ret)
		return 0;

	kfree(st);
err_with_kvm_ref:
	kvm_put_kvm(kvm);
	return ret;
}

static int kvm_ptdump_guest_close(struct inode *m, struct file *file)
{
	struct kvm *kvm = m->i_private;
	void *st = ((struct seq_file *)file->private_data)->private;

	kfree(st);
	kvm_put_kvm(kvm);

	return single_release(m, file);
}

static const struct file_operations kvm_ptdump_guest_fops = {
	.open		= kvm_ptdump_guest_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= kvm_ptdump_guest_close,
};

static int kvm_pgtable_range_show(struct seq_file *m, void *unused)
{
	struct kvm *kvm = m->private;
	u32 ia_bits = ptdump_get_ranges(kvm);

	seq_printf(m, "%2u\n", ia_bits);
	return 0;
}

static int kvm_pgtable_levels_show(struct seq_file *m, void *unused)
{
	struct kvm *kvm = m->private;
	s8 start_level = ptdump_get_level(kvm);

	seq_printf(m, "%1d\n", KVM_PGTABLE_MAX_LEVELS - start_level);
	return 0;
}

static int kvm_pgtable_debugfs_open(struct inode *m, struct file *file,
				    int (*show)(struct seq_file *, void *))
{
	struct kvm *kvm = m->i_private;
	int ret;

	if (!kvm_get_kvm_safe(kvm))
		return -ENOENT;

	ret = single_open(file, show, kvm);
	if (ret < 0)
		kvm_put_kvm(kvm);
	return ret;
}

static int kvm_pgtable_range_open(struct inode *m, struct file *file)
{
	return kvm_pgtable_debugfs_open(m, file, kvm_pgtable_range_show);
}

static int kvm_pgtable_levels_open(struct inode *m, struct file *file)
{
	return kvm_pgtable_debugfs_open(m, file, kvm_pgtable_levels_show);
}

static int kvm_pgtable_debugfs_close(struct inode *m, struct file *file)
{
	struct kvm *kvm = m->i_private;

	kvm_put_kvm(kvm);
	return single_release(m, file);
}

static const struct file_operations kvm_pgtable_range_fops = {
	.open		= kvm_pgtable_range_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= kvm_pgtable_debugfs_close,
};

static const struct file_operations kvm_pgtable_levels_fops = {
	.open		= kvm_pgtable_levels_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= kvm_pgtable_debugfs_close,
};

static int pkvm_ptdump_alloc_page(struct pkvm_ptdump_log_hdr **log_pages, size_t *num_pages)
{
	struct pkvm_ptdump_log_hdr *p, *it;
	size_t len = 1;

	p = (void *)get_zeroed_page(GFP_KERNEL_ACCOUNT);
	if (!p)
		return -ENOMEM;

	p->pfn_next = INVALID_PTDUMP_PFN;

	if (*log_pages == NULL)
		*log_pages = p;
	else {
		it = *log_pages;
		while (it->pfn_next != INVALID_PTDUMP_PFN) {
			it = pfn_to_kaddr(it->pfn_next);
			len++;
		}
		it->pfn_next = virt_to_pfn(p);
	}

	if (num_pages)
		*num_pages = len;

	return 0;
}

static void pkvm_ptdump_free_pages(struct pkvm_ptdump_log_hdr *log_pages)
{
	struct pkvm_ptdump_log_hdr *tmp;
	u64 log_pfn;

	if (log_pages == NULL)
		return;

	do {
		log_pfn = log_pages->pfn_next;
		tmp = pfn_to_kaddr(log_pfn);
		free_page((unsigned long)log_pages);
		log_pages = tmp;
	} while (log_pfn != INVALID_PTDUMP_PFN);
}

static u64 pkvm_ptdump_unpack_pte(struct pkvm_ptdump_log *log)
{
	return FIELD_PREP(KVM_PTE_VALID, log->valid) |
		FIELD_PREP(KVM_PTE_LEAF_ATTR_LO_S2_S2AP_R, log->r) |
		FIELD_PREP(KVM_PTE_LEAF_ATTR_LO_S2_S2AP_W, log->w) |
		FIELD_PREP(KVM_PTE_LEAF_ATTR_HI_S2_XN, log->xn) |
		FIELD_PREP(KVM_PTE_TYPE, log->table) |
		FIELD_PREP(KVM_PGTABLE_PROT_SW0 | KVM_PGTABLE_PROT_SW1, log->page_state);
}

static int pkvm_ptdump_show(struct seq_file *m, void *unused)
{
	struct kvm_ptdump_guest_state *st = m->private;
	struct kvm *kvm = st->kvm;
	struct ptdump_pg_state *parser_state = &st->parser_state;
	struct ptdump_state *pt_st = &parser_state->ptdump;
	int ret, i;
	struct pkvm_ptdump_log *log = NULL;
	struct pkvm_ptdump_log_hdr *it;
	size_t num_pages;

	parser_state->seq = m;
	parser_state->level = -1;
	parser_state->start_address = 0;

retry_dump:
	ret = kvm_call_hyp_nvhe(__pkvm_ptdump, PKVM_HANDLE(kvm),
				PKVM_PTDUMP_WALK_RANGE, st->log_pages);
	if (ret == -ENOMEM) {
		ret = pkvm_ptdump_alloc_page(&st->log_pages, &num_pages);
		if (ret)
			return ret;

		for (i = 0; i < num_pages; i++) {
			ret = pkvm_ptdump_alloc_page(&st->log_pages, NULL);
			if (ret)
				return ret;
		}
		goto retry_dump;
	} else if (ret != 0)
		return ret;

	it = st->log_pages;
	for (;;) {
		for (i = 0; i < it->w_index; i += sizeof(struct pkvm_ptdump_log)) {
			log = (void *)it + sizeof(struct pkvm_ptdump_log_hdr) + i;
			note_page(pt_st, ((unsigned long)log->pfn) << PAGE_SHIFT, log->level,
				  pkvm_ptdump_unpack_pte(log));
		}

		if (it->pfn_next == INVALID_PTDUMP_PFN)
			break;

		it = pfn_to_kaddr(it->pfn_next);
	}

	return 0;
}

static int pkvm_ptdump_guest_open(struct inode *m, struct file *file)
{
	struct kvm *kvm = m->i_private;
	struct kvm_ptdump_guest_state *st;
	int ret;

	if (!kvm_get_kvm_safe(kvm))
		return -ENOENT;

	st = kvm_ptdump_parser_create(kvm);
	if (IS_ERR(st)) {
		ret = PTR_ERR(st);
		goto err_with_kvm_ref;
	}

	pkvm_ptdump_alloc_page(&st->log_pages, NULL);

	ret = single_open(file, pkvm_ptdump_show, st);
	if (!ret)
		return 0;

	pkvm_ptdump_free_pages(st->log_pages);
	kfree(st);
err_with_kvm_ref:
	kvm_put_kvm(kvm);
	return ret;
}

static int pkvm_ptdump_guest_close(struct inode *m, struct file *file)
{
	struct kvm *kvm = m->i_private;
	struct kvm_ptdump_guest_state *st = ((struct seq_file *)file->private_data)->private;

	pkvm_ptdump_free_pages(st->log_pages);
	kfree(st);
	kvm_put_kvm(kvm);

	return single_release(m, file);
}

static const struct file_operations pkvm_ptdump_guest_fops = {
	.open		= pkvm_ptdump_guest_open,
	.read		= seq_read,
	.release	= pkvm_ptdump_guest_close,
};

void kvm_s2_ptdump_create_debugfs(struct kvm *kvm)
{
	debugfs_create_file("stage2_page_tables", 0400, kvm->debugfs_dentry,
			    kvm, is_protected_kvm_enabled() ?
			    &pkvm_ptdump_guest_fops : &kvm_ptdump_guest_fops);
	debugfs_create_file("ipa_range", 0400, kvm->debugfs_dentry, kvm,
			    &kvm_pgtable_range_fops);
	debugfs_create_file("stage2_levels", 0400, kvm->debugfs_dentry,
			    kvm, &kvm_pgtable_levels_fops);
}

static int kvm_host_pgtable_range_open(struct inode *m, struct file *file)
{
	return single_open(file, kvm_pgtable_range_show, NULL);
}

static int kvm_host_pgtable_levels_open(struct inode *m, struct file *file)
{
	return single_open(file, kvm_pgtable_levels_show, NULL);
}

static const struct file_operations kvm_host_pgtable_range_fops = {
	.open		= kvm_host_pgtable_range_open,
	.read		= seq_read,
	.release	= single_release,
};

static const struct file_operations kvm_host_pgtable_levels_fops = {
	.open		= kvm_host_pgtable_levels_open,
	.read		= seq_read,
	.release	= single_release,
};

static int kvm_ptdump_host_open(struct inode *m, struct file *file)
{
	struct kvm_ptdump_guest_state *st;
	int ret;

	st = kvm_ptdump_parser_create(NULL);
	if (IS_ERR(st))
		return PTR_ERR(st);

	for (int i = 0; i < MAX_LOG_PAGES; i++)
		pkvm_ptdump_alloc_page(&st->log_pages, NULL);

	ret = single_open(file, pkvm_ptdump_show, st);
	if (!ret)
		return 0;

	pkvm_ptdump_free_pages(st->log_pages);
	kfree(st);
	return ret;
}

static int kvm_ptdump_host_close(struct inode *m, struct file *file)
{
	struct kvm_ptdump_guest_state *st = ((struct seq_file *)file->private_data)->private;

	pkvm_ptdump_free_pages(st->log_pages);
	kfree(st);

	return single_release(m, file);
}

static const struct file_operations kvm_ptdump_host_fops = {
	.open		= kvm_ptdump_host_open,
	.read		= seq_read,
	.release	= kvm_ptdump_host_close,
};

void kvm_s2_ptdump_host_create_debugfs(void)
{
	struct dentry *kvm_debugfs_dir = debugfs_lookup("kvm", NULL);

	debugfs_create_file("host_stage2_page_tables", 0400, kvm_debugfs_dir,
			    NULL, &kvm_ptdump_host_fops);
	debugfs_create_file("ipa_range", 0400, kvm_debugfs_dir, NULL,
			    &kvm_host_pgtable_range_fops);
	debugfs_create_file("stage2_levels", 0400, kvm_debugfs_dir,
			    NULL, &kvm_host_pgtable_levels_fops);
}
