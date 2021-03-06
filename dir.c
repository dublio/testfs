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

static int testfs_prepare_block(struct page *page, loff_t pos, unsigned len)
{
	return __block_write_begin(page, pos, len, testfs_get_block);
}

static int testfs_commit_block(struct page *page, loff_t pos, unsigned len)
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
		for (i = 0; i < entry_per_page; i++, pos += entry_size) {
			if (pos == dir->i_size)
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
		}
		unlock_page(page);
	}
	BUG();
	return -EINVAL;
got:
	if (testfs_prepare_block(page, pos, entry_size)) {
		unlock_page(page);
		testfs_put_page(page);
		return -EIO;
	}
	/* find a free slot */
	entry[i].inode = cpu_to_le32(inode->i_ino);
	entry[i].name_len = namelen;
	entry[i].file_type = fs_umode_to_ftype(inode->i_mode);
	memcpy(entry[i].name, name, namelen);

	err = testfs_commit_block(page, pos, entry_size);

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

/**
 * testfs_lookup_by_name - lookup a file/dir/symlink by name
 *
 * @dir:	the parent direcotry will be searched
 * @dentry:	the dentry->d_name.name will be will be searched
 * @pg:		the page pointer that contains struct testfs_dir_entry
 *
 * Attenton: the caller should unmap and put @pg by call testfs_put_page
 *
 * Return: the pointer of struct testfs_dir_entry* on succes, others on error
 */
static struct testfs_dir_entry *testfs_lookup_by_name(struct inode *dir,
					struct dentry *dentry, struct page **pg)
{
	struct testfs_dir_entry *tde;
	struct page *page;
	unsigned long i, total_pages = dir_pages(dir);
	unsigned j, name_len = dentry->d_name.len;
	loff_t pos = 0, entry_size = sizeof(struct testfs_dir_entry);

	if (name_len > TESTFS_FILE_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	for (i = 0; i < total_pages; i++) {
		page = testfs_get_page(dir, i);
		if (IS_ERR(page))
			return ERR_PTR(-EIO);
		tde = (struct testfs_dir_entry *)page_address(page);
		for (j = 0; j < TEST_FS_DENTRY_PER_PAGE; j++, pos += entry_size) {
			if (pos == dir->i_size) {
				testfs_put_page(page);
				return ERR_PTR(-ENOENT);
			}
			if (tde[j].name_len != name_len)
				continue;

			if (!strncmp(dentry->d_name.name, tde[j].name,name_len)) {
				*pg = page;
				return &tde[j];
			}
		}
		testfs_put_page(page);
	}

	return ERR_PTR(-ENOENT);
}

static int testfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode * inode = d_inode(dentry);
	struct testfs_dir_entry *tde;
	struct page *page;
	char *kaddr;
	int ret;
	loff_t pos;

	tde = testfs_lookup_by_name(dir, dentry, &page);
	if (IS_ERR(tde))
		return PTR_ERR(tde);

	/* page will be unlock when write done ??? */
	lock_page(page);

	/*  prepare write to the disk, get the mapping between page and lba */
	kaddr = page_address(page);
	pos = page_offset(page) + ((char *)tde - kaddr);

	ret = testfs_prepare_block(page, pos, TEST_FS_DENTRY_SIZE);
	if(ret) {
		log_err("prepare block error: parent.ino=%lu, dentry=%s\n",
				dir->i_ino, dentry->d_name.name);
		unlock_page(page);
		goto out;
	}

	/* clear this dentry, and mark it as unused by set name_len to 0 */
	memset(tde, 0, TEST_FS_DENTRY_SIZE);

	ret = testfs_commit_block(page, pos, TEST_FS_DENTRY_SIZE);
	if(ret)
		log_err("write block error: parent.ino=%lu, dentry=%s\n",
				dir->i_ino, dentry->d_name.name);
	else
		inode_dec_link_count(inode);
out:
	testfs_put_page(page);

	return ret;
}

static int testfs_name_to_ino(struct inode *dir, struct dentry *dentry, ino_t *ino)
{
	struct testfs_dir_entry *tde;
	struct page *page;

	tde = testfs_lookup_by_name(dir, dentry, &page);
	if (IS_ERR(tde))
		return PTR_ERR(tde);

	*ino = le32_to_cpu(tde->inode);
	testfs_put_page(page);

	return 0;
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

static int testfs_make_empty_dir(struct inode *parent, struct inode *new_dir)
{
	struct page *page;
	unsigned block_size = new_dir->i_sb->s_blocksize;
	struct testfs_dir_entry *tde;
	int err;
	void *kaddr;

	page = grab_cache_page(new_dir->i_mapping, 0);
	if (!page)
		return -ENOMEM;

	err = testfs_prepare_block(page, 0, block_size);
	if (err) {
		unlock_page(page);
		goto fail;
	}
	kaddr = kmap_atomic(page);
	memset(kaddr, 0, block_size);
	tde = (struct testfs_dir_entry *)kaddr;
	tde->name_len = 1;
	tde->name[0] = '.';
	tde->inode = cpu_to_le32(new_dir->i_ino);
	tde->file_type = fs_umode_to_ftype(new_dir->i_mode);

	tde = (struct testfs_dir_entry *)((char *)kaddr + TEST_FS_DENTRY_SIZE);
	tde->name_len = 2;
	tde->name[0] = '.';
	tde->name[1] = '.';
	tde->inode = cpu_to_le32(parent->i_ino);
	tde->file_type = fs_umode_to_ftype(parent->i_mode);
	kunmap_atomic(kaddr);
	err = testfs_commit_block(page, 0, block_size);
fail:
	put_page(page);
	return err;
}

static int testfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int ret = 0;

	inode_inc_link_count(dir);
	inode = testfs_new_inode(dir, S_IFDIR | mode, &dentry->d_name);
	if (IS_ERR(inode)) {
		ret = PTR_ERR(inode);
		goto dec_dir_link;
	}
	inode_inc_link_count(inode);

	ret = testfs_make_empty_dir(dir, inode);
	if (ret) {
		log_err("failed to create empty dir, parent:%lu, child:%lu\n",
			dir->i_ino, inode->i_ino);
		goto destroy_inode;
	}

	ret = testfs_add_inode_to_dir(dentry, inode);
	if (ret) {
		log_err("failed to add dir, parent:%lu, child:%lu\n",
			dir->i_ino, inode->i_ino);
		goto destroy_inode;
	}

	return 0;

destroy_inode:
        inode_dec_link_count(inode);
        discard_new_inode(inode);
dec_dir_link:
	inode_dec_link_count(dir);
	return ret;
}


/**
 * testfs_dir_empty - check directory is empty
 *
 * @inode: the directory's inode
 *
 * Returns: true on empty, false on not-empty
 *
 */
static inline bool testfs_dir_empty(struct inode *inode)
{
	unsigned long i, total_pages = dir_pages(inode);
	loff_t pos = 0, total_size = inode->i_size;
	struct testfs_dir_entry *tde;
	struct page *page;
	char *s, *e;

	for (; i < total_pages; i++) {
		page = testfs_get_page(inode, i);
		if (IS_ERR(page)) {
			log_err("bad page in inode %lu, skip\n", inode->i_ino);
			return false;
		}

		s = (char *)page_address(page);
		e = s + PAGE_SIZE;

		for (;s < e; s += TEST_FS_DENTRY_SIZE) {
			/* check current pos and total file size */
			if (pos == total_size) {
				testfs_put_page(page);
				return true;
			}

			tde = (struct testfs_dir_entry *)s;
			switch (tde->name_len) {
			case 0:
				break;
			case 1: /* check . */
				if (tde->name[0] == '.')
					break;
				goto not_empty;
			case 2: /* check .. */
				if ((tde->name[0] == '.') && (tde->name[1] == '.'))
					break;
				goto not_empty;
			default:
				goto not_empty;
			}

			pos += TEST_FS_DENTRY_SIZE;
		}
		testfs_put_page(page);
	}

	return true;

not_empty:
	testfs_put_page(page);
	return false;
}

static int testfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int ret = -ENOTEMPTY;

	if (testfs_dir_empty(inode)) {
		ret = testfs_unlink(dir, dentry);
		if (!ret) {
			inode->i_size = 0;
			inode_dec_link_count(inode);
			inode_dec_link_count(dir);
		}
	}

	return ret;
}

static int testfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	loff_t pos = ctx->pos, total_size = inode->i_size;
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
			/* check current pos and total file size */
			if (ctx->pos == total_size) {
				testfs_put_page(page);
				return 0;
			}

			tde = (struct testfs_dir_entry *)s;
			/*
			 * if find a unused entry, skip it, remeber to update
			 * ctx->pos, which means we have read it.
			 */
			if (tde->name_len == 0)
				goto next;
			/* copy valid dentry to @ctx */
			if (!dir_emit(ctx, tde->name, tde->name_len,
					le32_to_cpu(tde->inode),
					fs_ftype_to_dtype(tde->file_type))) {
				testfs_put_page(page);
				return 0;
			}
next:
			ctx->pos += TEST_FS_DENTRY_SIZE;
		}
		testfs_put_page(page);
	}

	return 0;
}

size_t testfs_read_dir(struct file *filp, char __user *buf, size_t siz,
					loff_t *ppos)
{
	struct inode *inode = file_inode(filp);

	log_err("ino:%lu\n", inode->i_ino);
	return generic_read_dir(filp, __user buf, siz, ppos);
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

const struct inode_operations testfs_dir_iops = {
	.lookup		= testfs_lookup,
	.create		= testfs_create,
	.unlink		= testfs_unlink,
	.mkdir		= testfs_mkdir,
	.rmdir		= testfs_rmdir,
	.getattr        = testfs_getattr,
};
