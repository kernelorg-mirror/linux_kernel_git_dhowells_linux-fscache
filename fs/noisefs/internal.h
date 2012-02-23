/* Predictable noise generator filesystem internal defs
 *
 * Copyright (C) 2011 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

struct noisefs_super {
	spinlock_t	ino_lock;	/* Lock for *_ino */
	unsigned long	regular_ino;	/* Last regular inode number allocated */
	unsigned long	special_ino;	/* Last special inode number allocated */
	unsigned	fs_key;		/* Filesystem noise key */
};

static inline struct noisefs_super *NOISEFS_SB(struct super_block *sb)
{
	return (struct noisefs_super *)sb->s_fs_info;
}

struct noisefs_inode {
	struct inode	vfs_inode;
	spinlock_t	lock;
	unsigned	iver_timeout;
	time_t		iver_expiry;
	unsigned	inject_error_at;
	int		error_to_inject;
};

static inline struct noisefs_inode *NOISEFS_I(struct inode *inode)
{
	return container_of(inode, struct noisefs_inode, vfs_inode);
}

/*
 * file.c
 */
extern const struct address_space_operations noisefs_aops;
extern const struct file_operations noisefs_file_operations;
extern const struct inode_operations noisefs_file_inode_operations;

/*
 * inode.c
 */
extern void noisefs_i_init_once(void *);
extern struct inode *noisefs_iget(struct super_block *,
				  const struct inode *, int, dev_t);

/*
 * Debugging
 */
#define dbgprintk(FMT, ...) \
	no_printk("[%-6.6s] "FMT"\n", current->comm, ##__VA_ARGS__)
#define kenter(FMT, ...) dbgprintk("==> %s("FMT")", __func__ , ##__VA_ARGS__)
#define kleave(FMT, ...) dbgprintk("<== %s()"FMT"", __func__ , ##__VA_ARGS__)
#define kdebug(FMT, ...) dbgprintk("    "FMT, ##__VA_ARGS__)
