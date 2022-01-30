#ifndef _LINUX_FS_STRUCT_H
#define _LINUX_FS_STRUCT_H

struct dentry;
struct vfsmount;

/**
 * 文件系统结构
 */
struct fs_struct {
	atomic_t count;	/* 共享该表的进程个数 */
	rwlock_t lock;	/* 用于表中字段读/写的自旋锁 */
	int umask;	/* 当打开文件设置文件权限时使用的位掩码 */
	/*
	 * root:	根目录的目录项
	 * pwd:		当前工作目录的目录项
	 * altroot:	模拟根目录的目录项（x86上未使用）
	 */
	struct dentry * root, * pwd, * altroot;
	/*
	 * rootmnt:	根目录所安装的文件系统对象
	 * pwdmnt:	当前工作目录所安装的文件系统对象
	 * altrootmnt:	模拟根目录所安装的文件系统对象
	 */
	struct vfsmount * rootmnt, * pwdmnt, * altrootmnt;
};

#define INIT_FS {				\
	.count		= ATOMIC_INIT(1),	\
	.lock		= RW_LOCK_UNLOCKED,	\
	.umask		= 0022, \
}

extern void exit_fs(struct task_struct *);
extern void set_fs_altroot(void);
extern void set_fs_root(struct fs_struct *, struct vfsmount *, struct dentry *);
extern void set_fs_pwd(struct fs_struct *, struct vfsmount *, struct dentry *);
extern struct fs_struct *copy_fs_struct(struct fs_struct *);
extern void put_fs_struct(struct fs_struct *);

#endif /* _LINUX_FS_STRUCT_H */
