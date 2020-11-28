#include "testfs.h"

static struct kmem_cache *testfs_icachep;

static void init_once(void *foo)
{
	struct testfs_inode *ti = (struct testfs_inode *)foo;

	inode_init_once(&ti->vfs_inode);
}

int testfs_inode_cache_init(void)
{
	log_err("\n");

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

	log_err("\n");

	ti = kmem_cache_alloc(testfs_icachep, GFP_KERNEL);
	if (!ti)
		return NULL;

	return &ti->vfs_inode;
}

void testfs_free_inode(struct inode *inode)
{
	struct testfs_inode *ti = TESTFS_I(inode);

	log_err("\n");

	kmem_cache_free(testfs_icachep, ti);
}

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
	if (testfs_get_block_and_offset(sb, ino, &blkid, &offset))
		return ERR_PTR(-EINVAL);

	tmp = sb_bread(sb, blkid);
	if (!tmp)
		return ERR_PTR(-EIO);

	*bh = tmp;

	return (struct testfs_disk_inode *)(tmp->b_data + offset);
}

static int testfs_get_new_block(struct super_block *sb, u32 *blkid)
{
	struct buffer_head *bh;
	unsigned long *bitmap, index = 0;
	bool got = false;

	/* read data bitmap */
	bh = sb_bread_unmovable(sb, TEST_FS_BLKID_DBITMAP);
	if (!bh) {
		log_err("failed to read data bitmap\n");
		return -EIO;
	}

	bitmap = (unsigned long *)bh->b_data;

	while (1) {
		/* find the first available bit */
		index = find_first_zero_bit_le(bitmap, TEST_FS_BLOCK_SIZE);

		/* if return 0, means this bit has been used, retry other bit */
		if (!test_and_set_bit_le(index, bitmap)) {
			*blkid = (u32)index;
			got = true;
			break;
		}
	}

	if (!got) {
		log_err("not found available data block\n");
		goto free_bh;
	}


	/* update data bitmap */
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;

free_bh:
	brelse(bh);
	return -ENOSPC;
}

/*
 * testfs_get_blocks - alloc/read block from disk for this inode
 * @inode:	the inode interested
 * @iblock:	offset within @inode
 * @bno:	block index on disk
 * @new:	new allocated ? maybe it has been in disk
 * @boundary:	??
 * @create:	create new block if no
 *
 */
static int _testfs_get_block(struct inode *inode, sector_t iblock, u32 *bno,
			bool *new, int create)
{
	struct buffer_head *disk_inode_bh;
	struct super_block *sb = inode->i_sb;
	struct testfs_disk_inode *tdi;
	unsigned old_blkid;
	int ret = -ENOSPC;

	/*
	 * how many blocked has been allocated ?, to simplify the code logic
	 * we only allocate maximun 16 blocks for a file.
	 */
	if (inode->i_size >= TEST_FS_FILE_MAX_BYTE) {
		log_err("file size limitation\n");
		return -ENOSPC;
	}

	/* read inode info from disk for this ino */
	tdi = testfs_get_disk_inode(sb, inode->i_ino, &disk_inode_bh);
	if (IS_ERR(tdi))
		return -EIO;

	/* check allocated ? */
	old_blkid = le32_to_cpu(tdi->i_block[iblock]);
	if (old_blkid > 0) {
		*bno = old_blkid;
		ret = 0;
		goto out;
	}

	if (create == 0)
		goto out;

	/* alloc new data block */
	ret = testfs_get_new_block(sb, bno);
	if (ret)
		goto out;

	*new = true;

	/* update inode dentry */
	tdi->i_blocks = cpu_to_le32(le32_to_cpu(tdi->i_blocks) + 1);
	tdi->i_block[iblock] = cpu_to_le32(*bno);
	mark_buffer_dirty(disk_inode_bh);
	sync_dirty_buffer(disk_inode_bh);

out:
	brelse(disk_inode_bh);
	return ret;
}

int testfs_get_block(struct inode *inode, sector_t iblock,
                struct buffer_head *bh_result, int create)
{
        bool new = false;
        u32 bno;
        int ret;

        ret = _testfs_get_block(inode, iblock, &bno, &new, create);
        if (ret)
                return ret;

        map_bh(bh_result, inode->i_sb, bno);
        bh_result->b_size = (1 << inode->i_blkbits);
        if (new)
                set_buffer_new(bh_result);

	log_err("ino:%lu, block_index:%u\n", inode->i_ino, bno);
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
		log_err("wrong mode, %x\n", inode->i_mode);
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

struct inode *testfs_new_inode(struct inode *dir, umode_t mode,
				const struct qstr *qstr)
{
	struct inode *inode;
	struct super_block *sb;
	struct testfs_sb_info *sbi;
	struct buffer_head *bh;
	unsigned long *bitmap;
	ino_t ino;

        sb = dir->i_sb;
	sbi = sb->s_fs_info;
        inode = new_inode(sb);
        if (!inode)
                return ERR_PTR(-ENOMEM);

	/* read inode bitmap from disk */
	bh = sb_bread_unmovable(sb, TEST_FS_BLKID_IBITMAP);
	if (!bh) {
		log_err("failed to read inode bitmap\n");
		goto free_inode;
	}

	bitmap = (unsigned long *)bh->b_data;

	while (1) {
		/* find the first available bit */
		ino = find_first_zero_bit_le(bitmap, TEST_FS_BLOCK_SIZE);

		/* if return 0, means this bit has been used, retry other bit */
		if (!test_and_set_bit_le(ino, bitmap))
			break;
	}
	log_err("ino:%lu\n", ino);

	/* write inode bitmap back to disk */
	mark_buffer_dirty(bh);
	if (sb->s_flags & SB_SYNCHRONOUS)
		sync_dirty_buffer(bh);
	brelse(bh);

	inode_init_owner(inode, dir, mode);
	inode->i_ino = ino;
	inode->i_blocks = 0;
	inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);
	inode->i_op = &testfs_file_iops;
	inode->i_fop = &testfs_file_fops;
	inode->i_mapping->a_ops = &testfs_aops;
	spin_lock(&sbi->s_inode_gen_lock);
	inode->i_generation = sbi->s_inode_gen++;
	spin_unlock(&sbi->s_inode_gen_lock);

	if (insert_inode_locked(inode) < 0) {
		log_err("failed to insert inode: %ld\n", inode->i_ino);
		goto free_inode;
	}

	return inode;

free_inode:
	make_bad_inode(inode);
	iput(inode);
	return ERR_PTR(-EIO);
}
