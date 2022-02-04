/*
 * device.h - generic, centralized driver model
 *
 * Copyright (c) 2001-2003 Patrick Mochel <mochel@osdl.org>
 *
 * This file is released under the GPLv2
 *
 * See Documentation/driver-model/ for more information.
 */

#ifndef _DEVICE_H_
#define _DEVICE_H_

#include <linux/config.h>
#include <linux/ioport.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <asm/semaphore.h>
#include <asm/atomic.h>

#define DEVICE_NAME_SIZE	50
#define DEVICE_NAME_HALF	__stringify(20)	/* Less than half to accommodate slop */
#define DEVICE_ID_SIZE		32
#define BUS_ID_SIZE		KOBJ_NAME_LEN


enum {
	SUSPEND_NOTIFY,
	SUSPEND_SAVE_STATE,
	SUSPEND_DISABLE,
	SUSPEND_POWER_DOWN,
};

enum {
	RESUME_POWER_ON,
	RESUME_RESTORE_STATE,
	RESUME_ENABLE,
};

struct device;
struct device_driver;
struct class;
struct class_device;
struct class_simple;

/**
 * 总线类型
 *
 * 每个bus_type对象都包含一个内嵌的子系统，bus_subsys把所有嵌入在bus_type对象中的子系统集合在一起。
 * bus_subsys子系统对应的目录为/sys/bus。
 */
struct bus_type {
	char			* name;		/* 总线类型的名称。例如"pci" */

	struct subsystem	subsys;		/* 与总线类型相关的kobject子系统 */
	struct kset		drivers;	/* 驱动程序的kobject集合 */
	struct kset		devices;	/* 设备的kobject集合。
						 * devices目录存放了指向/sys/devices下目录的符号链接。 */

	struct bus_attribute	* bus_attrs;	/* 指向对象的指针，该对象包含总线属性和用于导出此属性到sysfs文件系统的方法 */
	struct device_attribute	* dev_attrs;	/* 指向对象的指针，该对象包含设备属性和用于导出此属性到sysfs文件系统的方法 */
	struct driver_attribute	* drv_attrs;	/* 指向对象的指针，该对象包含驱动程序属性和用于导出此属性到sysfs文件系统的方法 */

	/* 检验给定的设备驱动程序是否支持特定设备的方法 */
	int		(*match)(struct device * dev, struct device_driver * drv);
	/* 注册设备时调用的方法 */
	int		(*hotplug) (struct device *dev, char **envp, 
				    int num_envp, char *buffer, int buffer_size);
	/* 保存硬件设备的上下文状态并改变设备供电状态的方法 */
	int		(*suspend)(struct device * dev, pm_message_t state);
	/* 改变供电状态和恢复硬件设备上下文的方法 */
	int		(*resume)(struct device * dev);
};

extern int bus_register(struct bus_type * bus);
extern void bus_unregister(struct bus_type * bus);

extern int bus_rescan_devices(struct bus_type * bus);

extern struct bus_type * get_bus(struct bus_type * bus);
extern void put_bus(struct bus_type * bus);

extern struct bus_type * find_bus(char * name);

/* iterator helpers for buses */

int bus_for_each_dev(struct bus_type * bus, struct device * start, void * data,
		     int (*fn)(struct device *, void *));

int bus_for_each_drv(struct bus_type * bus, struct device_driver * start, 
		     void * data, int (*fn)(struct device_driver *, void *));


/* driverfs interface for exporting bus attributes */
/**
 * 总线属性。
 */
struct bus_attribute {
	struct attribute	attr;
	ssize_t (*show)(struct bus_type *, char * buf);
	ssize_t (*store)(struct bus_type *, const char * buf, size_t count);
};

/**
 * 定义一个总线属性。
 */
#define BUS_ATTR(_name,_mode,_show,_store)	\
struct bus_attribute bus_attr_##_name = __ATTR(_name,_mode,_show,_store)

extern int bus_create_file(struct bus_type *, struct bus_attribute *);
extern void bus_remove_file(struct bus_type *, struct bus_attribute *);

/**
 * 设备驱动程序模型中的驱动程序
 *
 * device_driver对象的几个方法用于处理热插拔、即插即用和电源管理。
 * 通常，device_driver对象被静态地嵌入到一个更大的描述符中，如：pci_driver。
 */
struct device_driver {
	char			* name;		/* 驱动程序的名称 */
	struct bus_type		* bus;		/* 指向总线描述符的指针 */

	struct semaphore	unload_sem;	/* 禁止卸载设备驱动程序的信号量。
						 * 当引用计数为0时释放该信号量 */
	struct kobject		kobj;		/* 内嵌的kobject */
	struct list_head	devices;	/* 驱动程序所支持的所有设备组成的链表的头 */

	struct module 		* owner;	/* 实现驱动程序的模块（如果有的话） */

	/* 探测设备的方法（检测设备驱动程序是否可以控制该设备） */
	int	(*probe)	(struct device * dev);
	/* 移走设备时调用的方法 */
	int 	(*remove)	(struct device * dev);
	/* 设备断电时调用的方法。 */
	void	(*shutdown)	(struct device * dev);
	/* 设备置于低功率状态时所调用的方法 */
	int	(*suspend)	(struct device * dev, u32 state, u32 level);
	/* 设备恢复正常状态时所调用的方法 */
	int	(*resume)	(struct device * dev, u32 level);
};


extern int driver_register(struct device_driver * drv);
extern void driver_unregister(struct device_driver * drv);

extern struct device_driver * get_driver(struct device_driver * drv);
extern void put_driver(struct device_driver * drv);
extern struct device_driver *driver_find(const char *name, struct bus_type *bus);


/* driverfs interface for exporting driver attributes */
/**
 * 驱动程序属性。
 */
struct driver_attribute {
	struct attribute	attr;
	ssize_t (*show)(struct device_driver *, char * buf);
	ssize_t (*store)(struct device_driver *, const char * buf, size_t count);
};

/**
 * 定义一个驱动程序属性。
 */
#define DRIVER_ATTR(_name,_mode,_show,_store)	\
struct driver_attribute driver_attr_##_name = __ATTR(_name,_mode,_show,_store)

extern int driver_create_file(struct device_driver *, struct driver_attribute *);
extern void driver_remove_file(struct device_driver *, struct driver_attribute *);


/**
 * device classes
 *
 * 每个class对象都包含一个内嵌的子系统，class_subsys把所有嵌入在class对象中的子系统集合在一起。
 * class_subsys子系统对应的目录为/sys/class。
 */
struct class {
	char			* name;		/* 类名称 */

	struct subsystem	subsys;		/* 与设备类型相关的kobject子系统 */
	struct list_head	children;
	struct list_head	interfaces;

	struct class_attribute		* class_attrs;		/* 一个类被注册后，将创建该字段指向的数组中的所有属性 */
	struct class_device_attribute	* class_dev_attrs;	/* 该类添加的设备的默认属性 */

	/* 设备热插拨时，调用此回调函数为应用程序创建环境变量 */
	int	(*hotplug)(struct class_device *dev, char **envp, 
			   int num_envp, char *buffer, int buffer_size);

	/* 把设备从类中删除时，调用release方法 */
	void	(*release)(struct class_device *dev);
	/* 当类被释放时，调用此方法 */
	void	(*class_release)(struct class *class);
};

extern int class_register(struct class *);
extern void class_unregister(struct class *);

extern struct class * class_get(struct class *);
extern void class_put(struct class *);


struct class_attribute {
	struct attribute	attr;
	ssize_t (*show)(struct class *, char * buf);
	ssize_t (*store)(struct class *, const char * buf, size_t count);
};

#define CLASS_ATTR(_name,_mode,_show,_store)			\
struct class_attribute class_attr_##_name = __ATTR(_name,_mode,_show,_store) 

extern int class_create_file(struct class *, const struct class_attribute *);
extern void class_remove_file(struct class *, const struct class_attribute *);

/**
 * 按类型的逻辑设备，多个class_device对象可以表示同一设备。
 */
struct class_device {
	struct list_head	node;

	struct kobject		kobj;
	struct class		* class;	/* required */
	struct device		* dev;		/* not necessary, but nice to have */
	void			* class_data;	/* class-specific data */

	char	class_id[BUS_ID_SIZE];	/* unique to this class */
};

static inline void *
class_get_devdata (struct class_device *dev)
{
	return dev->class_data;
}

static inline void
class_set_devdata (struct class_device *dev, void *data)
{
	dev->class_data = data;
}


extern int class_device_register(struct class_device *);
extern void class_device_unregister(struct class_device *);
extern void class_device_initialize(struct class_device *);
extern int class_device_add(struct class_device *);
extern void class_device_del(struct class_device *);

extern int class_device_rename(struct class_device *, char *);

extern struct class_device * class_device_get(struct class_device *);
extern void class_device_put(struct class_device *);

struct class_device_attribute {
	struct attribute	attr;
	ssize_t (*show)(struct class_device *, char * buf);
	ssize_t (*store)(struct class_device *, const char * buf, size_t count);
};

#define CLASS_DEVICE_ATTR(_name,_mode,_show,_store)		\
struct class_device_attribute class_device_attr_##_name = 	\
	__ATTR(_name,_mode,_show,_store)

extern int class_device_create_file(struct class_device *, 
				    const struct class_device_attribute *);
extern void class_device_remove_file(struct class_device *, 
				     const struct class_device_attribute *);
extern int class_device_create_bin_file(struct class_device *,
					struct bin_attribute *);
extern void class_device_remove_bin_file(struct class_device *,
					 struct bin_attribute *);

struct class_interface {
	struct list_head	node;
	struct class		*class;

	int (*add)	(struct class_device *);
	void (*remove)	(struct class_device *);
};

extern int class_interface_register(struct class_interface *);
extern void class_interface_unregister(struct class_interface *);

/* interface for class simple stuff */
extern struct class_simple *class_simple_create(struct module *owner, char *name);
extern void class_simple_destroy(struct class_simple *cs);
extern struct class_device *class_simple_device_add(struct class_simple *cs, dev_t dev, struct device *device, const char *fmt, ...)
	__attribute__((format(printf,4,5)));
extern int class_simple_set_hotplug(struct class_simple *, 
	int (*hotplug)(struct class_device *dev, char **envp, int num_envp, char *buffer, int buffer_size));
extern void class_simple_device_remove(dev_t dev);


/**
 * 设备驱动程序模型中的设备
 *
 * device对象全部收集在devices_subsys子系统中，该子系统对应的目录为/sys/devices。
 * 通常，device对象被静态地嵌入到一个更大的描述符中，如：pci_dev。
 */
struct device {
	struct list_head node;		/* node in sibling list */
	struct list_head bus_list;	/* node in bus's list */
	struct list_head driver_list;	/* 驱动程序对象的设备（devices）链表的结点 */
	struct list_head children;	/* 子设备链表的头 */
	struct device 	* parent;	/* 指向父设备的指针
					 * 父设备，即该设备所属的设备。子设备离开父设备无法正常工作。
					 * 大多数情况下，一个父设备通常是某种总线或者是宿主控制器。
					 * 如果parent为NULL，表示该设备是顶层设备。 */

	struct kobject kobj;		/* 内嵌的kobject
					 * 通过该字段将设备连接到结构体系中。
					 * 作为通用准则，device->kobj->parent和device->parent->kobj是相同的。 */
	char	bus_id[BUS_ID_SIZE];	/* position on parent bus
					 * 在总线上唯一标识该设备的字符串。
					 * 如PCI设备使用了标准PCI ID格式，它包括:域编号、总线编号、设备编号和功能编号。 */

	struct bus_type	* bus;		/* 指向所连接总线的指针
					 * type of bus device is on */
	struct device_driver *driver;	/* which driver has allocated this
					   device */
	void		*driver_data;	/* data private to the driver */
	
	void		*platform_data;	/* Platform specific data (e.g. ACPI,
					   BIOS data relevant to device) */
	struct dev_pm_info	power;	/* 电源管理信息 */

	u32		detach_state;	/* State to enter when device is
					   detached from its driver. */

	u64		*dma_mask;	/* dma mask (if dma'able device) */
	u64		coherent_dma_mask;/* Like dma_mask, but for
					     alloc_coherent mappings as
					     not all hardware supports
					     64 bit addresses for consistent
					     allocations such descriptors. */

	struct list_head	dma_pools;	/* dma pools (if dma'ble) */

	struct dma_coherent_mem	*dma_mem; /* internal for coherent mem
					     override */

	/* 释放设备描述符的回调函数 */
	void	(*release)(struct device * dev);
};

static inline struct device *
list_to_dev(struct list_head *node)
{
	return list_entry(node, struct device, node);
}

static inline void *
dev_get_drvdata (struct device *dev)
{
	return dev->driver_data;
}

static inline void
dev_set_drvdata (struct device *dev, void *data)
{
	dev->driver_data = data;
}

/*
 * High level routines for use by the bus drivers
 */
extern int device_register(struct device * dev);
extern void device_unregister(struct device * dev);
extern void device_initialize(struct device * dev);
extern int device_add(struct device * dev);
extern void device_del(struct device * dev);
extern int device_for_each_child(struct device *, void *,
		     int (*fn)(struct device *, void *));

/*
 * Manual binding of a device to driver. See drivers/base/bus.c
 * for information on use.
 */
extern int  driver_probe_device(struct device_driver * drv, struct device * dev);
extern void device_bind_driver(struct device * dev);
extern void device_release_driver(struct device * dev);
extern int  device_attach(struct device * dev);
extern void driver_attach(struct device_driver * drv);


/* driverfs interface for exporting device attributes */

/**
 * 设备属性。
 */
struct device_attribute {
	struct attribute	attr;
	ssize_t (*show)(struct device * dev, char * buf);
	ssize_t (*store)(struct device * dev, const char * buf, size_t count);
};

/**
 * 初始化一个设备属性。
 */
#define DEVICE_ATTR(_name,_mode,_show,_store) \
struct device_attribute dev_attr_##_name = __ATTR(_name,_mode,_show,_store)


extern int device_create_file(struct device *device, struct device_attribute * entry);
extern void device_remove_file(struct device * dev, struct device_attribute * attr);

/*
 * Platform "fixup" functions - allow the platform to have their say
 * about devices and actions that the general device layer doesn't
 * know about.
 */
/* Notify platform of device discovery */
extern int (*platform_notify)(struct device * dev);

extern int (*platform_notify_remove)(struct device * dev);


/**
 * get_device - atomically increment the reference count for the device.
 *
 */
extern struct device * get_device(struct device * dev);
extern void put_device(struct device * dev);
extern struct device *device_find(const char *name, struct bus_type *bus);


/* drivers/base/platform.c */

struct platform_device {
	char		* name;
	u32		id;
	struct device	dev;
	u32		num_resources;
	struct resource	* resource;
};

#define to_platform_device(x) container_of((x), struct platform_device, dev)

extern int platform_device_register(struct platform_device *);
extern void platform_device_unregister(struct platform_device *);

extern struct bus_type platform_bus_type;
extern struct device platform_bus;

extern struct resource *platform_get_resource(struct platform_device *, unsigned int, unsigned int);
extern int platform_get_irq(struct platform_device *, unsigned int);
extern struct resource *platform_get_resource_byname(struct platform_device *, unsigned int, char *);
extern int platform_get_irq_byname(struct platform_device *, char *);
extern int platform_add_devices(struct platform_device **, int);

extern struct platform_device *platform_device_register_simple(char *, unsigned int, struct resource *, unsigned int);

/* drivers/base/power.c */
extern void device_shutdown(void);


/* drivers/base/firmware.c */
extern int firmware_register(struct subsystem *);
extern void firmware_unregister(struct subsystem *);

/* debugging and troubleshooting/diagnostic helpers. */
#define dev_printk(level, dev, format, arg...)	\
	printk(level "%s %s: " format , (dev)->driver ? (dev)->driver->name : "" , (dev)->bus_id , ## arg)

#ifdef DEBUG
#define dev_dbg(dev, format, arg...)		\
	dev_printk(KERN_DEBUG , dev , format , ## arg)
#else
#define dev_dbg(dev, format, arg...) do { (void)(dev); } while (0)
#endif

#define dev_err(dev, format, arg...)		\
	dev_printk(KERN_ERR , dev , format , ## arg)
#define dev_info(dev, format, arg...)		\
	dev_printk(KERN_INFO , dev , format , ## arg)
#define dev_warn(dev, format, arg...)		\
	dev_printk(KERN_WARNING , dev , format , ## arg)

/* Create alias, so I can be autoloaded. */
#define MODULE_ALIAS_CHARDEV(major,minor) \
	MODULE_ALIAS("char-major-" __stringify(major) "-" __stringify(minor))
#define MODULE_ALIAS_CHARDEV_MAJOR(major) \
	MODULE_ALIAS("char-major-" __stringify(major) "-*")
#endif /* _DEVICE_H_ */
