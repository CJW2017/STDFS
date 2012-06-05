/*
 * linux/fs/lkfs/super.c
 * 
 * Disk struct:
 * [boot block 0 ][super block 1][inode table block 2 .. 33][data block 34 .. 1023]
 * 
 * Copyright (C) 2012 Hu Yugui(yugui.hu@hotmail.com)
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

static void init_once(void *foo)
{
	struct lkfs_inode_info *ei = (struct lkfs_inode_info *) foo;
	inode_init_once(&ei->vfs_inode);
}

static struct kmem_cache * lkfs_inode_cachep;
static int init_inodecache(void)
{
	lkfs_inode_cachep = kmem_cache_create("lkfs_inode_cache",
					     sizeof(struct lkfs_inode_info),
					     0, (SLAB_RECLAIM_ACCOUNT|
						SLAB_MEM_SPREAD),
					     init_once);
	if (lkfs_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	kmem_cache_destroy(lkfs_inode_cachep);
}

static struct inode *lkfs_alloc_inode(struct super_block *sb)
{
	struct lkfs_inode_info *ei;
	ei = (struct ext2_inode_info *)kmem_cache_alloc(lkfs_inode_cachep, GFP_KERNEL);
	if (!ei)
		return NULL;
	ei->vfs_inode.i_version = 1;
	return &ei->vfs_inode;
}

static void lkfs_destroy_inode(struct inode *inode)
{
	kmem_cache_free(lkfs_inode_cachep, LKFS_I(inode));
}

static int lkfs_statfs (struct dentry * dentry, struct kstatfs * buf)
{
	struct super_block *sb = dentry->d_sb;
	struct lkfs_sb_info *sbi = LKFS_SB(sb);
	struct lkfs_super_block *es = sbi->s_es;
	u64 fsid;

	buf->f_type = LKFS_SUPER_MAGIC;
	buf->f_bsize = sb->s_blocksize;
	buf->f_blocks = le32_to_cpu(es->s_blocks_count);
	buf->f_bfree = le32_to_cpu(es->s_free_blocks_count);
	buf->f_bavail = buf->f_bfree - le32_to_cpu(es->s_r_blocks_count);
	if (buf->f_bfree < le32_to_cpu(es->s_r_blocks_count))
		buf->f_bavail = 0;
	buf->f_files = le32_to_cpu(es->s_inodes_count);
	buf->f_ffree = le32_to_cpu(es->s_free_inodes_count );
	buf->f_namelen = LKFS_NAME_LEN;
	fsid = le64_to_cpup((void *)es->s_uuid) ^
	       le64_to_cpup((void *)es->s_uuid + sizeof(u64));
	buf->f_fsid.val[0] = fsid & 0xFFFFFFFFUL;
	buf->f_fsid.val[1] = (fsid >> 32) & 0xFFFFFFFFUL;
	return 0;
}

static void lkfs_sync_super(struct super_block *sb, int wait)
{
	mark_buffer_dirty(LKFS_SB(sb)->s_sbh);
	if (wait)
		sync_dirty_buffer(LKFS_SB(sb)->s_sbh); /* update superblock */
	sb->s_dirt = 0;
}

void lkfs_write_super(struct super_block *sb)
{
	lkfs_sync_super(sb, 1);
}

static const struct super_operations lkfs_sops = {
	.alloc_inode	= lkfs_alloc_inode,
	.destroy_inode	= lkfs_destroy_inode,
	.write_inode	= lkfs_write_inode,
	//.put_super	= lkfs_put_super,
	.write_super	= lkfs_write_super,
/*	.sync_fs	= lkfs_sync_fs,*/
	.statfs		= lkfs_statfs,
	/*.remount_fs	= lkfs_remount,*/
};

static int lkfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct buffer_head * bh;
	struct lkfs_sb_info * sbi;
	struct lkfs_super_block * es;
	struct inode *root;
	unsigned long block;
	unsigned long offset = 0;
	long ret = -EINVAL;
	int blocksize = BLOCK_SIZE;
	int err;

	sbi = kzalloc(sizeof(struct lkfs_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sb->s_fs_info = sbi;
	sbi->s_sb_block = 1; /* boot block */

	blocksize = sb_min_blocksize(sb, BLOCK_SIZE);
	if (!blocksize) {
		lkfs_debug("error: unable to set blocksize\n");
		goto failed_sbi;
	}

	if (!(bh = sb_bread(sb, 1))) {
		lkfs_debug("error: unable to read superblock\n");
		goto failed_sbi;
	}
	sbi->s_sbh = bh; /* super block buffer head in memory */
	es = (struct lkfs_super_block *) (((char *)bh->b_data) + offset);
	sbi->s_es = es;
	sb->s_magic = le16_to_cpu(es->s_magic);
	lkfs_debug("lkfs id: %s\n", es->s_volume_name);
	if (sb->s_magic != LKFS_SUPER_MAGIC)
		goto cantfind_lkfs;

	blocksize = BLOCK_SIZE << le32_to_cpu(sbi->s_es->s_log_block_size);
	sbi->s_inode_size = LKFS_GOOD_OLD_INODE_SIZE;
	sbi->s_first_ino = LKFS_GOOD_OLD_FIRST_INO;
	sb->s_maxbytes = 8192 << 10;

	if (sb->s_blocksize != bh->b_size) {
			lkfs_debug(sb, KERN_ERR, "error: unsupported blocksize\n");
		goto failed_mount;
	}

	sb->s_op = &lkfs_sops;

	root = lkfs_iget(sb, LKFS_ROOT_INO);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto failed_mount;
	}
	if (!S_ISDIR(root->i_mode) || !root->i_blocks || !root->i_size) {
		iput(root);
		lkfs_debug("error: corrupt root inode, run lkfsck\n");
		goto failed_mount;
	}

	sb->s_root = d_alloc_root(root);
	if (!sb->s_root) {
		iput(root);
		lkfs_debug("error: get root inode failed\n");
		ret = -ENOMEM;
		goto failed_mount;
	}
	return 0;

cantfind_lkfs:
	lkfs_debug("error: can't find an lkfs filesystem on dev %s.\n", sb->s_id);
	goto failed_mount;
	
failed_mount:
	brelse(bh);
failed_sbi:
	sb->s_fs_info = NULL;
	kfree(sbi);
	return ret;
}

static int lkfs_get_sb(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data, struct vfsmount *mnt)
{
	return get_sb_bdev(fs_type, flags, dev_name, data, lkfs_fill_super, mnt);
}

static struct file_system_type lkfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "lkfs",
	.get_sb		= lkfs_get_sb,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init init_lkfs_fs(void)
{
	int err = 0;
	err = init_inodecache();
	if (err)
		goto out;
        err = register_filesystem(&lkfs_fs_type);
	lkfs_debug("lkfs initialize...\n");
	lkfs_debug("super block size: %d\n", sizeof(struct lkfs_super_block));
	if (err)
		goto out;
	return 0;
out:
	destroy_inodecache();
	return err;
}

static void __exit exit_lkfs_fs(void)
{
	unregister_filesystem(&lkfs_fs_type);
	destroy_inodecache();
}

module_init(init_lkfs_fs)
module_exit(exit_lkfs_fs)