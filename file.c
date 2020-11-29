#include "testfs.h"

long testfs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(filp);

	log_err("ino:%lu cmd: %x, arg:%lx\n", inode->i_ino, cmd, arg);

	return 0;
}

#ifdef CONFIG_COMPAT
long testfs_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(filp);

	log_err("ino:%lu cmd: %x, arg:%lx\n", inode->i_ino, cmd, arg);

	return 0;
}
#endif

static int testfs_file_open(struct inode *inode, struct file *file)
{
	log_err("ino:%lu\n", inode->i_ino);

	return 0;
}

static int testfs_file_release(struct inode *inode, struct file *file)
{
	log_err("ino:%lu\n", inode->i_ino);

	return 0;
}

int testfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
        int ret;

        ret = generic_file_fsync(file, start, end, datasync);
        if (ret == -EIO)
                /* We don't really know where the IO error happened... */
                log_err("detected IO error when writing metadata buffers");
        return ret;
}

int testfs_getattr(const struct path *path, struct kstat *stat,
                unsigned int request_mask, unsigned int query_flags)
{
        struct inode *inode = d_backing_inode(path->dentry);

	log_err("ino:%lu\n", inode->i_ino);

        generic_fillattr(inode, stat);
        return 0;
}

const struct inode_operations testfs_file_iops = {
        .getattr        = testfs_getattr,
};

const struct file_operations testfs_file_fops = {
        .llseek         = generic_file_llseek,
        .read_iter      = generic_file_read_iter,
        .write_iter     = generic_file_write_iter,
        .unlocked_ioctl = testfs_ioctl,
#ifdef CONFIG_COMPAT
        .compat_ioctl   = testfs_compat_ioctl,
#endif
        .open           = testfs_file_open,
        .release        = testfs_file_release,
	.fsync		= testfs_fsync,
};
