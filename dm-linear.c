/*
 * Copyright (C) 2001-2003 Sistina Software (UK) Limited.
 *
 * (c) 2018, vt@altlinux.org: secure deletion on discard
 *
 * This file is released under the GPL.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/device-mapper.h>
#include <linux/random.h>

MODULE_AUTHOR("<vt@altlinux.org>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("dm-linear with secure deletion on discard");

#define DM_MSG_PREFIX "secdel"

/*
 * Linear: maps a linear range of a device.
 */
struct linear_c {
	struct dm_dev *dev;
	sector_t start;
};

/*
 * Construct a linear mapping: <dev_path> <offset>
 */
static int linear_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct linear_c *lc;
	unsigned long long tmp;
	char dummy;
	int ret;

	if (argc != 2) {
		ti->error = "Invalid argument count";
		return -EINVAL;
	}

	lc = kmalloc(sizeof(*lc), GFP_KERNEL);
	if (lc == NULL) {
		ti->error = "Cannot allocate linear context";
		return -ENOMEM;
	}

	ret = -EINVAL;
	if (sscanf(argv[1], "%llu%c", &tmp, &dummy) != 1) {
		ti->error = "Invalid device sector";
		goto bad;
	}
	lc->start = tmp;

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &lc->dev);
	if (ret) {
		ti->error = "Device lookup failed";
		goto bad;
	}

	/* permit discards no matter if underlying device supports them */
	ti->discards_supported = 1;

	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;
	ti->num_write_same_bios = 1;
	ti->private = lc;
	return 0;

      bad:
	kfree(lc);
	return ret;
}

static void linear_dtr(struct dm_target *ti)
{
	struct linear_c *lc = (struct linear_c *) ti->private;

	dm_put_device(ti, lc->dev);
	kfree(lc);
}

static sector_t linear_map_sector(struct dm_target *ti, sector_t bi_sector)
{
	struct linear_c *lc = ti->private;

	return lc->start + dm_target_offset(ti, bi_sector);
}

static void linear_map_bio(struct dm_target *ti, struct bio *bio)
{
	struct linear_c *lc = ti->private;

	bio->bi_bdev = lc->dev->bdev;
	if (bio_sectors(bio))
		bio->bi_iter.bi_sector =
			linear_map_sector(ti, bio->bi_iter.bi_sector);
}

static void bio_end_erase(struct bio *bio)
{
	struct bio_vec *bvec;
	int i;

	if (bio->bi_error)
		DMERR("bio_end_erase %lu[%u] error=%d",
		    bio->bi_iter.bi_sector,
		    bio->bi_iter.bi_size >> 9,
		    bio->bi_error);
	bio_for_each_segment_all(bvec, bio, i)
		if (bvec->bv_page != ZERO_PAGE(0))
			__free_page(bvec->bv_page);
	bio_put(bio);
}

/*
 * send amount of masking data to the device
 * @mode: 0 to write zeros, otherwise to write random data
 */
static int issue_erase(struct block_device *bdev, sector_t sector,
    sector_t nr_sects, gfp_t gfp_mask, int mode)
{
	int ret = 0;
	struct bio *bio;
	unsigned int sz;

	while (nr_sects) {
		DMDEBUG("bio_alloc<%lu[%lu]> %lu", sector, nr_sects,
		    min(nr_sects, (sector_t)BIO_MAX_PAGES));

		bio = bio_alloc(gfp_mask,
		    min(nr_sects, (sector_t)BIO_MAX_PAGES));
		if (!bio) {
			DMERR("issue_erase %lu[%lu]: no memory to allocate bio",
			    sector, nr_sects);
			ret = -ENOMEM;
			break;
		}

		bio->bi_iter.bi_sector = sector;
		bio->bi_bdev   = bdev;
		bio->bi_end_io = bio_end_erase;

		while (nr_sects != 0) {
			struct page *page = NULL;

			sz = min((sector_t) PAGE_SIZE >> 9, nr_sects);
			if (mode) {
				page = alloc_page(gfp_mask);
				if (!page) {
					DMERR("issue_erase %lu[%lu]: no memory to allocate page for random data",
					    sector, nr_sects);
					/* fallback to zero filling */
				} else {
					void *ptr;

					ptr = kmap_atomic(page);
					get_random_bytes(ptr, sz << 9);
					kunmap_atomic(ptr);
				}
			}
			if (!page)
				page = ZERO_PAGE(0);
			ret = bio_add_page(bio, page, sz << 9, 0);
			nr_sects -= ret >> 9;
			sector += ret >> 9;
			if (ret < (sz << 9))
				break;
		}
		ret = 0;
		bio_set_op_attrs(bio, REQ_OP_WRITE, 0);
		submit_bio(bio);
		cond_resched();
	}

	return ret;
}

/* convert discards into writes */
static int secdel_map_discard(struct dm_target *ti, struct bio *sbio)
{
	struct block_device *bi_bdev = sbio->bi_bdev;
	sector_t sector = sbio->bi_iter.bi_sector;
	sector_t nr_sects = bio_sectors(sbio);

	if (!bio_sectors(sbio))
		return 0;
	if (bio_op(sbio) != REQ_OP_DISCARD)
		return 0;

	BUG_ON(sbio->bi_vcnt != 0);

	DMDEBUG("DISCARD %lu: %u sectors", sbio->bi_iter.bi_sector,
	    bio_sectors(sbio));

	bio_endio(sbio);

	issue_erase(bi_bdev, sector, nr_sects, GFP_NOFS, 1);
	return 1;
}

static int linear_map(struct dm_target *ti, struct bio *bio)
{
	linear_map_bio(ti, bio);
	if (secdel_map_discard(ti, bio))
		return DM_MAPIO_SUBMITTED;
	return DM_MAPIO_REMAPPED;
}

static void linear_status(struct dm_target *ti, status_type_t type,
			  unsigned status_flags, char *result, unsigned maxlen)
{
	struct linear_c *lc = (struct linear_c *) ti->private;

	switch (type) {
	case STATUSTYPE_INFO:
		result[0] = '\0';
		break;

	case STATUSTYPE_TABLE:
		snprintf(result, maxlen, "%s %llu", lc->dev->name,
				(unsigned long long)lc->start);
		break;
	}
}

static int linear_prepare_ioctl(struct dm_target *ti,
		struct block_device **bdev, fmode_t *mode)
{
	struct linear_c *lc = (struct linear_c *) ti->private;
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

static int linear_iterate_devices(struct dm_target *ti,
				  iterate_devices_callout_fn fn, void *data)
{
	struct linear_c *lc = ti->private;

	return fn(ti, lc->dev, lc->start, ti->len, data);
}

static long linear_direct_access(struct dm_target *ti, sector_t sector,
				 void **kaddr, pfn_t *pfn, long size)
{
	struct linear_c *lc = ti->private;
	struct block_device *bdev = lc->dev->bdev;
	struct blk_dax_ctl dax = {
		.sector = linear_map_sector(ti, sector),
		.size = size,
	};
	long ret;

	ret = bdev_direct_access(bdev, &dax);
	*kaddr = dax.addr;
	*pfn = dax.pfn;

	return ret;
}

static struct target_type linear_target = {
	.name   = "secdel",
	.version = {1, 0, 0},
	.module = THIS_MODULE,
	.ctr    = linear_ctr,
	.dtr    = linear_dtr,
	.map    = linear_map,
	.status = linear_status,
	.prepare_ioctl = linear_prepare_ioctl,
	.iterate_devices = linear_iterate_devices,
	.direct_access = linear_direct_access,
};

int __init dm_linear_init(void)
{
	int r = dm_register_target(&linear_target);

	if (r < 0)
		DMERR("register failed %d", r);

	return r;
}

void dm_linear_exit(void)
{
	dm_unregister_target(&linear_target);
}

module_init(dm_linear_init);
module_exit(dm_linear_exit);
