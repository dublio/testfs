#include <linux/aer.h>
#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/hdreg.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/poison.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <asm/unaligned.h>
#include <linux/proc_fs.h>

#define TEST_FS_MAGIC 0x11223344

struct testfs_sb_info {
	int foo;
};


static void testfs_put_super(struct super_block *sb)
{
	pr_err("%s,%d\n", __func__, __LINE__);

	kfree(sb->s_fs_info);
	sb->s_fs_info = NULL;
}

struct super_operations testfs_sops = {
	.put_super = testfs_put_super,
};

static inline int testfs_fill_super(struct super_block *sb, void *data, int silent)
{
	int ret;
	struct testfs_sb_info *sbi;
	struct inode *root_inode;

	pr_err("%s,%d\n", __func__, __LINE__);

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	ret = -ENOMEM;

	sb->s_magic = TEST_FS_MAGIC;
	sb->s_fs_info = sbi;
	sb->s_op = &testfs_sops;

	root_inode = new_inode(sb);
	if (!root_inode)
		goto free_sbi;

	root_inode->i_atime = root_inode->i_mtime = root_inode->i_ctime = current_time(root_inode);
	root_inode->i_sb = sb;
	inode_init_owner(root_inode, NULL, S_IFDIR);

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		pr_err("d make root failed\n");
		goto free_inode;
	}

	return 0;

free_inode:
	iput(root_inode);
free_sbi:
	kfree(sbi);
	return ret;
}

static struct dentry *testfs_mount(struct file_system_type *fs_type, int flags,
		       const char *dev_name, void *data)
{
	pr_err("%s,%d\n", __func__, __LINE__);
	return mount_bdev(fs_type, flags, dev_name, data, testfs_fill_super);
}

static void testfs_kill_sb(struct super_block *sb)
{
	pr_err("%s,%d\n", __func__, __LINE__);
	kill_block_super(sb);
}

struct file_system_type test_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "testfs",
	.mount		= testfs_mount,
	.kill_sb	= testfs_kill_sb,
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init testfs_init(void) {
	int ret;

	pr_err("%s,%d\n", __func__, __LINE__);
	ret = register_filesystem(&test_fs_type);
	if (ret) {
		pr_err("failed to register testfs\n");
		return -EINVAL;
	}

	return 0;
}

static void __exit testfs_exit(void) {
	pr_err("%s,%d\n", __func__, __LINE__);
	unregister_filesystem(&test_fs_type);
}

MODULE_LICENSE("GPL v3");
MODULE_AUTHOR("Weiping Zhang <zwp10758@gmail.com>");
module_init(testfs_init);
module_exit(testfs_exit);
