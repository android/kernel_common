/*
 * Copyright (C) 2010 The Chromium OS Authors <chromium-os-dev@chromium.org>
 *                    All Rights Reserved.
 *
 * This file is released under the GPL.
 */
/*
 * Implements a Chrome OS platform specific error handler.
 * When verity detects an invalid block, this error handling will
 * attempt to corrupt the kernel boot image. On reboot, the bios will
 * detect the kernel corruption and switch to the alternate kernel
 * and root file system partitions.
 *
 * Assumptions:
 * 1. Partitions are specified on the command line using uuid.
 * 2. The kernel partition is the partition number is one less
 *    than the root partition number.
 */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/crc32.h>
#include <linux/device.h>
#include <linux/device-mapper.h>
#include <linux/efi.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/notifier.h>
#include <linux/string.h>
#include <asm/page.h>

#include "dm-verity.h"

#define DM_MSG_PREFIX "verity-chromeos"
#define DMVERROR "DMVERROR"

#define GPT_TABLE_PAGE_NUM_ORDER 2
#define GPT_TABLE_SIZE ((1 << GPT_TABLE_PAGE_NUM_ORDER) * 4096)

struct gpt_header {
	__le64 signature;
	__le32 revision;
	__le32 header_size;
	__le32 header_crc32;
	__le32 reserved1;
	__le64 my_lba;
	__le64 alternate_lba;
	__le64 first_usable_lba;
	__le64 last_usable_lba;
	efi_guid_t disk_guid;
	__le64 partition_entry_lba;
	__le32 num_partition_entries;
	__le32 sizeof_partition_entry;
	__le32 partition_entry_array_crc32;

	/* The rest of the logical block is reserved by UEFI and must be zero.
	 * EFI standard handles this by:
	 *
	 * uint8_t		reserved2[ BlockSize - 92 ];
	 */
} __packed;

struct chromeos_kernel_gpt_attributes {
	u64 efi_spec:48;
	u64 priority:4;
	u64 tries:4;
	u64 success:1;
	u64 verity_error_counter:1;
	u64 unused:6;
} __packed;

struct gpt_entry {
	efi_guid_t partition_type_guid;
	efi_guid_t unique_partition_guid;
	__le64 starting_lba;
	__le64 ending_lba;
	struct chromeos_kernel_gpt_attributes attributes;
	__le16 partition_name[72/sizeof(__le16)];
} __packed;

static void chromeos_invalidate_kernel_endio(struct bio *bio)
{
	if (bio->bi_status) {
		DMERR("%s: bio operation failed (status=0x%x)", __func__,
		      bio->bi_status);
	}
	complete(bio->bi_private);
}

static int chromeos_invalidate_kernel_submit(struct bio *bio,
					     sector_t sector,
					     unsigned int len_byte,
					     struct page *page)
{
	DECLARE_COMPLETION_ONSTACK(wait);

	bio->bi_private = &wait;
	bio->bi_end_io = chromeos_invalidate_kernel_endio;

	bio->bi_iter.bi_sector = sector;
	bio->bi_vcnt = 1;
	bio->bi_iter.bi_idx = 0;
	bio->bi_iter.bi_size = len_byte;
	bio->bi_iter.bi_bvec_done = 0;
	bio->bi_io_vec[0].bv_page = page;
	bio->bi_io_vec[0].bv_len = len_byte;
	bio->bi_io_vec[0].bv_offset = 0;

	submit_bio(bio);
	/* Wait up to 2 seconds for completion or fail. */
	if (!wait_for_completion_timeout(&wait, msecs_to_jiffies(2000)))
		return -1;
	return 0;
}

static dev_t get_boot_dev_from_root_dev(struct block_device *root_bdev)
{
	/* Very basic sanity checking. This should be better. */
	if (!root_bdev || MAJOR(root_bdev->bd_dev) == 254 ||
	    root_bdev->bd_partno <= 1) {
		return 0;
	}
	return MKDEV(MAJOR(root_bdev->bd_dev), MINOR(root_bdev->bd_dev) - 1);
}

static char kern_guid[48];

/*
 * get_boot_dev is based on dm_get_device_by_uuid in dm_bootcache.
 *
 * This function is marked __ref because it calls the __init marked
 * early_lookup_bdev when called from the early boot code.
 */
static dev_t __ref get_boot_dev(void)
{
	const char partuuid[] = "PARTUUID=";
	char uuid[sizeof(partuuid) + 36];
	char *uuid_str;
	dev_t devt;

	if (!strlen(kern_guid)) {
		DMERR("Couldn't get uuid, try root dev");
		return 0;
	}

	if (strncmp(kern_guid, partuuid, strlen(partuuid))) {
		/* Not prefixed with "PARTUUID=", so add it */
		strcpy(uuid, partuuid);
		strlcat(uuid, kern_guid, sizeof(uuid));
		uuid_str = uuid;
	} else {
		uuid_str = kern_guid;
	}
        if (early_lookup_bdev(uuid_str, &devt)) {
                DMDEBUG("No matching partition for GUID: %s", uuid_str);
                return 0;
        }
	return devt;
}

/*
 * Invalidate the kernel which corresponds to the root block device.
 *
 * This function stamps DMVERROR on the beginning of the kernel partition.
 *
 * The kernel_guid commandline parameter is used to find the kernel partition
 *  number.
 * If that fails, the kernel partition is found by subtracting 1 from
 *  the root partition.
 * The DMVERROR string is stamped over only the CHROMEOS string at the
 *  beginning of the kernel blob, leaving the rest of it intact.
 */
static int chromeos_invalidate_kernel_bio(struct block_device *root_bdev)
{
	int ret = 0;
	struct block_device *bdev;
	struct bio *bio;
	struct page *page;
	dev_t devt;
	blk_mode_t dev_mode;

	devt = get_boot_dev();
	if (!devt) {
		devt = get_boot_dev_from_root_dev(root_bdev);
		if (!devt)
			return -EINVAL;
	}

	/* First we open the device for reading. */
	dev_mode = BLK_OPEN_READ | BLK_OPEN_EXCL;
	bdev = blkdev_get_by_dev(devt, dev_mode,
				 chromeos_invalidate_kernel_bio, NULL);
	if (IS_ERR(bdev)) {
		DMERR("invalidate_kernel: could not open device for reading");
		dev_mode = 0;
		ret = -1;
		goto failed_to_read;
	}

	bio = bio_alloc(bdev, 1, REQ_OP_READ | REQ_SYNC, GFP_NOIO);
	if (!bio) {
		ret = -1;
		goto failed_bio_alloc;
	}

	page = alloc_page(GFP_NOIO);
	if (!page) {
		ret = -ENOMEM;
		goto failed_to_alloc_page;
	}

	if (bdev_logical_block_size(bdev) > page_size(page)) {
		ret = -1;
		goto failed_to_submit_read;
	}

	/*
	 * Request read operation with REQ_PREFLUSH flag to ensure that the
	 * cache of non-volatile storage device has been flushed before read is
	 * started.
	 */
	if (chromeos_invalidate_kernel_submit(bio, 0,
					      bdev_logical_block_size(bdev),
					      page)) {
		ret = -1;
		goto failed_to_submit_read;
	}

	/* We have a page. Let's make sure it looks right. */
	if (memcmp("CHROMEOS", page_address(page), 8)) {
		DMERR("invalidate_kernel called on non-kernel partition");
		ret = -EINVAL;
		goto invalid_header;
	} else {
		DMERR("invalidate_kernel: found CHROMEOS kernel partition");
	}

	/* Stamp it and rewrite */
	memcpy(page_address(page), DMVERROR, strlen(DMVERROR));

	/* The block dev was being changed on read. Let's reopen here. */
	blkdev_put(bdev, chromeos_invalidate_kernel_bio);
	dev_mode = BLK_OPEN_WRITE | BLK_OPEN_EXCL;
	bdev = blkdev_get_by_dev(devt, dev_mode,
				 chromeos_invalidate_kernel_bio, NULL);
	if (IS_ERR(bdev)) {
		DMERR("invalidate_kernel: could not open device for writing");
		dev_mode = 0;
		ret = -1;
		goto failed_to_write;
	}

	/* We re-use the same bio to do the write after the read. Need to reset
	 * it to initialize bio->bi_remaining.
	 */
	bio_reset(bio, bdev, REQ_OP_WRITE | REQ_SYNC | REQ_FUA);

	/*
	 * Request write operation with REQ_FUA flag to ensure that I/O
	 * completion for the write is signaled only after the data has been
	 * committed to non-volatile storage.
	 */
	if (chromeos_invalidate_kernel_submit(bio, 0,
					      bdev_logical_block_size(bdev),
					      page)) {
		ret = -1;
		goto failed_to_submit_write;
	}

	DMERR("invalidate_kernel: completed.");
	ret = 0;
failed_to_submit_write:
failed_to_write:
invalid_header:
	__free_page(page);
failed_to_submit_read:
	/* Technically, we'll leak a page with the pending bio, but
	 *  we're about to panic so it's safer to do the panic() we expect.
	 */
failed_to_alloc_page:
	bio_put(bio);
failed_bio_alloc:
	if (dev_mode)
		blkdev_put(bdev, chromeos_invalidate_kernel_bio);
failed_to_read:
	return ret;
}

static inline __le32
efi_crc32(const void *buf, unsigned long len)
{
	return cpu_to_le32(crc32(~0L, buf, len) ^ ~0L);
}

static int chromeos_gpt_io_submit(struct bio *bio,
				  struct block_device *bdev,
				  unsigned int op,
				  unsigned int op_flags,
				  sector_t hdr_lba,
				  struct page *hdr_page,
				  struct page *tbl_pages)
{
	size_t block_size = bdev_logical_block_size(bdev);
	size_t sectors_per_lba = block_size >> SECTOR_SHIFT;
	int tbl_sector;
	struct gpt_header *header;

	if (bdev_logical_block_size(bdev) > page_size(hdr_page))
		return -1;

	bio_reset(bio, bdev, op | op_flags);
	if (chromeos_invalidate_kernel_submit(bio,
					      hdr_lba * sectors_per_lba,
					      block_size,
					      hdr_page)) {
		return  -1;
	}

	header = page_address(hdr_page);
	tbl_sector = le64_to_cpu(header->partition_entry_lba) * sectors_per_lba;

	bio_reset(bio, bdev, op | op_flags);
	if (chromeos_invalidate_kernel_submit(bio,
					      tbl_sector,
					      GPT_TABLE_SIZE,
					      tbl_pages)) {
		return  -1;
	}

	return 0;
}

static int chromeos_increment_gpt_err_count(struct page *hdr_page,
					    struct page *tbl_pages,
					    u8 active_gpt_entry_id)
{
	struct gpt_header *header = page_address(hdr_page);
	struct gpt_entry *entries = page_address(tbl_pages);
	struct gpt_entry *active_entry = &entries[active_gpt_entry_id];
	u64 gpt_table_size =
		(u64)le32_to_cpu(header->num_partition_entries) *
		le32_to_cpu(header->sizeof_partition_entry);

	if (gpt_table_size > GPT_TABLE_SIZE)
		return -1;

	if (active_entry->attributes.verity_error_counter == 1)
		return -1;

	active_entry->attributes.verity_error_counter = 1;
	header->partition_entry_array_crc32 =
		efi_crc32((const unsigned char *) (entries),
		gpt_table_size);
	header->header_crc32 = 0;
	header->header_crc32 = efi_crc32((const unsigned char *) (header),
		le32_to_cpu(header->header_size));

	return 0;
}

static int chromeos_handle_retries(struct bio *bio,
				   dev_t devt,
				   u8 active_gpt_entry_id,
				   sector_t hdr_lba,
				   struct page *hdr_page,
				   struct page *tbl_pages)
{
	struct block_device *bdev;
	fmode_t dev_mode = 0;
	int ret = 0;

	dev_mode = FMODE_READ;
	bdev = blkdev_get_by_dev(devt, dev_mode,
				 chromeos_handle_retries, NULL);
	if (IS_ERR(bdev)) {
		DMERR("update_tries: could not open device for reading: %ld",
		      PTR_ERR(bdev));
		dev_mode = 0;
		ret = -1;
		goto failed;
	}

	/*
	 * Request read operation with REQ_PREFLUSH flag to ensure that the
	 * cache of non-volatile storage device has been flushed before read is
	 * started.
	 */
	if (chromeos_gpt_io_submit(bio, bdev, REQ_OP_READ,
				   REQ_SYNC | REQ_PREFLUSH, hdr_lba,
				   hdr_page, tbl_pages)) {
		DMERR("update_tries: failed reading %s GPT",
		      hdr_lba == 1 ? "primary" : "secondary");
		ret = -1;
		goto failed;
	}

	if (chromeos_increment_gpt_err_count(hdr_page, tbl_pages,
					     active_gpt_entry_id)) {
		DMERR("update_tries: retries exceeded");
		ret = -1;
		goto failed;
	}

	/* The block dev was being changed on read. Let's reopen here. */
	blkdev_put(bdev, chromeos_handle_retries);
	dev_mode = FMODE_WRITE;
	bdev = blkdev_get_by_dev(devt, dev_mode,
				 chromeos_handle_retries, NULL);
	if (IS_ERR(bdev)) {
		DMERR("update_tries: could not open device for writing");
		dev_mode = 0;
		ret = -1;
		goto failed;
	}

	/*
	 * Request write operation with REQ_FUA flag to ensure that I/O
	 * completion for the write is signaled only after the data has been
	 * committed to non-volatile storage.
	 */
	if (chromeos_gpt_io_submit(bio, bdev, REQ_OP_WRITE,
				   REQ_SYNC | REQ_FUA, hdr_lba,
				   hdr_page, tbl_pages)) {
		DMERR("update_tries: failed writing %s GPT",
		      hdr_lba == 1 ? "primary" : "secondary");
		ret = -1;
		goto failed;
	}

failed:
	if (dev_mode)
		blkdev_put(bdev, chromeos_handle_retries);

	if (!ret)
		DMERR("update_tries: updated %s GPT",
		      hdr_lba == 1 ? "primary" : "secondary");

	return ret;
}

static int chromeos_update_tries(struct block_device *root_bdev)
{
	int ret = 0;
	struct bio *bio;
	struct page *hdr_page = NULL;
	struct page *tbl_pages = NULL;
	struct gpt_header *header;
	dev_t gpt_devt =
		disk_devt(dev_to_disk(&root_bdev->bd_disk->part0->bd_device));
	dev_t kernel_devt =
		get_boot_dev() ?: get_boot_dev_from_root_dev(root_bdev);
	struct block_device *kernel_bdev;
	u8 kernel_gpt_entry_id;

	if (!gpt_devt)
		return -EINVAL;

	/* Get block device to get the partno. */
	kernel_bdev = blkdev_get_by_dev(kernel_devt, FMODE_READ,
				 chromeos_update_tries, NULL);
	if (IS_ERR(kernel_bdev))
		return PTR_ERR(kernel_bdev);
	/*
	 * GPT entry offset is 0 based, and partno, represented by MINOR,
	 * is 1 based, so subtract 1.
	 */
	kernel_gpt_entry_id = kernel_bdev->bd_partno - 1;
	blkdev_put(kernel_bdev, chromeos_update_tries);

	bio = bio_alloc(root_bdev, 1, 0, GFP_NOIO);
	if (!bio) {
		ret = -1;
		goto failed_bio_alloc;
	}

	hdr_page = alloc_page(GFP_NOIO);
	tbl_pages = alloc_pages(GFP_NOIO, GPT_TABLE_PAGE_NUM_ORDER);
	if (!hdr_page || !tbl_pages) {
		ret = -ENOMEM;
		goto failed;
	}

	header = page_address(hdr_page);

	if (chromeos_handle_retries(bio, gpt_devt, kernel_gpt_entry_id, 1,
				    hdr_page, tbl_pages) ||
	    chromeos_handle_retries(bio, gpt_devt, kernel_gpt_entry_id,
				    le64_to_cpu(header->alternate_lba),
				    hdr_page, tbl_pages)) {
		DMERR("update_tries: retry failed, will invalidated kernel");
		ret = -1;
		goto failed;
	}

	DMERR("update_tries: completed");
	ret = 0;
failed:
	if (hdr_page)
		__free_page(hdr_page);
	if (tbl_pages)
		__free_pages(tbl_pages, GPT_TABLE_PAGE_NUM_ORDER);
	bio_put(bio);
failed_bio_alloc:
	return ret;
}

static int error_handler(struct notifier_block *nb, unsigned long transient,
			 void *opaque_err)
{
	struct dm_verity_error_state *err =
		(struct dm_verity_error_state *) opaque_err;
	err->behavior = DM_VERITY_ERROR_BEHAVIOR_PANIC;
	if (transient)
		return 0;

	// Do not invalidate kernel if successfully updated try count.
	if (!chromeos_update_tries(err->dev))
		return 0;

	/* Mark the kernel partition as invalid. */
	chromeos_invalidate_kernel_bio(err->dev);
	return 0;
}

static struct notifier_block chromeos_nb = {
	.notifier_call = &error_handler,
	.next = NULL,
	.priority = 1,
};

static int __init dm_verity_chromeos_init(void)
{
	int r;

	r = dm_verity_register_error_notifier(&chromeos_nb);
	if (r < 0)
		DMERR("failed to register handler: %d", r);
	else
		DMINFO("dm-verity-chromeos registered");
	return r;
}

static void __exit dm_verity_chromeos_exit(void)
{
	dm_verity_unregister_error_notifier(&chromeos_nb);
}

module_init(dm_verity_chromeos_init);
module_exit(dm_verity_chromeos_exit);

MODULE_AUTHOR("Will Drewry <wad@chromium.org>");
MODULE_DESCRIPTION("chromeos-specific error handler for dm-verity");
MODULE_LICENSE("GPL");

/* Declare parameter with no module prefix */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX	""
module_param_string(kern_guid, kern_guid, sizeof(kern_guid), 0);
