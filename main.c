/*
 * testfs - A simple block device based file system
 *
 * The license below covers all files distributed with testfs unless otherwise
 * noted in the file itself.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <https://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2020 Weiping Zhang <zwp10758@gmail.com>
 *
 */
#include "testfs.h"

static struct dentry *testfs_mount(struct file_system_type *fs_type, int flags,
		       const char *dev_name, void *data)
{
	log_err("\n");
	return mount_bdev(fs_type, flags, dev_name, data, testfs_fill_super);
}

static void testfs_kill_sb(struct super_block *sb)
{
	log_err("\n");
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

	log_err("\n");
	ret = testfs_inode_cache_init();
	if (ret) {
		log_err("failed to init testfs icache\n");
		return -ENOMEM;
	}

	ret = register_filesystem(&test_fs_type);
	if (ret) {
		log_err("failed to register testfs\n");
		goto deinit_icache;
	}

	return 0;

deinit_icache:
	testfs_inode_cache_deinit();
	return ret;
}

static void __exit testfs_exit(void) {
	log_err("\n");
	testfs_inode_cache_deinit();
	unregister_filesystem(&test_fs_type);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Weiping Zhang <zwp10758@gmail.com>");
module_init(testfs_init);
module_exit(testfs_exit);
