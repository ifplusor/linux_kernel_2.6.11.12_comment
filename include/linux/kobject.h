/*
 * kobject.h - generic kernel object infrastructure.
 *
 * Copyright (c) 2002-2003	Patrick Mochel
 * Copyright (c) 2002-2003	Open Source Development Labs
 *
 * This file is released under the GPLv2.
 *
 * 
 * Please read Documentation/kobject.txt before using the kobject
 * interface, ESPECIALLY the parts about reference counts and object
 * destructors. 
 */

#ifndef _KOBJECT_H_
#define _KOBJECT_H_

#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/list.h>
#include <linux/sysfs.h>
#include <linux/rwsem.h>
#include <linux/kref.h>
#include <linux/kobject_uevent.h>
#include <linux/kernel.h>
#include <asm/atomic.h>

#define KOBJ_NAME_LEN	20

/* counter to tag the hotplug event, read only except for the kobject core */
extern u64 hotplug_seqnum;

/*
 * 对应于sysfs文件系统中的一个目录项，它通常被嵌入一个叫做“容器”的更大对象中。
 * 典型的容器有总线、设备及驱动程序描述符。
 */
struct kobject {
	char			* k_name;	/* 指向容器名称 */
	char			name[KOBJ_NAME_LEN];	/* 如果容器名称不超过20个字符，就存在这里 */
	struct kref		kref;		/* 引用计数器 */
	struct list_head	entry;		/* 用于将kobject插入某个链表 */
	struct kobject		* parent;	/* 指向父kobject */
	struct kset		* kset;		/* 指向包含的kset。kset是同类型的kobject结构的一个集合体 */
	struct kobj_type	* ktype;	/* 指向kobject的类型描述符 */
	struct dentry		* dentry;	/* 指向与kobject对应的sysfs文件系统的dentry */
};

extern int kobject_set_name(struct kobject *, const char *, ...)
	__attribute__((format(printf,2,3)));

static inline char * kobject_name(struct kobject * kobj)
{
	return kobj->k_name;
}

extern void kobject_init(struct kobject *);
extern void kobject_cleanup(struct kobject *);

extern int kobject_add(struct kobject *);
extern void kobject_del(struct kobject *);

extern int kobject_rename(struct kobject *, char *new_name);

extern int kobject_register(struct kobject *);
extern void kobject_unregister(struct kobject *);

extern struct kobject * kobject_get(struct kobject *);
extern void kobject_put(struct kobject *);

extern char * kobject_get_path(struct kobject *, int);

/**
 * kobject的类型
 */
struct kobj_type {
	void (*release)(struct kobject *);		/* release方法，用于释放kobject（容器） */
	struct sysfs_ops	* sysfs_ops;		/* sysfs操作表 */
	struct attribute	** default_attrs;	/* sysfs文件系统的缺省属性链表 */
};


/**
 *	kset - a set of kobjects of a specific type, belonging
 *	to a specific subsystem.
 *
 *	All kobjects of a kset should be embedded in an identical 
 *	type. This type may have a descriptor, which the kset points
 *	to. This allows there to exist sets of objects of the same
 *	type in different subsystems.
 *
 *	A subsystem does not have to be a list of only one type 
 *	of object; multiple ksets can belong to one subsystem. All 
 *	ksets of a subsystem share the subsystem's lock.
 *
 *      Each kset can support hotplugging; if it does, it will be given
 *      the opportunity to filter out specific kobjects from being
 *      reported, as well as to add its own "data" elements to the
 *      environment being passed to the hotplug helper.
 */
/**
 * 控制热插拨事件的结构。由kset的hotplug_ops指向该结构。
 * 如果kset不包含一个指定的kobject，则在sysfs分层结构中进行搜索，直到找到一个包含有kset的kobject为止，然后使用这个kset的热插拨操作。
 */
struct kset_hotplug_ops {
	/**
	 * 无论何时，当内核要为指定的kobject产生事件时，都要调用filter函数。如果filter函数返回0，将不产生事件。
	 * 因此，该函数给kset一个机会，用于决定是否向用户空间传递指定的事件。
	 * 使用此函数的一个例子是块设备子系统。在block_hotplug_filter中，只为kobject产生磁盘和分区事件，而不会为请求队列kobject产生事件。
	 */
	int (*filter)(struct kset *kset, struct kobject *kobj);
	/**
	 * 在调用用户空间的热插拨程序时，相关子系统的名字将作为唯一的参数传递给它。
	 * name方法负责提供此名字。它将返回一个适合传递给用户空间的字符串。
	 */
	char *(*name)(struct kset *kset, struct kobject *kobj);
	/**
	 * 任何热插拨脚本所需要知道的信息将通过环境变量传递。最后一个hotplug方法会在调用脚本前，提供添加环境变量的机会。
	 */
	int (*hotplug)(struct kset *kset, struct kobject *kobj, char **envp,
			int num_envp, char *buffer, int buffer_size);
};

/**
 * kset - a set of kobjects of a specific type, belonging
 * to a specific subsystem.
 *
 * 当创建一个kobject对象时，通常需要将它们加入到kset中。
 */
struct kset {
	struct subsystem	* subsys;	/* 所属子系统 */
	struct kobj_type	* ktype;	/* 所包含的kobject的类型 */
	struct list_head	list;		/* 包含在kset中的kobject链表的首部 */
	struct kobject		kobj;		/* 嵌入的kobject */
	struct kset_hotplug_ops	* hotplug_ops;	/* 用于处理kobject结构的过滤和热插拨操作的回调函数表 */
};


extern void kset_init(struct kset * k);
extern int kset_add(struct kset * k);
extern int kset_register(struct kset * k);
extern void kset_unregister(struct kset * k);

static inline struct kset * to_kset(struct kobject * kobj)
{
	return kobj ? container_of(kobj,struct kset,kobj) : NULL;
}

static inline struct kset * kset_get(struct kset * k)
{
	return k ? to_kset(kobject_get(&k->kobj)) : NULL;
}

static inline void kset_put(struct kset * k)
{
	kobject_put(&k->kobj);
}

static inline struct kobj_type * get_ktype(struct kobject * k)
{
	if (k->kset && k->kset->ktype)
		return k->kset->ktype;
	else 
		return k->ktype;
}

extern struct kobject * kset_find_obj(struct kset *, const char *);


/**
 * Use this when initializing an embedded kset with no other 
 * fields to initialize.
 */
#define set_kset_name(str)	.kset = { .kobj = { .name = str } }



/**
 * 子系统，kset的集合
 */
struct subsystem {
	struct kset		kset;	/* 下层对象集合 */
	struct rw_semaphore	rwsem;	/* 访问子系统所用的读写信号量 */
};

/**
 * 定义一个子系统。
 */
#define decl_subsys(_name,_type,_hotplug_ops) \
struct subsystem _name##_subsys = { \
	.kset = { \
		.kobj = { .name = __stringify(_name) }, \
		.ktype = _type, \
		.hotplug_ops =_hotplug_ops, \
	} \
}
#define decl_subsys_name(_varname,_name,_type,_hotplug_ops) \
struct subsystem _varname##_subsys = { \
	.kset = { \
		.kobj = { .name = __stringify(_name) }, \
		.ktype = _type, \
		.hotplug_ops =_hotplug_ops, \
	} \
}

/* The global /sys/kernel/ subsystem for people to chain off of */
extern struct subsystem kernel_subsys;

/**
 * Helpers for setting the kset of registered objects.
 * Often, a registered object belongs to a kset embedded in a 
 * subsystem. These do no magic, just make the resulting code
 * easier to follow. 
 */

/**
 *	kobj_set_kset_s(obj,subsys) - set kset for embedded kobject.
 *	@obj:		ptr to some object type.
 *	@subsys:	a subsystem object (not a ptr).
 *
 *	Can be used for any object type with an embedded ->kobj.
 */

#define kobj_set_kset_s(obj,subsys) \
	(obj)->kobj.kset = &(subsys).kset

/**
 *	kset_set_kset_s(obj,subsys) - set kset for embedded kset.
 *	@obj:		ptr to some object type.
 *	@subsys:	a subsystem object (not a ptr).
 *
 *	Can be used for any object type with an embedded ->kset.
 *	Sets the kset of @obj's  embedded kobject (via its embedded
 *	kset) to @subsys.kset. This makes @obj a member of that 
 *	kset.
 */

#define kset_set_kset_s(obj,subsys) \
	(obj)->kset.kobj.kset = &(subsys).kset

/**
 *	subsys_set_kset(obj,subsys) - set kset for subsystem
 *	@obj:		ptr to some object type.
 *	@subsys:	a subsystem object (not a ptr).
 *
 *	Can be used for any object type with an embedded ->subsys.
 *	Sets the kset of @obj's kobject to @subsys.kset. This makes
 *	the object a member of that kset.
 */

#define subsys_set_kset(obj,_subsys) \
	(obj)->subsys.kset.kobj.kset = &(_subsys).kset

extern void subsystem_init(struct subsystem *);
extern int subsystem_register(struct subsystem *);
extern void subsystem_unregister(struct subsystem *);

static inline struct subsystem * subsys_get(struct subsystem * s)
{
	return s ? container_of(kset_get(&s->kset),struct subsystem,kset) : NULL;
}

static inline void subsys_put(struct subsystem * s)
{
	kset_put(&s->kset);
}

struct subsys_attribute {
	struct attribute attr;
	ssize_t (*show)(struct subsystem *, char *);
	ssize_t (*store)(struct subsystem *, const char *, size_t); 
};

extern int subsys_create_file(struct subsystem * , struct subsys_attribute *);
extern void subsys_remove_file(struct subsystem * , struct subsys_attribute *);

#ifdef CONFIG_HOTPLUG
void kobject_hotplug(struct kobject *kobj, enum kobject_action action);
int add_hotplug_env_var(char **envp, int num_envp, int *cur_index,
			char *buffer, int buffer_size, int *cur_len,
			const char *format, ...)
	__attribute__((format (printf, 7, 8)));
#else
static inline void kobject_hotplug(struct kobject *kobj, enum kobject_action action) { }
static inline int add_hotplug_env_var(char **envp, int num_envp, int *cur_index, 
				      char *buffer, int buffer_size, int *cur_len, 
				      const char *format, ...)
{ return 0; }
#endif

#endif /* __KERNEL__ */
#endif /* _KOBJECT_H_ */
