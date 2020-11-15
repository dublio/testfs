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

/*
 * To simplify entry alloc/release, use fix name length, even some space wasted
 *
 * Total 64: 4 + 1 + 59
 */
struct testfs_dir_entry {
#define TESTFS_FILE_NAME_LEN 59
	__le32 inode;
	__u8 name_len;	/* 0 means free slot */
	__u8 name[TESTFS_FILE_NAME_LEN];
};

static struct page *testfs_get_page(struct inode *inode, unsigned long n)
{
        struct address_space *mapping = inode->i_mapping;
        struct page *page = read_mapping_page(mapping, n, NULL);

        if (IS_ERR(page))
		goto err;

	if (unlikely(PageError(page)))
		goto put;

	kmap(page);

        return page;

put:
	put_page(page);
err:
        return ERR_PTR(-EIO);
}

static void testfs_put_page(struct page *page)
{
	kunmap(page);
	put_page(page);
}

static int testfs_commit_chunk(struct page *page, loff_t pos, unsigned len)
{
        struct address_space *mapping = page->mapping;
        struct inode *dir = mapping->host;
        int err = 0;

        inode_inc_iversion(dir);
        block_write_end(NULL, mapping, pos, len, len, page, NULL);

        if (pos + len > dir->i_size) {
                i_size_write(dir, pos + len);
                mark_inode_dirty(dir);
        }

        if (IS_DIRSYNC(dir)) {
                err = write_one_page(page);
                if (!err)
                        err = sync_inode_metadata(dir, 1);
        } else {
                unlock_page(page);
        }

        return err;
}

static int testfs_add_link(struct dentry *dentry, struct inode *inode)
{
	struct inode *dir = d_inode(dentry->d_parent);
	struct page *page;
	struct testfs_dir_entry *entry;
	const char *name = dentry->d_name.name;
	int err, i, entry_per_page, namelen = dentry->d_name.len;
	unsigned long n, npages = dir_pages(dir);
	loff_t pos = 0, entry_size = sizeof(struct testfs_dir_entry);

	if (namelen > TESTFS_FILE_NAME_LEN) {
		pr_err("max name lenght:%d\n", TESTFS_FILE_NAME_LEN);
		return -EINVAL;
	}

	/* Now: page_size(4096) is multiple entry_size(64) */
	entry_per_page = PAGE_SIZE / entry_size;

	/*
	 * n <= npages; means we can alloc a new page if not free slot found
	 * in existing pages.
	 */
	for (n = 0; n <= npages; n++) {
		page = testfs_get_page(dir, n);
		if (IS_ERR(page))
			return PTR_ERR(page);

		lock_page(page);

		/* check if exist or find a free slot to store this inode. */
		entry = (struct testfs_dir_entry *)page_address(page);
		for (i = 0; i < entry_per_page; i++) {
			if (pos == inode->i_size)
				goto got;
			if (entry[i].name_len > 0) {
				if (namelen != entry[i].name_len)
					continue;
				/* already exist */
				if (!memcmp(name, entry[i].name, namelen)) {
					unlock_page(page);
					testfs_put_page(page);
					return -EEXIST;
				}
			} else
				goto got;
			pos +=  entry_size;
		}
		unlock_page(page);
	}
	BUG();
	return -EINVAL;
got:
	if (__block_write_begin(page, pos, entry_size, testfs_get_block)) {
		unlock_page(page);
		testfs_put_page(page);
		return -EIO;
	}
	/* find a free slot */
	entry[i].inode = cpu_to_le32(inode->i_ino);
	entry[i].name_len = namelen;
	memcpy(entry[i].name, name, namelen);

	err = testfs_commit_chunk(page, pos, entry_size);

	dir->i_mtime = dir->i_ctime = current_time(dir);
        mark_inode_dirty(dir);

	kunmap(page);
	put_page(page);
	return err;
}


static int testfs_add_inode_to_dir(struct dentry *dentry, struct inode *inode)
{
        int err = testfs_add_link(dentry, inode);
        if (!err) {
                d_instantiate_new(dentry, inode);
                return 0;
        }
        inode_dec_link_count(inode);
        discard_new_inode(inode);
        return err;
}

static int testfs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
        struct inode *inode;

        inode = testfs_new_inode(dir, mode, &dentry->d_name);
        if (IS_ERR(inode))
                return PTR_ERR(inode);

        mark_inode_dirty(inode);
        return testfs_add_inode_to_dir(dentry, inode);
}

const struct inode_operations testfs_dir_iops = {
	.create		= testfs_create,
	.getattr        = testfs_getattr,
};
