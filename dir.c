#include "testfs.h"

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
		log_err("max name lenght:%d\n", TESTFS_FILE_NAME_LEN);
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
	entry[i].file_type = fs_umode_to_ftype(inode->i_mode);
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

/* find @dentry->d_name.name from @dir's data block */
static int testfs_name_to_ino(struct inode *dir, struct dentry *dentry, ino_t *ino)
{
	struct testfs_dir_entry *tde;
	struct page *page;
	unsigned long i, total_pages = dir_pages(dir);
	unsigned j, name_len = dentry->d_name.len;

	if (name_len > TESTFS_FILE_NAME_LEN)
		return -ENAMETOOLONG;

	for (i = 0; i < total_pages; i++) {
		page = testfs_get_page(dir, i);
		if (IS_ERR(page))
			return -EIO;
		tde = (struct testfs_dir_entry *)page_address(page);
		for (j = 0; j < TEST_FS_DENTRY_PER_PAGE; j++) {
			if (tde[j].name_len != name_len)
				continue;

			if (!strncmp(dentry->d_name.name, tde[j].name,name_len)) {
				*ino = le32_to_cpu(tde[j].inode);
				testfs_put_page(page);
				return 0;
			}
		}
		testfs_put_page(page);
	}

	return -ENOENT;
}

static struct dentry *testfs_lookup(struct inode *dir, struct dentry *dentry,
				unsigned int flags)
{
	struct inode * inode;
	ino_t ino;
	int res;

	res = testfs_name_to_ino(dir, dentry, &ino);
	if (res) {
		if (res != -ENOENT)
			return ERR_PTR(res);
		inode = NULL;
	} else {
		inode = testfs_iget(dir->i_sb, ino);
		if (inode == ERR_PTR(-ESTALE)) {
			log_err("deleted inode referenced: %lu", (unsigned long) ino);
			return ERR_PTR(-EIO);
		}
	}

	return d_splice_alias(inode, dentry);
}

const struct inode_operations testfs_dir_iops = {
	.lookup		= testfs_lookup,
	.create		= testfs_create,
	.getattr        = testfs_getattr,
};

static int testfs_readdir(struct file *file, struct dir_context *ctx)
{
	loff_t pos = ctx->pos;
	struct inode *inode = file_inode(file);
	struct testfs_dir_entry *tde;
	unsigned int offset = pos & ~PAGE_MASK;
	unsigned long i = pos >> PAGE_SHIFT;
	unsigned long total_pages = dir_pages(inode);
	struct page *page;
	char *s, *e;
#if 0
	/* need support revalidate */
	bool need_revalidate = !inode_eq_iversion(inode, file->f_version);
#endif

	if (pos > inode->i_size - TEST_FS_DENTRY_SIZE)
		return 0;

	for (; i < total_pages; i++, offset = 0) {
		page = testfs_get_page(inode, i);
		if (IS_ERR(page)) {
			log_err("bad page in inode %lu, skip\n", inode->i_ino);
			ctx->pos += PAGE_SIZE - offset;
			return PTR_ERR(page);
		}

		s = (char *)page_address(page);
		e = s + PAGE_SIZE;
		s += offset;

		for (;s < e; s += TEST_FS_DENTRY_SIZE) {
			tde = (struct testfs_dir_entry *)s;
			if (tde->name_len == 0)
				continue;
			/* copy valid dentry to @ctx */
			if (!dir_emit(ctx, tde->name, tde->name_len,
					le32_to_cpu(tde->inode),
					fs_ftype_to_dtype(tde->file_type))) {
				testfs_put_page(page);
				return 0;
			}

			ctx->pos += TEST_FS_DENTRY_SIZE;
		}
		testfs_put_page(page);
	}

	return 0;
}

const struct file_operations testfs_dir_fops = {
        .llseek         = generic_file_llseek,
        .read           = generic_read_dir,
        .unlocked_ioctl = testfs_ioctl,
#ifdef CONFIG_COMPAT
        .compat_ioctl   = testfs_compat_ioctl,
#endif
	.fsync		= testfs_fsync,
	.iterate_shared = testfs_readdir,
};
