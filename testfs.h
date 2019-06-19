#ifndef __TESTFS_H__
#define __TESTFS_H__

/**************************************************************
 * inode
 **************************************************************/

#define TESTFS_ROOT_INO		1
struct testfs_inode {
	struct inode vfs_inode;
	void *priv;
};

#define TESTFS_DISK_INODE_SIZE	128

/* simplify the inode strcture, only support 16 blocks in a file */
#define TEST_FS_N_BLOCKS	16

struct testfs_disk_inode {
	__le16 i_mode;		/* File mode */
	__le16 i_links_count;	/* Links count */
	__le32 i_uid;		/* Low 16 bits of User Uid */
	__le32 i_gid;		/* Low 16 bits of Group Id */
/*16*/	__le32 i_size;		/* Size in bytes */
	__le32 i_atime;		/* Access time */
	__le32 i_ctime;		/* Creation time */
	__le32 i_mtime;		/* Modification time */
/*32*/	__le32 i_generation;	/* ??? */
	__le32 i_flags;		/* File flags */
	__le32 i_blocks;	/* Blocks count */
/*44*/	__le32 i_block[TEST_FS_N_BLOCKS];/* Pointers to blocks */
	__u8   reserved[88];
};

#define TESTFS_I(inode) container_of(inode, struct testfs_inode, vfs_inode)


/**************************************************************
 * super block
 **************************************************************/
struct inode *testfs_alloc_inode(struct super_block *sb);
#define TEST_FS_MAGIC		0x11223344
#define TEST_FS_BLOCK_SIZE	4096

/* block index */
#define TEST_FS_BLKID_SB	0	/* super block */
#define TEST_FS_BLKID_IBITMAP	1	/* inode bitmap */
#define TEST_FS_BLKID_DBITMAP	2	/* data block bitmap */
#define TEST_FS_BLKID_ITABLE	3	/* inode table */

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
};

int testfs_fill_super(struct super_block *sb, void *data, int silent);
int testfs_get_block_and_offset(struct super_block *sb, ino_t ino,
				unsigned long *blkid, unsigned long *offset);
int testfs_inode_cache_init(void);
void testfs_inode_cache_deinit(void);
void testfs_free_inode(struct inode *inode);
struct inode *testfs_iget(struct super_block *sb, int ino);
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
