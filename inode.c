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
#include <linux/fiemap.h>
#include <linux/iomap.h>
#include <linux/namei.h>
#include <linux/uio.h>
#include <linux/fiemap.h>

#include "testfs.h"

static struct kmem_cache *testfs_icachep;

static void init_once(void *foo)
{
	struct testfs_inode *ti = (struct testfs_inode *)foo;

	pr_err("%s,%d\n", __func__, __LINE__);

	inode_init_once(&ti->vfs_inode);
}

int testfs_inode_cache_init(void)
{
	pr_err("%s,%d\n", __func__, __LINE__);

	testfs_icachep = kmem_cache_create("testfs_icache",
				sizeof(struct testfs_inode), 0,
				(SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD|
					SLAB_ACCOUNT),
				init_once);
	if (testfs_icachep == NULL)
		return -ENOMEM;
	return 0;
}

void testfs_inode_cache_deinit(void)
{
	kmem_cache_destroy(testfs_icachep);
}

struct inode *testfs_alloc_inode(struct super_block *sb)
{
	struct testfs_inode *ti;

	pr_err("%s,%d\n", __func__, __LINE__);

	ti = kmem_cache_alloc(testfs_icachep, GFP_KERNEL);
	if (!ti)
		return NULL;

	return &ti->vfs_inode;
}

void testfs_free_inode(struct inode *inode)
{
	struct testfs_inode *ti = TESTFS_I(inode);

	pr_err("%s,%d\n", __func__, __LINE__);

	kmem_cache_free(testfs_icachep, ti);
}

static int testfs_get_blocks(struct inode *inode,
                           sector_t iblock, unsigned long maxblocks,
                           u32 *bno, bool *new, bool *boundary,
                           int create)
{
	return 0;
}

static int testfs_get_block(struct inode *inode, sector_t iblock,
                struct buffer_head *bh_result, int create)
{
        unsigned max_blocks = bh_result->b_size >> inode->i_blkbits;
        bool new = false, boundary = false;
        u32 bno;
        int ret;

        ret = testfs_get_blocks(inode, iblock, max_blocks, &bno, &new, &boundary,
                        create);
        if (ret <= 0)
                return ret;

        map_bh(bh_result, inode->i_sb, bno);
        bh_result->b_size = (ret << inode->i_blkbits);
        if (new)
                set_buffer_new(bh_result);
        if (boundary)
                set_buffer_boundary(bh_result);
        return 0;

}

static int testfs_readpage(struct file *file, struct page *page)
{
	return mpage_readpage(page, testfs_get_block);
}

static void testfs_readahead(struct readahead_control *rac)
{
        mpage_readahead(rac, testfs_get_block);
}

static void testfs_truncate_blocks(struct inode *inode, loff_t offset)
{
	/* truncate block bitmap, and disk inode ??? */
	if (!inode || !offset)
		return;
}

static void testfs_write_failed(struct address_space *mapping, loff_t to)
{
        struct inode *inode = mapping->host;

        if (to > inode->i_size) {
                truncate_pagecache(inode, inode->i_size);
                testfs_truncate_blocks(inode, inode->i_size);
        }
}

static int testfs_writepage(struct page *page, struct writeback_control *wbc)
{
        return block_write_full_page(page, testfs_get_block, wbc);
}

static int
testfs_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
        return mpage_writepages(mapping, wbc, testfs_get_block);
}

static int testfs_write_begin(struct file *file, struct address_space *mapping,
                loff_t pos, unsigned len, unsigned flags,
                struct page **pagep, void **fsdata)
{
        int ret;

        ret = block_write_begin(mapping, pos, len, flags, pagep,
                                testfs_get_block);
        if (ret < 0)
                testfs_write_failed(mapping, pos + len);
        return ret;
}

static int testfs_write_end(struct file *file, struct address_space *mapping,
                        loff_t pos, unsigned len, unsigned copied,
                        struct page *page, void *fsdata)
{
        int ret;

        ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
        if (ret < len)
                testfs_write_failed(mapping, pos + len);
        return ret;
}

static ssize_t testfs_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
{
        struct file *file = iocb->ki_filp;
        struct address_space *mapping = file->f_mapping;
        struct inode *inode = mapping->host;
        size_t count = iov_iter_count(iter);
        loff_t offset = iocb->ki_pos;
        ssize_t ret;

        ret = blockdev_direct_IO(iocb, inode, iter, testfs_get_block);
        if (ret < 0 && iov_iter_rw(iter) == WRITE)
                testfs_write_failed(mapping, offset + count);
        return ret;
}

const struct address_space_operations testfs_aops = {
        .readpage               = testfs_readpage,
        .readahead              = testfs_readahead,
        .writepage              = testfs_writepage,
        .writepages             = testfs_writepages,
        .write_begin            = testfs_write_begin,
        .write_end              = testfs_write_end,
        .direct_IO              = testfs_direct_IO,
};

/*
 * testfs_get_disk_inode - get inode from the disk's inode table
 *
 * @sb:  the super block
 * @ino: inode index
 * @bh:  buffer_head of the block that contains this inode
 *
 */
struct testfs_disk_inode *testfs_get_disk_inode(struct super_block *sb,
				ino_t ino, struct buffer_head **bh)
{
	unsigned long blkid, offset;
	struct buffer_head *tmp;

	/*
	 * get the block index which contains this inode, and the offset
	 * within that block
	 */
	if (testfs_get_block_and_offset(sb, ino - 1, &blkid, &offset))
		return ERR_PTR(-EINVAL);

	tmp = sb_bread(sb, blkid);
	if (!tmp)
		return ERR_PTR(-EIO);

	*bh = tmp;

	return (struct testfs_disk_inode *)(tmp->b_data + offset);
}

struct inode *testfs_iget(struct super_block *sb, int ino)
{
	struct inode *inode;
	struct testfs_inode *ti;
	struct testfs_disk_inode *tdi;
	struct buffer_head *bh;
	int err;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	ti = TESTFS_I(inode);

	/* read inode info from disk for this ino */
	tdi = testfs_get_disk_inode(sb, ino, &bh);
	if (IS_ERR(tdi)) {
		err = PTR_ERR(tdi);
		goto out;
	}


	/* convert disk inode to memory inode */
	inode->i_mode = le16_to_cpu(tdi->i_mode);
	i_uid_write(inode, le32_to_cpu(tdi->i_uid));
	i_gid_write(inode, le32_to_cpu(tdi->i_gid));
	set_nlink(inode, le16_to_cpu(tdi->i_links_count));

	inode->i_size = le32_to_cpu(tdi->i_size);
	inode->i_blocks = le32_to_cpu(tdi->i_blocks);

	inode->i_atime.tv_sec = (signed)le32_to_cpu(tdi->i_atime);
	inode->i_ctime.tv_sec = (signed)le32_to_cpu(tdi->i_ctime);
	inode->i_mtime.tv_sec = (signed)le32_to_cpu(tdi->i_mtime);
	inode->i_atime.tv_nsec = 0;
	inode->i_mtime.tv_nsec = 0;
	inode->i_ctime.tv_nsec = 0;

	inode->i_generation = le32_to_cpu(tdi->i_generation);

	/* operations */
	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &testfs_file_iops;
		inode->i_fop = &testfs_file_fops;
		inode->i_mapping->a_ops = &testfs_aops;
	}  else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &testfs_dir_iops;
		inode->i_fop = &testfs_dir_fops;
		inode->i_mapping->a_ops = &testfs_aops;
	} else {
		pr_err("wrong mode, %x\n", inode->i_mode);
		goto out;
	}

	brelse (bh);
	unlock_new_inode(inode);
        return inode;

out:
	brelse(bh);
	iget_failed(inode);
	return ERR_PTR(err);
}
