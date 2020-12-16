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
#include <linux/random.h>

#include "testfs.h"

/*
 * get the block index which contains this inode, and the offset
 * within that block
 */
int testfs_get_block_and_offset(struct super_block *sb, ino_t ino,
				unsigned long *blkid, unsigned long *offset)
{
	struct testfs_sb_info *sbi = (struct testfs_sb_info *)sb->s_fs_info;
	int inode_per_block;

	/*
	 * since inode bitmap only use one block, so the max inode index should
	 * less that it.
	 */
	if (ino >= TEST_FS_BLOCK_SIZE) {
		log_err("ino (%ld) is too large, expect < %d\n",
				ino, TEST_FS_BLOCK_SIZE);
		return -EINVAL;
	}

	/* inode count per block */
	inode_per_block = sbi->s_block_size / sbi->s_inode_size;

	/* get the block index */
	*blkid = ino / inode_per_block + TEST_FS_BLKID_ITABLE;

	/* get offset within block */
	*offset = (ino % inode_per_block) * sbi->s_inode_size;

	return 0;
}

static void testfs_put_super(struct super_block *sb)
{
	struct testfs_sb_info *sbi = (struct testfs_sb_info *)sb->s_fs_info;

	log_err("\n");

	brelse(sbi->s_sb_bh);
	kfree(sb->s_fs_info);
	sb->s_fs_info = NULL;
}

struct super_operations testfs_sops = {
	.free_inode = testfs_free_inode,
	.alloc_inode = testfs_alloc_inode,
	.write_inode = testfs_write_inode,
	.evict_inode = testfs_evict_inode,
	.put_super = testfs_put_super,
};

int testfs_fill_super(struct super_block *sb, void *data, int silent)
{
	int ret, block_size, inode_size, total_blknr;
	struct testfs_sb_info *sbi;
	struct inode *root;
	struct buffer_head * bh;
	struct test_super_block *tsb;

	log_err("\n");

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	/* set block size for superblock */
	block_size = sb_min_blocksize(sb, TEST_FS_BLOCK_SIZE);
	if (!block_size) {
		log_err("failed to set block size (%d) for super block\n",
				TEST_FS_BLOCK_SIZE);
		goto free_sbi;
	}
	sbi->s_block_size = block_size;

	/* read super block from disk, the offset is 0 */
	bh = sb_bread_unmovable(sb, TEST_FS_BLKID_SB);
	if (!bh) {
		log_err("failed to read superblock from disk\n");
		goto free_sbi;
	}
	sbi->s_sb_bh = bh;
	tsb = (struct test_super_block *)bh->b_data;
	sbi->s_tsb = tsb;

	/* convert them to struct super_block */
	sb->s_magic = le16_to_cpu(tsb->s_magic);
	if (sb->s_magic != TEST_FS_MAGIC) {
		log_err("Wrong magic number %lx != %x\n",
				sb->s_magic, TEST_FS_MAGIC);
		goto free_bh;
	}

	/* verify block size */
	block_size = le32_to_cpu(tsb->s_block_size);
	if (block_size != sbi->s_block_size) {
		log_err("wrong block size %d, expect %d\n", block_size,
				sbi->s_block_size);
		goto free_bh;
	}
	sbi->s_block_size = block_size;

	/* verify inode size */
	inode_size = le32_to_cpu(tsb->s_inode_size);
	if (inode_size != TESTFS_DISK_INODE_SIZE) {
		log_err("wrong block size %d, expect %d\n", block_size,
				TESTFS_DISK_INODE_SIZE);
		goto free_bh;
	}
	sbi->s_inode_size = inode_size;

	/* the maximum file size */
	sb->s_maxbytes = TEST_FS_N_BLOCKS * block_size;

	ret = generic_check_addressable(sb->s_blocksize_bits,
							tsb->s_total_blknr);
	if (ret) {
		log_err("filesystem is too large to mount\n");
		goto free_bh;
	}

	/* verify filesystem size and block device size */
	total_blknr = sb->s_bdev->bd_inode->i_size >> sb->s_blocksize_bits;
	if (total_blknr < tsb->s_total_blknr) {
		log_err("filesystem size (%d) > disk size(%d), please re-format\n",
			tsb->s_total_blknr, total_blknr);
		goto free_bh;
	}

	/* basic initialization */
	spin_lock_init(&sbi->s_inode_gen_lock);
	get_random_bytes(&sbi->s_inode_gen, sizeof(u32));
	sbi->s_data_blkid = le32_to_cpu(tsb->s_data_blkid);

	ret = -ENOMEM;

	sb->s_magic = TEST_FS_MAGIC;
	sb->s_fs_info = sbi;
	sb->s_op = &testfs_sops;

	/* copy uuid */
	memcpy(&sb->s_uuid, tsb->s_uuid, sizeof(sb->s_uuid));

	root = testfs_iget(sb, TESTFS_ROOT_INO);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto free_sbi;
	}

	if (!S_ISDIR(root->i_mode)) {
		log_err("read root inode failed\n");
		goto free_inode;
	}

	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		log_err("d make root failed\n");
		goto free_inode;
	}

	return 0;

free_inode:
	iput(root);
free_bh:
	brelse(sbi->s_sb_bh);
free_sbi:
	kfree(sbi);
	return ret;
}
