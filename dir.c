#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mpage.h>
#include <linux/genhd.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/buffer_head.h>
#include "testfs.h"

const struct file_operations testfs_dir_fops = {
        .llseek         = generic_file_llseek,
        .read           = generic_read_dir,
        .unlocked_ioctl = testfs_ioctl,
#ifdef CONFIG_COMPAT
        .compat_ioctl   = testfs_compat_ioctl,
#endif
	.fsync		= testfs_fsync,
};

const struct inode_operations testfs_dir_iops = {
	.getattr        = testfs_getattr,
};
