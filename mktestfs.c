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
#define _GUN_SOURCE
#include <stdlib.h>
#include <sched.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <stdint.h>
#include <uuid/uuid.h>
#include <endian.h>

#define __le16 uint16_t
#define __le32 uint32_t
#define __u8 uint8_t

#define TESTFS_ROOT_INO		0
#define TESTFS_DISK_INODE_SIZE	128

/* simplify the inode strcture, only support 16 blocks in a file */
#define TEST_FS_N_BLOCKS	16

#define TEST_FS_V1		0x00010000
#define TEST_FS_MAGIC		0x1234
#define TEST_FS_BLOCK_SIZE	4096

/* block index */
#define TEST_FS_BLKID_SB	0	/* super block */
#define TEST_FS_BLKID_IBITMAP	1	/* inode bitmap */
#define TEST_FS_BLKID_DBITMAP	2	/* data block bitmap */
#define TEST_FS_BLKID_ITABLE	3	/* inode table */

#define TEST_FS_FILE_MAX_BYTE	(TEST_FS_BLOCK_SIZE * TEST_FS_N_BLOCKS)

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

const char *g_disk;
char g_tsb_buf[TEST_FS_BLOCK_SIZE];
struct test_super_block *g_tsb = (struct test_super_block *)g_tsb_buf;

char g_inode_bitmap[TEST_FS_BLOCK_SIZE];
char g_data_bitmap[TEST_FS_BLOCK_SIZE];

struct testfs_disk_inode g_root_inode;

static void *zmalloc(size_t size)
{
	void *buf = malloc(size);

	if (!buf)
		return NULL;

	memset(buf, 0, size);
	return buf;
}

static void usage(void)
{
	fprintf(stderr, "Please give a disk name, linke ./mktestfs /dev/sdb1\n");

	_exit(0);
}

static int testfs_write_super_block(int fd, size_t size, struct test_super_block *tsb)
{
	uuid_t uuid;
	uint32_t inode_per_block, inode_block_nr, index;
	size_t len = TEST_FS_BLOCK_SIZE, ret;

	/* format super block base on image size */
	tsb->s_version = htole32(TEST_FS_V1);
	tsb->s_block_size = htole32(TEST_FS_BLOCK_SIZE);
	tsb->s_inode_size = htole32(TESTFS_DISK_INODE_SIZE);
	tsb->s_total_blknr = htole32(size / TEST_FS_BLOCK_SIZE);

	/* super_block + inode_bitmap + data_bitmap */
	index = 3;

	/* inode block count */
	inode_per_block = TEST_FS_BLOCK_SIZE / TESTFS_DISK_INODE_SIZE;
	inode_block_nr = TEST_FS_BLOCK_SIZE / inode_per_block;
	tsb->s_inode_table_blknr = htole32(inode_block_nr);

	index += inode_block_nr;
	tsb->s_data_blkid = htole32(index);
	tsb->s_data_blknr = htole32(tsb->s_total_blknr - index);

	/* uuid */
	uuid_generate(uuid);
	memcpy(&tsb->s_uuid[0], &uuid, sizeof(tsb->s_uuid));

	/* magic */
	tsb->s_magic = htole32(TEST_FS_MAGIC);

	ret = write(fd, (char *)tsb, len);
	if (ret != len) {
		fprintf(stderr, "failed to write super block, %lu != %lu\n",
			ret, len);
		return -1;
	}

	return 0;
}

static int testfs_write_inode_bitmap(int fd, char *buf, size_t len)
{
	size_t ret;

	/* mark first bit to 1, used to root inode */
	buf[0] = 1;

	ret = write(fd, buf, len);
	if (ret != len) {
		fprintf(stderr, "failed to write inode bitmap, %lu != %lu\n",
			ret, len);
		return -1;
	}

	return 0;
}

static int testfs_write_data_bitmap(int fd, char *buf, size_t len)
{
	size_t ret;

	/* mark first bit to 1, used to root inode */
	buf[0] = 1;

	ret = write(fd, buf, len);
	if (ret != len) {
		fprintf(stderr, "failed to write data bitmap, %lu != %lu\n",
			ret, len);
		return -1;
	}

	return 0;
}

static int testfs_write_root_inode(int fd, struct test_super_block *tsb)
{
	struct testfs_disk_inode *tdi = &g_root_inode;
	size_t ret, len = sizeof(*tdi);

	/* init root inode */
	tdi->i_mode = htole16(S_IFDIR | S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	tdi->i_links_count = htole16(1);
	tdi->i_uid = htole32(0);
	tdi->i_gid = htole32(0);
#if 0
	/* no files in root inode */
	tdi->i_size = sizeof(struct testfs_dir_entry);
#endif
	tdi->i_size = htole32(0);

	tdi->i_blocks = htole32(0);

	ret = write(fd, tdi, len);
	if (ret != len) {
		fprintf(stderr, "failed to write inode bitmap, %lu != %lu\n",
			ret, len);
		return -1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int fd;
	size_t size;
	struct stat st;
	struct test_super_block *tsb;
	char *imap_buf;

	if (argc != 2)
		usage();

	g_disk = argv[1];

	/*
	 * Disk layout
	 * |--------|--------|--------|--------|--------------------|
	 *     1        2        3        4             5
	 *
	 * index | count | usage
	 * ------------------------------------
	 * 1     | 1     | super block
	 * 2     | 1     | inode bitmap
	 * 3     | 1     | data block bitmap
	 * 4     | N     | inode table
	 * 5     | M     | data region
	 *
	 */

	fd = open(g_disk, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "failed to open: %s\n", g_disk);
		return -1;
	}

	if (fstat(fd, &st)) {
		fprintf(stderr, "failed to stat: %s\n", g_disk);
		goto close;
	}

	size = st.st_size;
	if (size % TEST_FS_BLOCK_SIZE) {
		fprintf(stderr, "%lu size is not align to %lu\n",
					size, TEST_FS_BLOCK_SIZE);
		goto close;
	}

	printf("start make filesystem for:  %s\n", g_disk);
	printf("\tsizeof(test_super_block):   %lu\n", sizeof(struct test_super_block));
	printf("\tsizeof(testfs_disk_inode):  %lu\n", sizeof(struct testfs_disk_inode));
	printf("\tsizeof(testfs_dir_entry):   %lu\n", sizeof(struct testfs_dir_entry));
	printf("\tblock size:    %lu\n", TEST_FS_BLOCK_SIZE);
	printf("\ttotal blocks:  %lu\n", size / TEST_FS_BLOCK_SIZE);

	/* super block */
	if (testfs_write_super_block(fd, size, g_tsb)) {
		fprintf(stderr, "failed to write super block\n");
		goto close;
	}
	printf("write super block done\n");

	/* inode bitmap */
	if (testfs_write_inode_bitmap(fd, g_inode_bitmap, TEST_FS_BLOCK_SIZE)) {
		fprintf(stderr, "failed to write inode bitmap\n");
		goto close;
	}
	printf("write inode bitmap done\n");

	/* data bitmap */
	if (testfs_write_data_bitmap(fd, g_data_bitmap, TEST_FS_BLOCK_SIZE)) {
		fprintf(stderr, "failed to write data bitmap\n");
		goto close;
	}
	printf("write data bitmap done\n");

	/* inode table: root inode */
	if (testfs_write_root_inode(fd, g_tsb)) {
		fprintf(stderr, "failed to write root inode\n");
		goto close;
	}
	printf("write root inode done\n");
	printf("finished to make filesystem for:  %s\n", g_disk);

	close(fd);

	return 0;
free_tsb:
	free(tsb);
close:
	close(fd);
	return -1;
}
