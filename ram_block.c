/* Disk on RAM Driver */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/genhd.h> // For basic block driver framework
#include <linux/blkdev.h> // For at least, struct block_device_operations
#include <linux/hdreg.h> // For struct hd_geometry
#include <linux/errno.h>

#include "ram_device.h"

#define MYDISK_FIRST_MINOR 0
#define MYDISK_MINOR_CNT 16

static u_int mydisk_major = 0;

/* 
 * The internal structure representation of our Device
 */
static struct mydisk_device
{
	/* Size is the size of the device (in sectors) */
	unsigned int size;
	/* For exclusive access to our request queue */
	spinlock_t lock;
	/* Our request queue */
	struct request_queue *mydisk_queue;
	/* This is kernel's representation of an individual disk device */
	struct gendisk *mydisk_disk;
} mydisk_dev;

static int mydisk_open(struct block_device *bdev, fmode_t mode)
{
	unsigned unit = iminor(bdev->bd_inode);

	printk(KERN_INFO "mydisk: Device is opened\n");
	printk(KERN_INFO "mydisk: Inode number is %d\n", unit);

	if (unit > MYDISK_MINOR_CNT)
		return -ENODEV;
	return 0;
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0))
static int mydisk_close(struct gendisk *disk, fmode_t mode)
{
	printk(KERN_INFO "mydisk: Device is closed\n");
	return 0;
}
#else
static void mydisk_close(struct gendisk *disk, fmode_t mode)
{
	printk(KERN_INFO "mydisk: Device is closed\n");
}
#endif

static int mydisk_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	geo->heads = 1;
	geo->cylinders = 32;
	geo->sectors = 32;
	geo->start = 0;
	return 0;
}

/* 
 * Actual Data transfer
 */
static int mydisk_transfer(struct request *req)
{
	//struct mydisk_device *dev = (struct mydisk_device *)(req->rq_disk->private_data);

	int dir = rq_data_dir(req);
	sector_t start_sector = blk_rq_pos(req);
	unsigned int sector_cnt = blk_rq_sectors(req);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,14,0))
#define BV_PAGE(bv) ((bv)->bv_page)
#define BV_OFFSET(bv) ((bv)->bv_offset)
#define BV_LEN(bv) ((bv)->bv_len)
	struct bio_vec *bv;
#else
#define BV_PAGE(bv) ((bv).bv_page)
#define BV_OFFSET(bv) ((bv).bv_offset)
#define BV_LEN(bv) ((bv).bv_len)
	struct bio_vec bv;
#endif
	struct req_iterator iter;

	sector_t sector_offset;
	unsigned int sectors;
	u8 *buffer;

	int ret = 0;

	//printk(KERN_DEBUG "mydisk: Dir:%d; Sec:%lld; Cnt:%d\n", dir, start_sector, sector_cnt);

	sector_offset = 0;
	rq_for_each_segment(bv, req, iter)
	{
		buffer = page_address(BV_PAGE(bv)) + BV_OFFSET(bv);
		if (BV_LEN(bv) % MYDISK_SECTOR_SIZE != 0)
		{
			printk(KERN_ERR "mydisk: Should never happen: "
				"bio size (%d) is not a multiple of MYDISK_SECTOR_SIZE (%d).\n"
				"This may lead to data truncation.\n",
				BV_LEN(bv), MYDISK_SECTOR_SIZE);
			ret = -EIO;
		}
		sectors = BV_LEN(bv) / MYDISK_SECTOR_SIZE;
		printk(KERN_DEBUG "mydisk: Start Sector: %llu, Sector Offset: %llu; Buffer: %p; Length: %u sectors\n",
			(unsigned long long)(start_sector), (unsigned long long)(sector_offset), buffer, sectors);
		if (dir == WRITE) /* Write to the device */
		{
			ramdevice_write(start_sector + sector_offset, buffer, sectors);
		}
		else /* Read from the device */
		{
			ramdevice_read(start_sector + sector_offset, buffer, sectors);
		}
		sector_offset += sectors;
	}
	if (sector_offset != sector_cnt)
	{
		printk(KERN_ERR "mydisk: bio info doesn't match with the request info");
		ret = -EIO;
	}

	return ret;
}
	
/*
 * Represents a block I/O request for us to execute
 */
static void mydisk_request(struct request_queue *q)
{
	struct request *req;
	int ret;

	/* Gets the current request from the dispatch queue */
	while ((req = blk_fetch_request(q)) != NULL)
	{
#if 0
		/*
		 * This function tells us whether we are looking at a filesystem request
		 * - one that moves block of data
		 */
		if (!blk_fs_request(req))
		{
			printk(KERN_NOTICE "mydisk: Skip non-fs request\n");
			/* We pass 0 to indicate that we successfully completed the request */
			__blk_end_request_all(req, 0);
			//__blk_end_request(req, 0, blk_rq_bytes(req));
			continue;
		}
#endif
		ret = mydisk_transfer(req);
		__blk_end_request_all(req, ret);
		//__blk_end_request(req, ret, blk_rq_bytes(req));
	}
}

/* 
 * These are the file operations that performed on the ram block device
 */
static struct block_device_operations mydisk_fops =
{
	.owner = THIS_MODULE,
	.open = mydisk_open,
	.release = mydisk_close,
	.getgeo = mydisk_getgeo,
};
	
/* 
 * This is the registration and initialization section of the ram block device
 * driver
 */
static int __init mydisk_init(void)
{
	int ret;

	/* Set up our RAM Device */
	if ((ret = ramdevice_init()) < 0)
	{
		return ret;
	}
	mydisk_dev.size = ret;

	/* Get Registered */
	mydisk_major = register_blkdev(mydisk_major, "mydisk");
	if (mydisk_major <= 0)
	{
		printk(KERN_ERR "mydisk: Unable to get Major Number\n");
		ramdevice_cleanup();
		return -EBUSY;
	}

	/* Get a request queue (here queue is created) */
	spin_lock_init(&mydisk_dev.lock);
	mydisk_dev.mydisk_queue = blk_init_queue(mydisk_request, &mydisk_dev.lock);
	if (mydisk_dev.mydisk_queue == NULL)
	{
		printk(KERN_ERR "mydisk: blk_init_queue failure\n");
		unregister_blkdev(mydisk_major, "mydisk");
		ramdevice_cleanup();
		return -ENOMEM;
	}
	
	/*
	 * Add the gendisk structure
	 * By using this memory allocation is involved, 
	 * the minor number we need to pass bcz the device 
	 * will support this much partitions 
	 */
	mydisk_dev.mydisk_disk = alloc_disk(MYDISK_MINOR_CNT);
	if (!mydisk_dev.mydisk_disk)
	{
		printk(KERN_ERR "mydisk: alloc_disk failure\n");
		blk_cleanup_queue(mydisk_dev.mydisk_queue);
		unregister_blkdev(mydisk_major, "mydisk");
		ramdevice_cleanup();
		return -ENOMEM;
	}

 	/* Setting the major number */
	mydisk_dev.mydisk_disk->major = mydisk_major;
  	/* Setting the first mior number */
	mydisk_dev.mydisk_disk->first_minor = MYDISK_FIRST_MINOR;
 	/* Initializing the device operations */
	mydisk_dev.mydisk_disk->fops = &mydisk_fops;
 	/* Driver-specific own internal data */
	mydisk_dev.mydisk_disk->private_data = &mydisk_dev;
	mydisk_dev.mydisk_disk->queue = mydisk_dev.mydisk_queue;
	/*
	 * You do not want partition information to show up in 
	 * cat /proc/partitions set this flags
	 */
	//mydisk_dev.mydisk_disk->flags = GENHD_FL_SUPPRESS_PARTITION_INFO;
	sprintf(mydisk_dev.mydisk_disk->disk_name, "mydisk");
	/* Setting the capacity of the device in its gendisk structure */
	set_capacity(mydisk_dev.mydisk_disk, mydisk_dev.size);

	/* Adding the disk to the system */
	add_disk(mydisk_dev.mydisk_disk);
	/* Now the disk is "live" */
	printk(KERN_INFO "mydisk: Ram Block driver initialised (%d sectors; %d bytes)\n",
		mydisk_dev.size, mydisk_dev.size * MYDISK_SECTOR_SIZE);

	return 0;
}
/*
 * This is the unregistration and uninitialization section of the ram block
 * device driver
 */
static void __exit mydisk_cleanup(void)
{
	del_gendisk(mydisk_dev.mydisk_disk);
	put_disk(mydisk_dev.mydisk_disk);
	blk_cleanup_queue(mydisk_dev.mydisk_queue);
	unregister_blkdev(mydisk_major, "mydisk");
	ramdevice_cleanup();
}

module_init(mydisk_init);
module_exit(mydisk_cleanup);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anil Kumar Pugalia <email@sarika-pugs.com>");
MODULE_DESCRIPTION("Ram Block Driver");
MODULE_ALIAS_BLOCKDEV_MAJOR(mydisk_major);
