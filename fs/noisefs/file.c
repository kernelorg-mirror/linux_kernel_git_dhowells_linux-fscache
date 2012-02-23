/* Predictable noise generator file handler
 *
 * Copyright (C) 2011 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/aio.h>
#include <linux/uaccess.h>
#include "internal.h"

/*
 * Fabricate data for the reader to read
 */
static ssize_t noisefs_read(struct file *filp, char __user *buf, size_t len,
			    loff_t *ppos)
{
	struct inode *i = filp->f_dentry->d_inode;
	struct noisefs_inode *ni = NOISEFS_I(i);
	ssize_t ret;
	size_t piece;
	loff_t pos = *ppos, eof, avail;
	__be32 raw;
	const void const *raw_p = &raw;
	u32 data;

	kenter("%lu,,%zu,%llu", i->i_ino, len, pos);

	if (pos + len < pos)
		len = (loff_t)-1ULL - pos;
	if (len == 0) {
		kleave(" = 0 [len 0]");
		return 0;
	}

	if (ni->inject_error_at && i->i_version >= ni->inject_error_at) {
		pr_warning("noisefs_read() injected error\n");
		return ni->error_to_inject;
	}

	/* Determine the word covering the starting position and file size */
	spin_lock(&i->i_lock);
	eof = i_size_read(i);
	data = NOISEFS_SB(i->i_sb)->fs_key;
	data += i->i_ino + i->i_version;
	data += pos >> 2;
	spin_unlock(&i->i_lock);

	kdebug("start with %08x eof %llx", data, eof);

	if (pos >= eof) {
		kleave(" = 0 [after eof]");
		return 0;
	}

	/* Shrink the read to the part before the EOF */
	avail = eof - pos;
	if (avail < len)
		len = avail;
	*ppos = pos + len;
	ret = len;

	kdebug("readable %lx/%llx", len, avail);

	/* handle a misaligned starting position */
	if (pos & 3) {
		unsigned off = pos & 3;

		piece = sizeof(data) - off;
		piece = (len > piece) ? piece : len;

		kdebug("misaligned start %zu/%zu", piece, len);

		raw = cpu_to_be32(data);
		if (copy_to_user(buf, raw_p + off, piece))
			goto fault;
		buf += piece;
		len -= piece;
		data++;
	}

	/* Handle all the whole words */
	if (len >= 4) {
		kdebug("whole words %zu", len & ~3);
		do {
			raw = cpu_to_be32(data);
			if (put_user(raw, (__be32 __user *)buf) < 0)
				goto fault;
			buf += 4;
			len -= 4;
			data++;
		} while (len >= 4);
	}

	/* Handle trailing partial word */
	if (len > 0) {
		kdebug("partial trailer %zu", len);
		raw = cpu_to_be32(data);
		if (copy_to_user(buf, raw_p, len))
			goto fault;
	}

	kleave(" = %zd [%llx]", ret, *ppos);
	return ret;

fault:
	kleave(" = -EFAULT");
	return -EFAULT;
}

/*
 * Note a write, but don't actually store the data
 */
static ssize_t noisefs_write(struct file *filp, const char __user *buf,
			     size_t len, loff_t *ppos)
{
	struct inode *i = filp->f_dentry->d_inode;
	struct noisefs_inode *ni = NOISEFS_I(i);
	loff_t pos = *ppos, eof;
	time_t now;

	kenter("%lu{%llx},,%zu,%llu", i->i_ino, i->i_version, len, pos);

	if (pos + len < pos)
		return -EFBIG;

	if (ni->inject_error_at && i->i_version >= ni->inject_error_at) {
		pr_warning("noisefs_write() injected error\n");
		return ni->error_to_inject;
	}

	now = CURRENT_TIME.tv_sec;

	spin_lock(&i->i_lock);

	eof = i_size_read(i);

	if (filp->f_flags & O_APPEND)
		pos = eof;

	if (pos + len > eof)
		i_size_write(i, pos + len);
	i->i_version++;
	if (ni->iver_timeout)
		ni->iver_expiry = now + ni->iver_timeout;

	spin_unlock(&i->i_lock);
	*ppos = pos + len;
	return len;
}

static ssize_t noisefs_aio_write(struct kiocb *iocb,
				 const struct iovec *iov, unsigned long nr_segs,
				 loff_t pos)
{
	loff_t pos_copy = pos;
	return noisefs_write(iocb->ki_filp, NULL, iocb->ki_nbytes, &pos_copy);
}

static ssize_t noisefs_splice_write(struct pipe_inode_info *pipe,
				    struct file *out,
				    loff_t *ppos, size_t len,
				    unsigned int flags)
{
	return noisefs_write(out, NULL, len, ppos);
}

const struct file_operations noisefs_file_operations = {
	.read		= noisefs_read,
	.write		= noisefs_write,
	.aio_write	= noisefs_aio_write,
	.splice_write	= noisefs_splice_write,
	.llseek		= generic_file_llseek,
	.fsync		= noop_fsync,
};

static int noisefs_extract_val(const void *value, size_t size,
			       unsigned *_val)
{
	if (size > 0 && value && *(char *)value) {
		char buf[8];
		if (size > sizeof(buf) - 1)
			return -EINVAL;
		memcpy(buf, value, size);
		buf[size] = 0;
		return kstrtouint(value, 10, _val);
	}

	return 0;
}

static int noisefs_setxattr(struct dentry *dentry, const char *name,
			    const void *value, size_t size, int flags)
{
	struct inode *i = dentry->d_inode;
	struct noisefs_inode *ni = NOISEFS_I(i);

	kenter("%lu,%s,,,", i->i_ino, name);

	if (!name || !*name)
		return -EOPNOTSUPP;

	if (strcmp(name, "iversion_timo") == 0) {
		unsigned timeout = 0;

		if (noisefs_extract_val(value, size, &timeout) < 0)
			return -EINVAL;

		/* increment i_version after first getattr on this version */
		kdebug("set timeout %x", timeout);
		spin_lock(&ni->lock);
		ni->iver_timeout = timeout;
		if (!timeout)
			ni->iver_expiry = 0;
		spin_unlock(&ni->lock);
		return 0;
	}

	if (strcmp(name, "inject_error_at") == 0) {
		unsigned when = 0;

		if (noisefs_extract_val(value, size, &when) < 0)
			return -EINVAL;
		ni->inject_error_at = when;
		return 0;
	}

	if (strcmp(name, "error_to_inject") == 0) {
		unsigned err = 0;

		if (noisefs_extract_val(value, size, &err) < 0 ||
		    err <= 0 || err >= 512)
			return -EINVAL;
		ni->error_to_inject = -err;
		return 0;
	}

	return -EOPNOTSUPP;
}

static int noisefs_setattr(struct dentry *dentry, struct iattr *iattr)
{
	struct inode *i = dentry->d_inode;
	struct noisefs_inode *ni = NOISEFS_I(i);
	time_t now;
	int error;

	kenter("%lu{%llx},{%x}", i->i_ino, i->i_version, iattr->ia_valid);

	error = inode_change_ok(i, iattr);
	if (error)
		return error;

	if (ni->inject_error_at && i->i_version >= ni->inject_error_at) {
		pr_warning("noisefs_setattr() preinjected error\n");
		return ni->error_to_inject;
	}

	if (iattr->ia_valid & ATTR_SIZE) {
		now = CURRENT_TIME.tv_sec;
		spin_lock(&i->i_lock);
		i_size_write(i, iattr->ia_size);
		i->i_version++;
		if (ni->iver_timeout)
			ni->iver_expiry = now + ni->iver_timeout;
		spin_unlock(&i->i_lock);
	}

	if (ni->inject_error_at && i->i_version >= ni->inject_error_at) {
		pr_warning("noisefs_read() injected error\n");
		return ni->error_to_inject;
	}

	setattr_copy(i, iattr);
	mark_inode_dirty(i);
	return 0;
}

static int noisefs_getattr(struct vfsmount *mnt, struct dentry *dentry,
			   struct kstat *stat)
{
	struct inode *i = dentry->d_inode;
	struct noisefs_inode *ni = NOISEFS_I(i);
	time_t now;

	if (ni->iver_timeout) {
		now = CURRENT_TIME.tv_sec;
		spin_lock(&ni->lock);
		if (ni->iver_timeout) {
			if (!ni->iver_expiry || now > ni->iver_expiry) {
				if (ni->iver_expiry) {
					kdebug("i_version expired");
					spin_lock(&i->i_lock);
					i->i_version++;
					spin_unlock(&i->i_lock);
				}
				ni->iver_expiry = now + ni->iver_timeout;
			}
		}
		spin_unlock(&ni->lock);
	}

	if (ni->inject_error_at && i->i_version >= ni->inject_error_at) {
		pr_warning("noisefs_getattr() injected error\n");
		return ni->error_to_inject;
	}

	return simple_getattr(mnt, dentry, stat);
}

const struct inode_operations noisefs_file_inode_operations = {
	.getattr	= noisefs_getattr,
	.setattr	= noisefs_setattr,
	.setxattr	= noisefs_setxattr,
};

const struct address_space_operations noisefs_aops = {
	.readpage	= simple_readpage,
	.write_begin	= simple_write_begin,
	.write_end	= simple_write_end,
	.set_page_dirty = __set_page_dirty_no_writeback,
};
