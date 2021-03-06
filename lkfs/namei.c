/*
 * linux/fs/lkfs/namei.c
*/
#include <linux/module.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/parser.h>
#include <linux/random.h>
#include <linux/buffer_head.h>
#include <linux/exportfs.h>
#include <linux/vfs.h>
#include <linux/seq_file.h>
#include <linux/mount.h>
#include <linux/log2.h>
#include <linux/quotaops.h>
#include <asm/uaccess.h>
#include "lkfs.h"

static struct dentry *lkfs_lookup(struct inode * dir, struct dentry *dentry, struct nameidata *nd)
{
	struct inode * inode;
	ino_t ino;
	
	if (dentry->d_name.len > LKFS_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	ino = lkfs_inode_by_name(dir, &dentry->d_name);
	inode = NULL;
	if (ino) {
		inode = lkfs_iget(dir->i_sb, ino);
		if (unlikely(IS_ERR(inode))) {
			if (PTR_ERR(inode) == -ESTALE) {
				lkfs_debug("deleted inode referenced: %lu", (unsigned long) ino);
				return ERR_PTR(-EIO);
			} else {
				return ERR_CAST(inode);
			}
		}
	}
	return d_splice_alias(inode, dentry);
}

static inline int lkfs_add_nondir(struct dentry *dentry, struct inode *inode)
{
	int err = lkfs_add_link(dentry, inode);
	if (!err) {
		d_instantiate(dentry, inode);
		unlock_new_inode(inode);
		return 0;
	}
	inode_dec_link_count(inode);
	unlock_new_inode(inode);
	iput(inode);
	return err;
}

static int lkfs_create (struct inode * dir, struct dentry * dentry, int mode, struct nameidata *nd)
{
	struct inode *inode;

	inode = lkfs_new_inode(dir, mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	inode->i_op = &lkfs_file_inode_operations;
	inode->i_mapping->a_ops = &lkfs_aops;
	inode->i_fop = &lkfs_file_operations;
	mark_inode_dirty(inode);
	lkfs_sync_inode(inode); /* writeback inode to disk */
	
	return lkfs_add_nondir(dentry, inode);
}

static int lkfs_mknod (struct inode * dir, struct dentry *dentry, int mode, dev_t rdev)
{
	//struct inode * inode;
	//int err;

	return 0;
}

static int lkfs_symlink (struct inode * dir, struct dentry * dentry,
	const char * symname)
{
	//int err = -ENAMETOOLONG;
	return 0;
}

static int lkfs_link (struct dentry * old_dentry, struct inode * dir,
	struct dentry *dentry)
{
	//struct inode *inode = old_dentry->d_inode;
	//int err;

	return 0;
}

static int lkfs_mkdir(struct inode * dir, struct dentry * dentry, int mode)
{
	struct inode * inode;
	int err = -EMLINK;

	if (dir->i_nlink >= 32000)
		goto out;

	inode_inc_link_count(dir);

	inode = lkfs_new_inode (dir, S_IFDIR | mode);
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out_dir;

	inode->i_op = &lkfs_dir_inode_operations;
	inode->i_fop = &lkfs_dir_operations;

	inode->i_mapping->a_ops = &lkfs_aops;

	inode_inc_link_count(inode);

	err = lkfs_make_empty(inode, dir);
	if (err)
		goto out_fail;

	err = lkfs_add_link(dentry, inode);
	if (err)
		goto out_fail;

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);
out:
	return err;

out_fail:
	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	unlock_new_inode(inode);
	iput(inode);
out_dir:
	inode_dec_link_count(dir);
	goto out;
}

/* just dec i_nlink number, if i_nlink number is 0, drop_inode can delete inode from disk */
static int lkfs_unlink(struct inode * dir, struct dentry *dentry)
{
	struct inode * inode = dentry->d_inode;
	struct lkfs_dir_entry_2 * de;
	struct page * page;
	int err = -ENOENT;

	de = lkfs_find_entry(dir, &dentry->d_name, &page);
	if (!de)
		goto out;

	err = lkfs_delete_entry (de, page);
	if (err)
		goto out;

	inode->i_ctime = dir->i_ctime;
	inode_dec_link_count(inode);
	err = 0;
out:
	return err;
}

static int lkfs_rmdir (struct inode * dir, struct dentry *dentry)
{
	//struct inode * inode = dentry->d_inode;
	int err = -ENOTEMPTY;
	return err;
}

static int lkfs_rename (struct inode * old_dir, struct dentry * old_dentry,
	struct inode * new_dir,	struct dentry * new_dentry )
{
	return 0;
}

const struct inode_operations lkfs_dir_inode_operations = {
	.create		= lkfs_create,
	.lookup		= lkfs_lookup,
	.link		= lkfs_link,
	.unlink		= lkfs_unlink,
	.symlink	= lkfs_symlink,
	.mkdir		= lkfs_mkdir,
	.rmdir		= lkfs_rmdir,
	.mknod		= lkfs_mknod,
	.rename		= lkfs_rename,
	.setattr	= lkfs_setattr,
};

const struct inode_operations lkfs_special_inode_operations = {
	.setattr	= lkfs_setattr,
};

