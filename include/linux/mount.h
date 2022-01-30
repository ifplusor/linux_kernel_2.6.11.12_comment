/*
 *
 * Definitions for mount interface. This describes the in the kernel build 
 * linkedlist with mounted filesystems.
 *
 * Author:  Marco van Wieringen <mvw@planets.elm.net>
 *
 * Version: $Id: mount.h,v 2.0 1996/11/17 16:48:14 mvw Exp mvw $
 *
 */
#ifndef _LINUX_MOUNT_H
#define _LINUX_MOUNT_H
#ifdef __KERNEL__

#include <linux/list.h>
#include <linux/spinlock.h>
#include <asm/atomic.h>

#define MNT_NOSUID	1	/* 在已经安装文件系统中禁止setuid和setgid标志 */
#define MNT_NODEV	2	/* 在已经安装文件系统中禁止访问设备文件 */
#define MNT_NOEXEC	4	/* 在已经安装文件系统中不允许程序执行 */

/**
 * 已安装文件系统描述符
 */
struct vfsmount
{
	struct list_head mnt_hash;	/* 用于散列表链表的指针 */
	struct vfsmount *mnt_parent;	/* fs we are mounted on */
	struct dentry *mnt_mountpoint;	/* dentry of mountpoint */
	struct dentry *mnt_root;	/* root of the mounted tree */
	struct super_block *mnt_sb;	/* pointer to superblock */
	struct list_head mnt_mounts;	/* list of children, anchored here */
	struct list_head mnt_child;	/* and going through their mnt_child */
	atomic_t mnt_count;		/* 引用计数器 */
	int mnt_flags;			/* mount标志 */
	int mnt_expiry_mark;		/* true if marked for expiry */
	char *mnt_devname;		/* Name of device e.g. /dev/dsk/hda1 */
	struct list_head mnt_list;	/* 用于namespace的已安装文件系统链表的指针 */
	struct list_head mnt_fslink;	/* link in fs-specific expiry list */
	struct namespace *mnt_namespace; /* containing namespace */
};

static inline struct vfsmount *mntget(struct vfsmount *mnt)
{
	if (mnt)
		atomic_inc(&mnt->mnt_count);
	return mnt;
}

extern void __mntput(struct vfsmount *mnt);

static inline void _mntput(struct vfsmount *mnt)
{
	if (mnt) {
		if (atomic_dec_and_test(&mnt->mnt_count))
			__mntput(mnt);
	}
}

static inline void mntput(struct vfsmount *mnt)
{
	if (mnt) {
		mnt->mnt_expiry_mark = 0;
		_mntput(mnt);
	}
}

extern void free_vfsmnt(struct vfsmount *mnt);
extern struct vfsmount *alloc_vfsmnt(const char *name);
extern struct vfsmount *do_kern_mount(const char *fstype, int flags,
				      const char *name, void *data);

struct nameidata;

extern int do_add_mount(struct vfsmount *newmnt, struct nameidata *nd,
			int mnt_flags, struct list_head *fslist);

extern void mark_mounts_for_expiry(struct list_head *mounts);

/**
 * 保护已经安装文件系统的链表。
 */
extern spinlock_t vfsmount_lock;

#endif
#endif /* _LINUX_MOUNT_H */
