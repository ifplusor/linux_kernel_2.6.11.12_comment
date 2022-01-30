#ifndef _NAMESPACE_H_
#define _NAMESPACE_H_
#ifdef __KERNEL__

#include <linux/mount.h>
#include <linux/sched.h>

/**
 * 命名空间，表示进程的已安装文件系统树。
 *
 * 进程安装或卸载文件系统时，仅修改它的命名空间。修改对共享同一命名空间的所有进程可见，也只对它们可见。
 */
struct namespace {
	atomic_t		count;	/* 引用计数器 */
	struct vfsmount *	root;	/* 命名空间根目录的已安装文件系统描述符 */
	struct list_head	list;	/* 所有已安装文件系统描述符链表的头 */
	struct rw_semaphore	sem;	/* 保护这个结构的读/写信号量 */
};

extern void umount_tree(struct vfsmount *);
extern int copy_namespace(int, struct task_struct *);
extern void __put_namespace(struct namespace *namespace);

static inline void put_namespace(struct namespace *namespace)
{
	if (atomic_dec_and_test(&namespace->count))
		__put_namespace(namespace);
}

/**
 * 从进程描述符中分离出与命名空间相关的数据结构。
 * 如果没有其他进程共享该结构，还删除所有这些数据结构。
 */
static inline void exit_namespace(struct task_struct *p)
{
	struct namespace *namespace = p->namespace;
	if (namespace) {
		task_lock(p);
		p->namespace = NULL;
		task_unlock(p);
		put_namespace(namespace);
	}
}

static inline void get_namespace(struct namespace *namespace)
{
	atomic_inc(&namespace->count);
}

#endif
#endif
