// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/dma-mapping.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of_device.h>
#include <linux/highmem.h>
#include <linux/list.h>

#include "apummu_cmn.h"
#include "apummu_mem.h"
#include "apummu_import.h"
#include "linux/soc/mediatek/apummu_mem_def.h"

static struct apummu_mem *g_mem_sys;
static uint32_t general_SLB_attempt_cnt;
static uint32_t DMA_CNT;

static struct list_head g_mem_list_head;

struct ammu_mem_dma {
	dma_addr_t dma_addr;
	uint32_t dma_size;
	uint32_t size;
	uint32_t cnt;
	void *vaddr;
	struct list_head attachments;
	struct sg_table sgt;
	void *buf;
	bool uncached;
	struct mutex mtx;
	struct device *mem_dev;
};

#if USING_SELF_DMA
struct ammu_mem_dma_attachment {
	struct sg_table *sgt;
	struct device *dev;
	struct list_head node;
	bool mapped;
	bool uncached;
};

static struct sg_table *ammu_dmabuf_map_dma(struct dma_buf_attachment *attach,
	enum dma_data_direction dir)
{
	struct ammu_mem_dma_attachment *a = attach->priv;
	struct sg_table *table = NULL;
	int attr = attach->dma_map_attrs;
	int ret = 0;

	table = a->sgt;
	if (a->uncached)
		attr |= DMA_ATTR_SKIP_CPU_SYNC;

	ret = dma_map_sgtable(attach->dev, table, dir, attr);
	if (ret)
		table = ERR_PTR(ret);

	a->mapped = true;

	return table;
}

static void ammu_dmabuf_unmap_dma(struct dma_buf_attachment *attach,
				    struct sg_table *sgt,
				    enum dma_data_direction dir)
{
	struct ammu_mem_dma_attachment *a = attach->priv;
	int attr = attach->dma_map_attrs;
	int attr = 0;

	if (a->uncached)
		attr |= DMA_ATTR_SKIP_CPU_SYNC;

	a->mapped = false;
	dma_unmap_sgtable(attach->dev, sgt, dir, attr);
}

static struct sg_table *ammu_mem_dma_dup_sg(struct sg_table *table)
{
	struct sg_table *new_table;
	int ret, i;
	struct scatterlist *sg, *new_sg;

	new_table = kzalloc(sizeof(*new_table), GFP_KERNEL);
	if (!new_table)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(new_table, table->orig_nents, GFP_KERNEL);
	if (ret) {
		kfree(new_table);
		return ERR_PTR(-ENOMEM);
	}

	new_sg = new_table->sgl;
	for_each_sgtable_sg(table, sg, i) {
		sg_set_page(new_sg, sg_page(sg), sg->length, sg->offset);
		new_sg = sg_next(new_sg);
	}

	return new_table;
}

static int ammu_dmabuf_attach(struct dma_buf *dbuf,
	struct dma_buf_attachment *attach)
{
	struct ammu_mem_dma_attachment *a = NULL;
	struct apummu_mem *m = dbuf->priv;
	struct ammu_mem_dma *mdbuf = m->priv;
	int ret = 0;
	struct sg_table *table;

	AMMU_LOG_VERBO("dbuf(0x%llx)\n", (uint64_t)dbuf);

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	table = ammu_mem_dma_dup_sg(&mdbuf->sgt);
	if (IS_ERR(table)) {
		kfree(a);
		return -ENOMEM;
	}

	a->sgt = table;
	a->dev = attach->dev;
	INIT_LIST_HEAD(&a->node);
	a->mapped = false;
	a->uncached = mdbuf->uncached;
	attach->priv = a;

	mutex_lock(&mdbuf->mtx);
	list_add(&a->node, &mdbuf->attachments);
	mutex_unlock(&mdbuf->mtx);

	return ret;
}

static void ammu_dmabuf_detach(struct dma_buf *dbuf,
	struct dma_buf_attachment *attach)
{
	struct ammu_mem_dma_attachment *a = attach->priv;
	struct apummu_mem *m = dbuf->priv;
	struct ammu_mem_dma *mdbuf = m->priv;

	AMMU_LOG_VERBO("dbuf(0x%llx)\n", (uint64_t)dbuf);

	mutex_lock(&mdbuf->mtx);
	list_del(&a->node);
	mutex_unlock(&mdbuf->mtx);

	sg_free_table(a->sgt);
	kfree(a->sgt);
	kfree(a);
}

static void ammu_dmabuf_release(struct dma_buf *dbuf)
{
	struct apummu_mem *m = dbuf->priv;
	struct ammu_mem_dma *mdbuf = m->priv;

	AMMU_LOG_VERBO("dbuf(0x%llx) release\n", (uint64_t)dbuf);

	sg_free_table(&mdbuf->sgt);
	vunmap(mdbuf->vaddr);
	vfree(mdbuf->buf);
	kfree(mdbuf);
	kfree(m);
}

static struct dma_buf_ops ammu_dmabuf_ops = {
	.attach = ammu_dmabuf_attach,
	.detach = ammu_dmabuf_detach,
	.map_dma_buf = ammu_dmabuf_map_dma,
	.unmap_dma_buf = ammu_dmabuf_unmap_dma,
	.release = ammu_dmabuf_release,
};

static int ammu_mem_dma_allocate_sgt(const char *buf,
		size_t len, struct sg_table *sgt, bool uncached, void **vaddr)
{
	struct page **pages = NULL;
	unsigned int nr_pages;
	unsigned int alloc_size;
	unsigned int index;
	const char *p;
	int ret;
	pgprot_t pgprot = PAGE_KERNEL;
	void *va;

	nr_pages = DIV_ROUND_UP((unsigned long)buf + len, PAGE_SIZE)
		- ((unsigned long)buf / PAGE_SIZE);
	alloc_size = nr_pages * sizeof(struct page *);
	pages = kvmalloc(alloc_size, GFP_KERNEL);

	AMMU_LOG_VERBO("ammu buf: 0x%lx, len: 0x%lx, nr_pages: %d\n",
		(unsigned long)buf, len, nr_pages);

	if (!pages) {
		AMMU_LOG_ERR("No Page 0x%lx, len: 0x%lx, nr_pages: %d\n",
				(unsigned long)buf, len, nr_pages);
		return -ENOMEM;
	}

	p = buf - offset_in_page(buf);
	AMMU_LOG_VERBO("start p: 0x%llx buf: 0x%llx\n",
			(uint64_t) p, (uint64_t) buf);

	for (index = 0; index < nr_pages; index++) {
		if (is_vmalloc_addr(p))
			pages[index] = vmalloc_to_page(p);
		else
			pages[index] = kmap_to_page((void *)p);
		if (!pages[index]) {
			kvfree(pages);
			AMMU_LOG_ERR("map failed\n");
			return -EFAULT;
		}
		p += PAGE_SIZE;
	}
	if (uncached)
		pgprot = pgprot_writecombine(PAGE_KERNEL);

	// vmap to set page property
	va = vmap(pages, nr_pages, VM_MAP, pgprot);

	ret = sg_alloc_table_from_pages(sgt, pages, index,
		offset_in_page(buf), len, GFP_KERNEL);
	kvfree(pages);
	if (ret) {
		AMMU_LOG_ERR("sg_alloc_table_from_pages: %d\n", ret);
		return ret;
	}

	*vaddr = va;

	AMMU_LOG_VERBO("buf: 0x%lx, len: 0x%lx, sgt: 0x%lx nr_pages: %d va 0x%lx\n",
		(unsigned long)buf, len, (unsigned long)sgt, nr_pages, (unsigned long)va);

	return 0;
}

static int apummu_mem_alloc_sgt(struct device *dev, struct apummu_mem *mem)
{
	struct ammu_mem_dma *mdbuf = NULL;
	int ret = 0;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	void *kva;
	bool uncached = true;

	/* alloc ammu dma-buf container */
	mdbuf = kzalloc(sizeof(*mdbuf), GFP_KERNEL);
	if (!mdbuf) {
		AMMU_LOG_ERR("alloc mdbuf fail\n");
		return -ENOMEM;
	}

	mutex_init(&mdbuf->mtx);
	INIT_LIST_HEAD(&mdbuf->attachments);

	/* alloc buffer by dma */
	mdbuf->dma_size = PAGE_ALIGN(mem->size);
	AMMU_LOG_VERBO("alloc (mem size, dma size) (0x%x/0x%x)\n", mem->size, mdbuf->dma_size);

	kva = vzalloc(mdbuf->dma_size);
	if (!kva) {
		AMMU_LOG_ERR("alloc DRAM fail, (mem size, dma size)=(0x%x, 0x%x)\n",
			mem->size, mdbuf->dma_size);
		ret = -ENOMEM;
		goto free_ammu_dbuf;
	}

	mdbuf->buf = kva;

	if (ammu_mem_dma_allocate_sgt(
			kva, mdbuf->dma_size, &mdbuf->sgt,
			uncached, &mdbuf->vaddr)) {
		AMMU_LOG_ERR("get sgt: failed\n");
		ret = -ENOMEM;
		goto free_buf;
	}

	/* export as dma-buf */
	exp_info.ops = &ammu_dmabuf_ops;
	exp_info.size = mdbuf->dma_size;
	exp_info.flags = O_RDWR | O_CLOEXEC;
	exp_info.priv = mem;

	mem->dbuf = dma_buf_export(&exp_info);
	if (IS_ERR(mem->dbuf)) {
		AMMU_LOG_ERR("dma_buf_export Fail\n");
		ret = -ENOMEM;
		goto free_sgt;
	}

	mdbuf->mem_dev = dev;
	mdbuf->size = mem->size;
	mdbuf->uncached = uncached;
	mdbuf->cnt = DMA_CNT++;
	/* access data to ammu_mem */
	mem->vaddr = mdbuf->vaddr;
	mem->priv = mdbuf;

	if (uncached) {
		dma_sync_sgtable_for_device(dev,
			&mdbuf->sgt, DMA_TO_DEVICE);
	}

	goto out;

free_sgt:
	sg_free_table(&mdbuf->sgt);
free_buf:
	vfree(kva);
free_ammu_dbuf:
	kfree(mdbuf);
out:
	return ret;
}

static int ammu_mem_map_create(struct device *dev, struct apummu_mem *m)
{
	struct ammu_mem_map *map = NULL;
	struct scatterlist *sg = NULL;
	int ret = 0, i = 0;

	// get_dma_buf(m->dbuf);

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map) {
		ret = -ENOMEM;
		goto out;
	}

	/* attach device */
	map->attach = dma_buf_attach(m->dbuf, dev);
	if (IS_ERR(map->attach)) {
		ret = PTR_ERR(map->attach);
		AMMU_LOG_ERR("dma_buf_attach failed: %d\n", ret);
		goto free_map;
	}

	/* map device va */
	map->sgt = dma_buf_map_attachment_unlocked(map->attach,
		DMA_BIDIRECTIONAL);
	if (IS_ERR(map->sgt)) {
		ret = PTR_ERR(map->sgt);
		AMMU_LOG_ERR("dma_buf_map_attachment_unlocked failed: %d\n", ret);
		goto detach_dbuf;
	}

	/* get start addr and size */
	m->iova = sg_dma_address(map->sgt->sgl);
	for_each_sgtable_dma_sg(map->sgt, sg, i) {
		if (!sg)
			break;
		m->dva_size += sg_dma_len(sg);
	}

	/* check iova and size */
	if (!m->iova || !m->dva_size) {
		AMMU_LOG_ERR("can't get mem(0x%llx) iova(0x%llx/%u)\n",
			(uint64_t)m, m->iova, m->dva_size);
		ret = -ENOMEM;
		goto unmap_dbuf;
	}

	map->m = m;
	m->map = map;
	goto out;

unmap_dbuf:
	dma_buf_unmap_attachment_unlocked(map->attach,
		map->sgt, DMA_BIDIRECTIONAL);
detach_dbuf:
	dma_buf_detach(m->dbuf, map->attach);
free_map:
	m->iova = 0;
	m->dva_size = 0;
	m->map = NULL;
	kfree(map);
out:
	return ret;
}

static int ammu_mem_unmap(struct apummu_mem *m)
{
	int ret = 0;
	struct ammu_mem_map *map = m->map;

	/* unmap device va */
	dma_buf_unmap_attachment_unlocked(map->attach,
		map->sgt, DMA_BIDIRECTIONAL);
	dma_buf_detach(m->dbuf, map->attach);
	m->iova = 0;
	m->size = 0;
	m->map = NULL;
	kfree(map);

	return ret;
}
#endif

void apummu_mem_free(struct device *dev, struct apummu_mem *mem)
{
#if USING_SELF_DMA
	int ret = 0;

	ret = ammu_mem_unmap(mem);
	if (ret)
		AMMU_LOG_ERR("ammu_mem_unmap fail...\n");

	dma_buf_put(mem->dbuf);
#else
	if (mem->iova != 0) { // To handle dma_heap_buffer_alloc fail by signal
		dma_buf_unmap_attachment(mem->attach, mem->sgt, DMA_BIDIRECTIONAL);
		dma_buf_detach(mem->priv, mem->attach);
		dma_buf_put(mem->priv);
		dma_heap_put(mem->heap);
	}
	// dma_free_coherent(dev, mem->size, (void *)mem->kva, mem->iova);
#endif
}

int apummu_mem_alloc(struct device *dev, struct apummu_mem *mem)
{
	int ret = 0;
	struct platform_device *apusys_rv_pdev;
	struct device_node *apusys_rv_np;

	apusys_rv_np = of_parse_phandle(dev->of_node, "apu-rv-dev", 0);
	if (!apusys_rv_np) {
		pr_err("failed to find apu-rv-dev\n");
		return -ENXIO;
	}

	apusys_rv_pdev = of_find_device_by_node(apusys_rv_np);
	if (!apusys_rv_pdev) {
		pr_err("failed to find apu-rv-dev\n");
		return -1;
	}

	dev = &apusys_rv_pdev->dev;

#if USING_SELF_DMA
	ret = apummu_mem_alloc_sgt(dev, mem);
	if (ret) {
		AMMU_LOG_ERR("apummu_mem_alloc_sgt fail (0x%x)\n", mem->size);
		goto out;
	}

	ret = ammu_mem_map_create(dev, mem);
	if (ret) {
		AMMU_LOG_ERR("ammu_mem_map_create fail (0x%x)\n", mem->size);
		goto out;
	}
#else
	mem->heap = dma_heap_find("system");
	if (!mem->heap) {
		AMMU_LOG_ERR("Cannot get system heap\n");
		ret = -ENOMEM;
		goto out;
	}

	mem->priv = dma_heap_buffer_alloc(mem->heap, mem->size, O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR_OR_NULL(mem->priv)) {
		if (!mem->priv || mem->priv != ERR_PTR(-EINTR)) {
			AMMU_LOG_ERR("dma_heap_buffer_alloc fail mem size = 0x%x\n", mem->size);
			ret = -ENOMEM;
		} else {
			AMMU_LOG_WRN("APUMMU mem alloc fail trigger by signal...\n");
		}
		goto heap_alloc_err;
	}

	mem->attach = dma_buf_attach(mem->priv, dev);
	if (IS_ERR_OR_NULL(mem->attach)) {
		AMMU_LOG_ERR("dma_buf_attach fail\n");
		ret = -ENOMEM;
		goto dma_buf_attach_err;
	}

	mem->sgt = dma_buf_map_attachment(mem->attach, DMA_BIDIRECTIONAL);
	if (IS_ERR_OR_NULL(mem->sgt)) {
		AMMU_LOG_ERR("dma_buf_map_attachment fail\n");
		ret = -ENOMEM;
		goto dbuf_map_attachment_err;
	}

	mem->iova = sg_dma_address(mem->sgt->sgl);
#endif

	AMMU_LOG_INFO("DRAM alloc mem(0x%llx/0x%x)\n",
		mem->iova, mem->size);

out:
	return ret;

#if !(USING_SELF_DMA)
dbuf_map_attachment_err:
	dma_buf_detach(mem->priv, mem->attach);
dma_buf_attach_err:
	dma_buf_put(mem->priv);
heap_alloc_err:
	dma_heap_put(mem->heap);
#endif
	return ret;
}

#if !(DRAM_FALL_BACK_IN_RUNTIME)
int apummu_dram_remap_alloc(void *drvinfo)
{
	struct apummu_dev_info *adv = NULL;
	unsigned int i = 0;
	int ret = 0;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		ret = -EINVAL;
		goto out;
	}
	adv = (struct apummu_dev_info *)drvinfo;

	g_mem_sys.size = (uint64_t) adv->remote.vlm_size * adv->remote.dram_max;
	ret = apummu_mem_alloc(adv->dev, &g_mem_sys);
	if (ret) {
		AMMU_LOG_ERR("DRAM FB mem alloc fail\n");
		goto out;
	}

	adv->rsc.vlm_dram.base = (void *) g_mem_sys.kva;
	adv->rsc.vlm_dram.size = g_mem_sys.size;
	for (i = 0; i < adv->remote.dram_max; i++)
		adv->remote.dram[i] = g_mem_sys.iova + adv->remote.vlm_size * (uint64_t) i;

out:
	return ret;
}

int apummu_dram_remap_free(void *drvinfo)
{
	struct apummu_dev_info *adv = NULL;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		return -EINVAL;
	}
	adv = (struct apummu_dev_info *)drvinfo;

	apummu_mem_free(adv->dev, &g_mem_sys);
	adv->rsc.vlm_dram.base = NULL;
	return 0;
}
#else
int apummu_dram_remap_runtime_alloc(void *drvinfo)
{
	struct apummu_dev_info *adv = NULL;
	int ret = 0;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		ret = -EINVAL;
		goto out;
	}
	adv = (struct apummu_dev_info *)drvinfo;

	if (adv->rsc.vlm_dram.iova != 0) {
		AMMU_LOG_ERR("Error DRAM FB already alloc\n");
		ret = -EINVAL;
		goto out;
	}

	g_mem_sys = kzalloc(sizeof(*g_mem_sys), GFP_KERNEL);
	if (!g_mem_sys) {
		AMMU_LOG_ERR("g_mem_sys alloc fail\n");
		ret = -ENOMEM;
		goto out;
	}

	g_mem_sys->size = (uint64_t) adv->remote.vlm_size * adv->remote.dram_max;

	ret = apummu_mem_alloc(adv->dev, g_mem_sys);
	if (ret) {
		kfree(g_mem_sys);
		goto out;
	}

	adv->rsc.vlm_dram.base = (void *) g_mem_sys->kva;
	adv->rsc.vlm_dram.size = g_mem_sys->size;
	adv->rsc.vlm_dram.iova = g_mem_sys->iova;

out:
	return ret;
}

int apummu_dram_remap_runtime_free(void *drvinfo)
{
	int ret = 0;

	struct apummu_dev_info *adv = NULL;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		ret = -EINVAL;
		goto out;
	}
	adv = (struct apummu_dev_info *)drvinfo;

	apummu_mem_free(adv->dev, g_mem_sys);
	adv->rsc.vlm_dram.base = 0;
	adv->rsc.vlm_dram.size = 0;
	adv->rsc.vlm_dram.iova = 0;
#if USING_SELF_DMA
	g_mem_sys = NULL;
#else
	kfree(g_mem_sys);
#endif

out:
	return ret;
}

int apummu_dram_remap_runtime_alloc_with_size(void *drvinfo, uint32_t ctx_num_going_alloc, uint64_t *ret_IOVA)
{
	struct apummu_mem *mem;
	struct apummu_dev_info *adv = NULL;
	int ret = 0;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		ret = -EINVAL;
		goto out;
	}
	adv = (struct apummu_dev_info *)drvinfo;

	if (ctx_num_going_alloc == 0) {
		AMMU_LOG_ERR("Cannot alloc with ctx_num_going_alloc Zero\n");
		ret = -EINVAL;
		goto out;
	}

	mem = kzalloc(sizeof(struct apummu_mem), GFP_KERNEL);
	if (!mem) {
		AMMU_LOG_ERR("apummu_mem alloc fail\n");
		ret = -ENOMEM;
		goto out;
	}

	mem->size = (uint64_t) adv->remote.vlm_size * ctx_num_going_alloc;

	ret = apummu_mem_alloc(adv->dev, mem);
	if (ret) {
		kfree(mem);
		goto out;
	}

	*ret_IOVA = mem->iova;
	AMMU_LOG_VERBO("sbu_cmd_num = %u, mem->size = 0x%x, ret_IOVA = 0x%llx\n",
		ctx_num_going_alloc, mem->size, *ret_IOVA);
	list_add_tail(&mem->list, &g_mem_list_head);

out:
	return ret;
}

int apummu_dram_remap_runtime_free_single_node(void *drvinfo, uint64_t target_iova)
{
	int ret = -EINVAL;
	struct list_head *list_ptr;
	struct apummu_mem *target_mem_node;
	struct apummu_dev_info *adv = NULL;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		ret = -EINVAL;
		goto out;
	}
	adv = (struct apummu_dev_info *)drvinfo;

	list_for_each(list_ptr, &g_mem_list_head) {
		target_mem_node = list_entry(list_ptr, struct apummu_mem, list);
		if (target_mem_node->iova == target_iova) {
			ret = 0;
			list_del(&target_mem_node->list);
			apummu_mem_free(adv->dev, target_mem_node);
			kfree(target_mem_node);
			break;
		}
	}

out:
	return ret;
}

int apummu_dram_remap_runtime_free_whole_list(void *drvinfo)
{
	int ret = 0;
	struct list_head *list_ptr1, *list_ptr2;
	struct apummu_mem *mem_ptr;
	struct apummu_dev_info *adv = NULL;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		ret = -EINVAL;
		goto out;
	}
	adv = (struct apummu_dev_info *)drvinfo;

	list_for_each_safe(list_ptr1, list_ptr2, &g_mem_list_head) {
		mem_ptr = list_entry(list_ptr1, struct apummu_mem, list);
		list_del(&mem_ptr->list);
		apummu_mem_free(adv->dev, mem_ptr);
		kfree(mem_ptr);
		mem_ptr = NULL;
	}

out:
	return ret;
}
#endif

int apummu_alloc_general_SLB(void *drvinfo)
{
	struct apummu_dev_info *adv = NULL;
	uint64_t ret_addr, ret_size;
	uint32_t size = 0;
	int ret = 0;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		ret = -EINVAL;
		goto out;
	}
	adv = (struct apummu_dev_info *)drvinfo;

	if (adv->rsc.genernal_SLB.iova != 0) {
		/* Call check SLB status API, since SLB might timeout */
		if (apummu_check_slb_status(APUMMU_MEM_TYPE_GENERAL_S) != 0) {
			AMMU_LOG_INFO("general SLB already added\n");
			goto out;
		} else {
			adv->rsc.genernal_SLB.iova = 0;
			adv->rsc.genernal_SLB.size = 0;
			AMMU_LOG_INFO("Overwrite SLB status manually, (addr, size) = (%llx, %x)\n",
				adv->rsc.genernal_SLB.iova, adv->rsc.genernal_SLB.size);
		}
	}

	ret = apummu_alloc_slb(APUMMU_MEM_TYPE_GENERAL_S, size, adv->plat.slb_wait_time,
					&ret_addr, &ret_size);
	if (ret) {
		AMMU_LOG_WRN("general SLB alloc fail %u times...\n",
			general_SLB_attempt_cnt);
		general_SLB_attempt_cnt += 1;
		goto out;
	}

	adv->rsc.genernal_SLB.iova = ret_addr;
	adv->rsc.genernal_SLB.size = (uint32_t) ret_size;

	AMMU_LOG_VERBO("General SLB alloced after %u times. (addr, size) = (0x%llx, 0x%llx)\n",
		general_SLB_attempt_cnt, ret_addr, ret_size);
	general_SLB_attempt_cnt = 0;

out:
	return ret;
}

int apummu_free_general_SLB(void *drvinfo)
{
	struct apummu_dev_info *adv = NULL;
	int ret = 0;

	if (drvinfo == NULL) {
		AMMU_LOG_ERR("invalid argument\n");
		ret = -EINVAL;
		goto out;
	}
	adv = (struct apummu_dev_info *)drvinfo;

	if (adv->rsc.genernal_SLB.iova == 0) {
		AMMU_LOG_ERR("No general SLB is alloced\n");
		ret = -EINVAL;
		goto out;
	}

	ret = apummu_free_slb(APUMMU_MEM_TYPE_GENERAL_S);
	if (ret) {
		AMMU_LOG_WRN("general SLB free fail...\n");
		goto out;
	}

	adv->rsc.genernal_SLB.iova = 0;
	adv->rsc.genernal_SLB.size = 0;

	AMMU_LOG_VERBO("General SLB freeed\n");

out:
	return ret;
}

void apummu_mem_init(void)
{
	INIT_LIST_HEAD(&g_mem_list_head);
	general_SLB_attempt_cnt = 0;
	DMA_CNT = 0;
}
