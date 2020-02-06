/*
 * dm-secdel: secure deletion on discard
 * (c) 2018-2019, vt@altlinux.org.
 *
 * Based on dm-linear:
 * Copyright (C) 2001-2003 Sistina Software (UK) Limited.
 *
 * This file is licensed under the GPL-2.0-only.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/dax.h>
#include <linux/slab.h>
#include <linux/device-mapper.h>
#include <linux/random.h>
#include <linux/version.h>

MODULE_AUTHOR("<vt@altlinux.org>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("dm-linear with secure deletion on discard");
MODULE_VERSION("1.0.7");

#define DM_MSG_PREFIX "secdel"

unsigned long empty_ff_page[PAGE_SIZE / sizeof(unsigned long)];

/*
 * Linear: maps a linear range of a device.
 */
struct secdel_c {
	struct dm_dev *dev;
	sector_t start;
	char patterns[];
};

/*
 * Construct a linear mapping: <dev_path> <offset> [patterns]
 */
static int secdel_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct secdel_c *lc;
	unsigned long long tmp;
	size_t passes = 0;
	char dummy;
	int ret;
	int i;

	if (argc < 2) {
		ti->error = "Invalid argument count";
		return -EINVAL;
	}

	if (argc > 2)
		passes = strlen(argv[2]);

	/* +2 to allow empty argv[2] to be replaced with "R" */
	lc = kmalloc(sizeof(*lc) + passes + 2, GFP_KERNEL);
	if (lc == NULL) {
		ti->error = "Cannot allocate secdel context";
		return -ENOMEM;
	}

	ret = -EINVAL;
	if (sscanf(argv[1], "%llu%c", &tmp, &dummy) != 1 || tmp != (sector_t)tmp) {
		ti->error = "Invalid device sector";
		goto bad;
	}
	lc->start = tmp;

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &lc->dev);
	if (ret) {
		ti->error = "Device lookup failed";
		goto bad;
	}

	/* empty or unset argv[2] is replaced with "R" */
	if (argc > 2 && passes > 1)
		strcpy(lc->patterns, argv[2]);
	else
		strcpy(lc->patterns, "R");

	/* sanity check erasure patterns */
	passes = strlen(lc->patterns);
	for (i = 0; i < passes; i++) {
		switch (lc->patterns[i]) {
		case '0':
		case '1':
		case 'R':
			continue;
		default:
			ti->error = "Invalid character in patterns";
			ret = -EINVAL;
			goto bad;
		}
	}

	/* permit discards no matter if underlying device supports them */
	ti->discards_supported = 1;

	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,16,0)
	ti->num_secure_erase_bios = 1;
#endif
	ti->num_write_same_bios = 1;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
	ti->num_write_zeroes_bios = 1;
#endif
	ti->private = lc;
	DMINFO("start dev=%s src=%s patterns=%s",
	       dm_device_name(dm_table_get_md(ti->table)),
	       argv[0], lc->patterns);
	return 0;

      bad:
	kfree(lc);
	return ret;
}

static void secdel_dtr(struct dm_target *ti)
{
	struct secdel_c *lc = ti->private;

	DMINFO("stop dev=%s", dm_device_name(dm_table_get_md(ti->table)));
	dm_put_device(ti, lc->dev);
	kfree(lc);
}

static sector_t secdel_map_sector(struct dm_target *ti, sector_t bi_sector)
{
	struct secdel_c *lc = ti->private;

	return lc->start + dm_target_offset(ti, bi_sector);
}

#ifndef bio_set_dev
#define bio_set_dev(bio, x) (bio)->bi_bdev = (x)
#endif

static void secdel_map_bio(struct dm_target *ti, struct bio *bio)
{
	struct secdel_c *lc = ti->private;

	bio_set_dev(bio, lc->dev->bdev);
	if (bio_sectors(bio)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
	    || bio_op(bio) == REQ_OP_ZONE_RESET
#endif
	    )
		bio->bi_iter.bi_sector =
			secdel_map_sector(ti, bio->bi_iter.bi_sector);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,13,0)
#define bi_status bi_error
#endif
static void bio_end_erase(struct bio *bio)
{
	struct bio_vec *bvec;
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,2,0)
	int i;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,1,0)
	struct bvec_iter_all iter_all;
#endif

	if (bio->bi_status)
		DMERR("bio_end_erase %llu[%u] error=%d",
		    (long long unsigned int)bio->bi_iter.bi_sector,
		    bio->bi_iter.bi_size >> 9,
		    bio->bi_status);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,2,0)
	bio_for_each_segment_all(bvec, bio, iter_all)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5,1,0)
	bio_for_each_segment_all(bvec, bio, i, iter_all)
#else
	bio_for_each_segment_all(bvec, bio, i)
#endif
		if (bvec->bv_page != ZERO_PAGE(0) &&
		    bvec->bv_page != virt_to_page(empty_ff_page))
			__free_page(bvec->bv_page);
	bio_put(bio);
}

static void secdel_submit_bio(struct bio *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0)
	bio_set_op_attrs(bio, REQ_OP_WRITE, 0);
	submit_bio(bio);
#else
	submit_bio(WRITE, bio);
#endif

}

static int op_discard(struct bio *bio)
{
	if (
#ifdef bio_op
	bio_op(bio) == REQ_OP_DISCARD
#else
	bio->bi_rw & REQ_DISCARD
#endif
	)
		return true;
	return false;
}

/*
 * send amount of masking data to the device
 * @mode: 0 to write zeros, 1 to write ff-s,
 *       -1 to write random data
 */
static int issue_erase(struct block_device *bdev, sector_t sector,
    sector_t nr_sects, int mode)
{
	int ret = 0;
	const gfp_t gfp_mask = GFP_NOFS;

	while (nr_sects) {
		struct bio *bio;
		unsigned int nrvecs = min(nr_sects,
		    (sector_t)BIO_MAX_PAGES >> 3);

		DMDEBUG("bio_alloc<%llu[%llu]> %u", (unsigned long long)sector,
		    (unsigned long long)nr_sects, nrvecs);
		bio = bio_alloc(gfp_mask, nrvecs);
		if (!bio) {
			DMERR("%s %llu[%llu]: no memory to allocate bio (%u)",
			    __func__, (unsigned long long)sector,
			    (unsigned long long)nr_sects, nrvecs);
			ret = -ENOMEM;
			break;
		}

		bio->bi_iter.bi_sector = sector;
		bio_set_dev(bio, bdev);
		bio->bi_end_io = bio_end_erase;

		while (nr_sects != 0) {
			unsigned int sz;
			struct page *page = NULL;

			sz = min((sector_t)PAGE_SIZE >> 9, nr_sects);
			if (mode < 0) {
				page = alloc_page(gfp_mask);
				if (!page) {
					DMERR("issue_erase %llu[%llu]: no memory to allocate page for random data",
					    (unsigned long long)sector, (unsigned long long)nr_sects);
					/* will fallback to zero filling */
				} else {
					void *ptr;

					ptr = kmap_atomic(page);
					get_random_bytes(ptr, sz << 9);
					kunmap_atomic(ptr);
				}
			} else if (mode == 1)
				page = virt_to_page(empty_ff_page);
			if (!page)
				page = ZERO_PAGE(0);
			ret = bio_add_page(bio, page, sz << 9, 0);
			if (!ret && page != ZERO_PAGE(0) &&
			    page != virt_to_page(empty_ff_page))
				__free_page(page);
			nr_sects -= ret >> 9;
			sector += ret >> 9;
			if (ret < (sz << 9))
				break;
		}
		ret = 0;
		secdel_submit_bio(bio);
		cond_resched();
	}

	return ret;
}

/* convert discards into writes */
static int secdel_map_discard(struct dm_target *ti, struct bio *sbio)
{
	struct secdel_c *lc = ti->private;
	struct block_device *bdev = lc->dev->bdev;
	sector_t sector = sbio->bi_iter.bi_sector;
	sector_t nr_sects = bio_sectors(sbio);
	size_t passes, i;

	if (!bio_sectors(sbio))
		return 0;
	if (!op_discard(sbio))
		return 0;

	BUG_ON(sbio->bi_vcnt != 0);

	DMDEBUG("DISCARD %llu: %u sectors",
	    (unsigned long long)sbio->bi_iter.bi_sector,
	    bio_sectors(sbio));

	bio_endio(sbio);

	passes = strlen(lc->patterns);
	for (i = 0; i < passes; i++) {
		int mode;

		switch (lc->patterns[i]) {
		case '0': mode = 0; break;
		case '1': mode = 1; break;
		case 'R':
		default:
			  mode = -1;
		}
		issue_erase(bdev, sector, nr_sects, mode);
	}
	return 1;
}

static int secdel_map(struct dm_target *ti, struct bio *bio)
{
	secdel_map_bio(ti, bio);
	if (secdel_map_discard(ti, bio))
		return DM_MAPIO_SUBMITTED;
	return DM_MAPIO_REMAPPED;
}

#ifdef CONFIG_BLK_DEV_ZONED
# if LINUX_VERSION_CODE >= KERNEL_VERSION(4,20,0)
static int secdel_report_zones(struct dm_target *ti, sector_t sector,
			       struct blk_zone *zones, unsigned int *nr_zones)
{
	struct secdel_c *lc = (struct secdel_c *)ti->private;
	int ret;

	/* Do report and remap it */
	ret = blkdev_report_zones(lc->dev->bdev, secdel_map_sector(ti, sector),
				  zones, nr_zones);
	if (ret != 0)
		return ret;

	if (*nr_zones)
		dm_remap_zone_report(ti, lc->start, zones, nr_zones);
	return 0;
}
# elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
static int secdel_end_io(struct dm_target *ti, struct bio *bio,
			 blk_status_t *error)
{
	struct secdel_c *lc = ti->private;

	if (!*error && bio_op(bio) == REQ_OP_ZONE_REPORT)
		dm_remap_zone_report(ti, bio, lc->start);

	return DM_ENDIO_DONE;
}
# endif
#endif

static void secdel_status(struct dm_target *ti, status_type_t type,
			  unsigned status_flags, char *result, unsigned maxlen)
{
	struct secdel_c *lc = ti->private;

	switch (type) {
	case STATUSTYPE_INFO:
		result[0] = '\0';
		break;

	case STATUSTYPE_TABLE:
		snprintf(result, maxlen, "%s %llu %s", lc->dev->name,
				(unsigned long long)lc->start,
				lc->patterns);
		break;
	}
}

static int secdel_prepare_ioctl(struct dm_target *ti, struct block_device **bdev
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,16,0)

		, fmode_t *mode
#endif
		)
{
	struct secdel_c *lc = ti->private;
	struct dm_dev *dev = lc->dev;

	*bdev = dev->bdev;

	/*
	 * Only pass ioctls through if the device sizes match exactly.
	 */
	if (lc->start ||
	    ti->len != i_size_read(dev->bdev->bd_inode) >> SECTOR_SHIFT)
		return 1;
	return 0;
}

static int secdel_iterate_devices(struct dm_target *ti,
				  iterate_devices_callout_fn fn, void *data)
{
	struct secdel_c *lc = ti->private;

	return fn(ti, lc->dev, lc->start, ti->len, data);
}

static void secdel_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct secdel_c *lc = ti->private;

	limits->discard_granularity = bdev_logical_block_size(lc->dev->bdev);
	limits->max_discard_sectors = PAGE_SIZE >> 9;
}

#if IS_ENABLED(CONFIG_DAX_DRIVER)
# if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
static long secdel_dax_direct_access(struct dm_target *ti, pgoff_t pgoff,
				    long nr_pages, void **kaddr, pfn_t *pfn)
{
	long ret;
	struct secdel_c *lc = ti->private;
	struct block_device *bdev = lc->dev->bdev;
	struct dax_device *dax_dev = lc->dev->dax_dev;
	sector_t dev_sector, sector = pgoff * PAGE_SECTORS;

	dev_sector = secdel_map_sector(ti, sector);
	ret = bdev_dax_pgoff(bdev, dev_sector, nr_pages * PAGE_SIZE, &pgoff);
	if (ret)
		return ret;
	return dax_direct_access(dax_dev, pgoff, nr_pages, kaddr, pfn);
}
# elif LINUX_VERSION_CODE > KERNEL_VERSION(4,5,0)
static long secdel_direct_access(struct dm_target *ti, sector_t sector,
				 void **kaddr, pfn_t *pfn, long size)
{
	struct secdel_c *lc = ti->private;
	struct block_device *bdev = lc->dev->bdev;
	struct blk_dax_ctl dax = {
		.sector = secdel_map_sector(ti, sector),
		.size = size,
	};
	long ret;

	ret = bdev_direct_access(bdev, &dax);
	*kaddr = dax.addr;
	*pfn = dax.pfn;

	return ret;
}
# endif

# if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
static size_t secdel_dax_copy_from_iter(struct dm_target *ti, pgoff_t pgoff,
					void *addr, size_t bytes, struct iov_iter *i)
{
	struct secdel_c *lc = ti->private;
	struct block_device *bdev = lc->dev->bdev;
	struct dax_device *dax_dev = lc->dev->dax_dev;
	sector_t dev_sector, sector = pgoff * PAGE_SECTORS;

	dev_sector = secdel_map_sector(ti, sector);
	if (bdev_dax_pgoff(bdev, dev_sector, ALIGN(bytes, PAGE_SIZE), &pgoff))
		return 0;
	return dax_copy_from_iter(dax_dev, pgoff, addr, bytes, i);
}
# endif
#else
# define secdel_dax_direct_access NULL
# define secdel_direct_access NULL
# define secdel_dax_copy_from_iter NULL
#endif

static struct target_type secdel_target = {
	.name   = "secdel",
	.version = {1, 0, 3},
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
	.features = DM_TARGET_PASSES_INTEGRITY | DM_TARGET_ZONED_HM,
#endif
	.module = THIS_MODULE,
	.ctr    = secdel_ctr,
	.dtr    = secdel_dtr,
	.map    = secdel_map,
#ifdef CONFIG_BLK_DEV_ZONED
# if LINUX_VERSION_CODE >= KERNEL_VERSION(4,20,0)
	.report_zones = secdel_report_zones,
# elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
	.end_io = secdel_end_io,
# endif
#endif
	.status = secdel_status,
	.prepare_ioctl = secdel_prepare_ioctl,
	.io_hints = secdel_io_hints,
	.iterate_devices = secdel_iterate_devices,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
	.direct_access = secdel_dax_direct_access,
#elif LINUX_VERSION_CODE > KERNEL_VERSION(4,5,0)
	.direct_access = secdel_direct_access,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
	.dax_copy_from_iter = secdel_dax_copy_from_iter,
#endif
};

int __init dm_secdel_init(void)
{
	int r = dm_register_target(&secdel_target);

	if (r < 0)
		DMERR("register failed %d", r);
	memset(empty_ff_page, 0xff, sizeof(empty_ff_page));
	return r;
}

void dm_secdel_exit(void)
{
	dm_unregister_target(&secdel_target);
}

module_init(dm_secdel_init);
module_exit(dm_secdel_exit);
