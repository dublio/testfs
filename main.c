#include "testfs.h"

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

	ret = testfs_inode_cache_init();
	if (ret) {
		pr_err("failed to init testfs icache\n");
		return -ENOMEM;
	}

	ret = register_filesystem(&test_fs_type);
	if (ret) {
		pr_err("failed to register testfs\n");
		goto deinit_icache;
	}

	return 0;

deinit_icache:
	testfs_inode_cache_deinit();
	return ret;
}

static void __exit testfs_exit(void) {
	pr_err("%s,%d\n", __func__, __LINE__);
	testfs_inode_cache_deinit();
	unregister_filesystem(&test_fs_type);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Weiping Zhang <zwp10758@gmail.com>");
module_init(testfs_init);
module_exit(testfs_exit);
