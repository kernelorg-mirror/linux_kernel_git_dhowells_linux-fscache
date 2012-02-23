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

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/exportfs.h>
#include <linux/pagemap.h>
#include <linux/parser.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include "internal.h"

MODULE_LICENSE("GPL");

static struct kmem_cache *noisefs_inode_cache;

static struct inode *noisefs_alloc_inode(struct super_block *sb)
{
	struct noisefs_inode *ni;

	ni = kmem_cache_alloc(noisefs_inode_cache, GFP_KERNEL);
	if (!ni)
		return NULL;
	return &ni->vfs_inode;
}

static void noisefs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	struct noisefs_inode *ni = NOISEFS_I(inode);

	INIT_LIST_HEAD(&inode->i_dentry);
	kmem_cache_free(noisefs_inode_cache, ni);
}

static void noisefs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, noisefs_i_callback);
}

static const struct super_operations noisefs_super_ops = {
	.statfs		= simple_statfs,
	.alloc_inode	= noisefs_alloc_inode,
	.drop_inode	= generic_delete_inode,
	.destroy_inode	= noisefs_destroy_inode,
	.show_options	= generic_show_options,
};

static int noisefs_encode_fh(struct dentry *dentry, __u32 *fh,
			     int *max_len, int connectable)
{
	struct inode *inode = dentry->d_inode;
	int len = *max_len;
	int type = FILEID_INO32_GEN;

	kenter("%lx,,{%d},%d", inode->i_ino, len, connectable);

	if (connectable && (len < 4)) {
		*max_len = 4;
		return 255;
	} else if (len < 2) {
		*max_len = 2;
		return 255;
	}

	len = 2;
	fh[0] = inode->i_ino;
	fh[1] = inode->i_generation;
	if (connectable && !S_ISDIR(inode->i_mode)) {
		struct inode *parent;

		spin_lock(&dentry->d_lock);
		parent = dentry->d_parent->d_inode;
		fh[2] = parent->i_ino;
		fh[3] = parent->i_generation;
		spin_unlock(&dentry->d_lock);
		len = 4;
		type = FILEID_INO32_GEN_PARENT;
	}
	*max_len = len;
	kleave(" = %d [%d]", type, len);
	return type;
}

static struct inode *noisefs_nfs_get_inode(struct super_block *sb,
					   u64 ino, u32 generation)
{
	struct inode *inode;

	kenter(",%llx,%x", ino, generation);

	/* If the inode exists, it will be in core already. */
	inode = ilookup(sb, ino);
	if (!inode) {
		kleave(" = -ESTALE [no ino]");
		return ERR_PTR(-ESTALE);
	}
	if (generation && inode->i_generation != generation) {
		kleave(" = -ESTALE [%x!=%x]", inode->i_generation, generation);
		iput(inode);
		return ERR_PTR(-ESTALE);
	}
	return inode;
}

static struct dentry *noisefs_fh_to_dentry(struct super_block *sb,
					   struct fid *fid,
					   int fh_len, int fh_type)
{
	struct dentry *dentry;

	kenter(",,%d,%d", fh_len, fh_type);
	dentry = generic_fh_to_dentry(sb, fid, fh_len, fh_type,
				      noisefs_nfs_get_inode);
	kleave(" = %p", dentry);
	return dentry;
}

static struct dentry *noisefs_fh_to_parent(struct super_block *sb,
					   struct fid *fid,
					   int fh_len, int fh_type)
{
	struct dentry *dentry;

	kenter(",,%d,%d", fh_len, fh_type);
	dentry = generic_fh_to_parent(sb, fid, fh_len, fh_type,
				      noisefs_nfs_get_inode);
	kleave(" = %p", dentry);
	return dentry;
}

static struct dentry *noisefs_get_parent(struct dentry *child)
{
	struct dentry *dentry;

	kenter("%lx", child->d_inode ? child->d_inode->i_ino : 0);

	/* should we check !IS_ROOT(child)? */
	dentry = dget_parent(child);
	kleave(" = %lx", dentry->d_inode->i_ino);
	return dentry;
}

static const struct export_operations noisefs_export_ops = {
	.encode_fh	= noisefs_encode_fh,
	.fh_to_dentry	= noisefs_fh_to_dentry,
	.fh_to_parent	= noisefs_fh_to_parent,
	.get_parent	= noisefs_get_parent,
};

enum {
	Opt_fs_key,
	Opt_err
};

static const match_table_t noisefs_mountopt_tokens = {
	{ Opt_fs_key,	"fs_key=%d" },
	{ Opt_err,	NULL }
};

static int noisefs_parse_options(char *data, struct noisefs_super *s)
{
	substring_t args[MAX_OPT_ARGS];
	int option;
	int token;
	char *p;

	while ((p = strsep(&data, ",")) != NULL) {
		if (!*p)
			continue;

		token = match_token(p, noisefs_mountopt_tokens, args);
		switch (token) {
		case Opt_fs_key:
			if (match_int(&args[0], &option))
				return -EINVAL;
			break;
		default:
			pr_err("noisefs: Unknown mount option\n");
			return -EINVAL;
		}
	}

	return 0;
}

int noisefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct noisefs_super *s;
	struct inode *inode;
	struct dentry *root;
	int err;

	save_mount_options(sb, data);

	err = -ENOMEM;
	s = kzalloc(sizeof(struct noisefs_super), GFP_KERNEL);
	if (!s)
		goto fail;

	spin_lock_init(&s->ino_lock);
	s->special_ino = 0x80000000U;

	sb->s_fs_info = s;

	err = noisefs_parse_options(data, s);
	if (err)
		goto fail_have_super;

	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_blocksize		= PAGE_CACHE_SIZE;
	sb->s_blocksize_bits	= PAGE_CACHE_SHIFT;
	sb->s_magic		= 0x4E6F6973U;
	sb->s_op		= &noisefs_super_ops;
	sb->s_export_op		= &noisefs_export_ops;
	sb->s_time_gran		= 1;
	sb->s_flags		|= MS_I_VERSION;

	err = -ENOMEM;
	inode = noisefs_iget(sb, NULL,
			     S_IFDIR | S_IRUGO | S_IXUGO | S_IWUSR, 0);
	if (!inode)
		goto fail_have_super;

	root = d_alloc_root(inode);
	if (!root)
		goto fail_have_inode;
	sb->s_root = root;
	return 0;

fail_have_inode:
	iput(inode);
fail_have_super:
	kfree(sb->s_fs_info);
	sb->s_fs_info = NULL;
fail:
	return err;
}

struct dentry *noisefs_mount(struct file_system_type *fs_type,
			     int flags, const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags, data, noisefs_fill_super);
}

static void noisefs_kill_sb(struct super_block *sb)
{
	kfree(sb->s_fs_info);
	kill_litter_super(sb);
}

static struct file_system_type noisefs_fs_type = {
	.name		= "noisefs",
	.mount		= noisefs_mount,
	.kill_sb	= noisefs_kill_sb,
};

static int __init init_noisefs_fs(void)
{
	int ret;

	noisefs_inode_cache = kmem_cache_create("noisefs_inode_cache",
						sizeof(struct noisefs_inode),
						0,
						SLAB_HWCACHE_ALIGN,
						noisefs_i_init_once);
	if (!noisefs_inode_cache) {
		pr_notice("noisefs: Failed to allocate inode cache\n");
		ret = -ENOMEM;
		goto error;
	}

	ret = register_filesystem(&noisefs_fs_type);
	if (ret < 0) {
		pr_notice("noisefs: Failed to register filesystem\n");
		goto error_fsreg;
	}
	return 0;

error_fsreg:
	kmem_cache_destroy(noisefs_inode_cache);
error:
	return ret;
}

static void __exit exit_noisefs_fs(void)
{
	unregister_filesystem(&noisefs_fs_type);
	kmem_cache_destroy(noisefs_inode_cache);
}

module_init(init_noisefs_fs)
module_exit(exit_noisefs_fs)
