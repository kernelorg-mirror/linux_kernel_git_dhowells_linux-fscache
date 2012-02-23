/* Predictable noise generator filesystem
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
#include <linux/time.h>
#include <linux/pagemap.h>
#include <linux/backing-dev.h>
#include <linux/slab.h>
#include "internal.h"

static const struct inode_operations noisefs_dir_inode_operations;

static struct backing_dev_info noisefs_backing_dev_info = {
	.name		= "noisefs",
	.ra_pages	= 0,	/* No readahead */
	.capabilities	= BDI_CAP_NO_ACCT_AND_WRITEBACK,
};

void noisefs_i_init_once(void *_inode)
{
	struct noisefs_inode *ni = _inode;

	memset(ni, 0, sizeof(*ni));
	inode_init_once(&ni->vfs_inode);
	spin_lock_init(&ni->lock);
}

/*
 * Get an inode and initialise it
 */
struct inode *noisefs_iget(struct super_block *sb, const struct inode *dir,
			   int mode, dev_t dev)
{
	struct noisefs_super *s = NOISEFS_SB(sb);
	struct noisefs_inode *ni;
	struct inode *inode;
	ino_t ino;

	kenter(",%lx,%o,%x", dir ? dir->i_ino : 0, mode, dev);

	spin_lock(&s->ino_lock);
	if (S_ISREG(mode)) {
		if (s->regular_ino == INT_MAX)
			goto out_of_inos;
		ino = ++s->regular_ino;
	} else {
		if (s->special_ino == UINT_MAX)
			goto out_of_inos;
		ino = ++s->special_ino;
	}
	spin_unlock(&s->ino_lock);

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW)) {
		kenter(" = {%lx} [extant]", inode->i_ino);
		return inode;
	}

	inode_init_owner(inode, dir, mode);
	inode->i_mapping->a_ops = &noisefs_aops;
	inode->i_mapping->backing_dev_info = &noisefs_backing_dev_info;
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_unevictable(inode->i_mapping);
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;

	switch (mode & S_IFMT) {
	default:
		init_special_inode(inode, mode, dev);
		break;
	case S_IFREG:
		inode->i_op = &noisefs_file_inode_operations;
		inode->i_fop = &noisefs_file_operations;
		break;
	case S_IFDIR:
		inode->i_op = &noisefs_dir_inode_operations;
		inode->i_fop = &simple_dir_operations;

		/* directory inodes start off with i_nlink == 2 (for "." entry) */
		inc_nlink(inode);
		break;
	case S_IFLNK:
		inode->i_op = &page_symlink_inode_operations;
		break;
	}

	ni = NOISEFS_I(inode);
	ni->error_to_inject = -EIO;

	unlock_new_inode(inode);
	kenter(" = {%lx} [new]", inode->i_ino);
	return inode;

out_of_inos:
	spin_unlock(&s->ino_lock);
	pr_warning("noisefs: Ran out of inode numbers\n");
	return NULL;
}

/*
 * File creation. Allocate an inode, and we're done..
 */
static int noisefs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode,
			 dev_t dev)
{
	struct inode *inode = noisefs_iget(dir->i_sb, dir, mode, dev);
	int error = -ENOSPC;

	if (inode) {
		d_instantiate(dentry, inode);
		dget(dentry);	/* Extra count - pin the dentry in core */
		dir->i_mtime = dir->i_ctime = CURRENT_TIME;
		error = 0;
	}
	return error;
}

static int noisefs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int retval = noisefs_mknod(dir, dentry, mode | S_IFDIR, 0);
	if (!retval)
		inc_nlink(dir);
	return retval;
}

static int noisefs_create(struct inode *dir, struct dentry *dentry,
			  umode_t mode, struct nameidata *nd)
{
	return noisefs_mknod(dir, dentry, mode | S_IFREG, 0);
}

static int noisefs_symlink(struct inode *dir, struct dentry *dentry,
			   const char *symname)
{
	struct inode *inode;
	int error = -ENOSPC;

	inode = noisefs_iget(dir->i_sb, dir, S_IFLNK|S_IRWXUGO, 0);
	if (inode) {
		int l = strlen(symname) + 1;
		error = page_symlink(inode, symname, l);
		if (!error) {
			d_instantiate(dentry, inode);
			dget(dentry);
			dir->i_mtime = dir->i_ctime = CURRENT_TIME;
		} else {
			iput(inode);
		}
	}
	return error;
}

static const struct inode_operations noisefs_dir_inode_operations = {
	.create		= noisefs_create,
	.lookup		= simple_lookup,
	.link		= simple_link,
	.unlink		= simple_unlink,
	.symlink	= noisefs_symlink,
	.mkdir		= noisefs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= noisefs_mknod,
	.rename		= simple_rename,
};
