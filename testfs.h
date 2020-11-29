#ifndef __TESTFS_H__
#define __TESTFS_H__
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
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <asm/unaligned.h>
#include <linux/proc_fs.h>
#include <linux/iversion.h>
#include <linux/writeback.h>


#define log_err(fmt,...) pr_err("[%s, %d] "fmt, __func__, __LINE__, ## __VA_ARGS__)
/**************************************************************
 * inode
 **************************************************************/

#define TESTFS_ROOT_INO		0
#define TESTFS_DISK_INODE_SIZE	128
/* simplify the inode strcture, only support 16 blocks in a file */
#define TEST_FS_N_BLOCKS	16

struct testfs_inode {
	struct inode vfs_inode;
	/*
	 * use to record the mapping between disk->lba and file's offset,
	 * to avoid read/write disk every time, we only record/update the mapping
	 * int memory, until it was write back to the underline disk.
	 */
	__le32 i_block[TEST_FS_N_BLOCKS];/* Pointers to blocks */
	int is_new_inode;
};

struct testfs_disk_inode {
	__le16 i_mode;		/* File mode */
	__le16 i_links_count;	/* Links count */
	__le32 i_uid;		/* Low 16 bits of User Uid */
	__le32 i_gid;		/* Low 16 bits of Group Id */
/*16 */	__le32 i_size;		/* Size in bytes */
	__le32 i_atime;		/* Access time */
	__le32 i_ctime;		/* Creation time */
	__le32 i_mtime;		/* Modification time */
/*32 */	__le32 i_generation;	/* ??? */
	__le32 i_flags;		/* File flags */
/*40 */	__le32 i_blocks;	/* Blocks count */
/*104*/	__le32 i_block[TEST_FS_N_BLOCKS];/* Pointers to blocks */
	__u8   reserved[24];
};

#define TESTFS_I(inode) container_of(inode, struct testfs_inode, vfs_inode)


/**************************************************************
 * super block
 **************************************************************/
struct inode *testfs_alloc_inode(struct super_block *sb);
#define TEST_FS_MAGIC		0x1234
#define TEST_FS_BLOCK_SIZE	4096

/* block index */
#define TEST_FS_BLKID_SB	0	/* super block */
#define TEST_FS_BLKID_IBITMAP	1	/* inode bitmap */
#define TEST_FS_BLKID_DBITMAP	2	/* data block bitmap */
#define TEST_FS_BLKID_ITABLE	3	/* inode table */

#define TEST_FS_FILE_MAX_BYTE	(TEST_FS_BLOCK_SIZE * TEST_FS_N_BLOCKS)

struct test_super_block {
	__le32 s_version;
	__le32 s_block_size;		/* block size (byte) */
	__le32 s_inode_size;		/* disk inode size (byte) */
	__le32 s_total_blknr;		/* total blocks include meta */

	/* inode table */
	__le32 s_inode_table_blknr;	/* inode table block count */

	/* data blocks */
	__le32 s_data_blkid;		/* data block index */
	__le32 s_data_blknr;		/* data block count */

	__u8   s_uuid[16];             /* 128-bit uuid */

	__le16 s_magic;

	/* reserved field */
	__le32 s_reserved[];

};
struct testfs_sb_info {
	struct buffer_head *s_sb_bh;
	struct test_super_block *s_tsb;

	int inode_table_blknr;
	u32 s_block_size;
	u32 s_inode_size;

	spinlock_t s_inode_gen_lock;
	u32 s_inode_gen;
};

/**************************************************************
 * inode
 **************************************************************/

#define TEST_FS_DENTRY_SIZE	64
#define TEST_FS_DENTRY_PER_PAGE	(PAGE_SIZE / TEST_FS_DENTRY_SIZE)
/*
 * To simplify entry alloc/release/lookup, use fix name length,
 * and align to page, even some space wasted.
 *
 * Total 64: 4 + 1 + 1 + 58
 */
struct testfs_dir_entry {
#define TESTFS_FILE_NAME_LEN 58
	__le32 inode;
	__u8 file_type;
	__u8 name_len;	/* 0 means free slot */
	__u8 name[TESTFS_FILE_NAME_LEN];
};


int testfs_fill_super(struct super_block *sb, void *data, int silent);
int testfs_get_block_and_offset(struct super_block *sb, ino_t ino,
				unsigned long *blkid, unsigned long *offset);
int testfs_get_block(struct inode *inode, sector_t iblock,
                struct buffer_head *bh_result, int create);
struct inode *testfs_new_inode(struct inode *dir, umode_t mode,
				const struct qstr *qstr);
int testfs_inode_cache_init(void);
void testfs_inode_cache_deinit(void);
void testfs_free_inode(struct inode *inode);
struct inode *testfs_iget(struct super_block *sb, int ino);
int testfs_write_inode(struct inode *inode, struct writeback_control *wbc);
int testfs_fsync(struct file *file, loff_t start, loff_t end, int datasync);
long testfs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
#ifdef CONFIG_COMPAT
long testfs_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
#endif
int testfs_getattr(const struct path *path, struct kstat *stat,
                unsigned int request_mask, unsigned int query_flags);

extern const struct inode_operations testfs_file_iops;
extern const struct file_operations testfs_file_fops;
extern const struct inode_operations testfs_dir_iops;
extern const struct file_operations testfs_dir_fops;
#endif /* __TESTFS_H__ */
