/*
 * High memory handling common code and variables.
 *
 * (C) 1999 Andrea Arcangeli, SuSE GmbH, andrea@suse.de
 *          Gerhard Wichert, Siemens AG, Gerhard.Wichert@pdb.siemens.de
 *
 *
 * Redesigned the x86 32-bit VM architecture to deal with
 * 64-bit physical space. With current x86 CPUs this
 * means up to 64 Gigabytes physical RAM.
 *
 * Rewrote high memory support to move the page cache into
 * high memory. Implemented permanent (schedulable) kmaps
 * based on Linus' idea.
 *
 * Copyright (C) 1999 Ingo Molnar <mingo@redhat.com>
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/swap.h>
#include <linux/bio.h>
#include <linux/pagemap.h>
#include <linux/mempool.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/hash.h>
#include <linux/highmem.h>
#include <asm/tlbflush.h>

static mempool_t *page_pool, *isa_page_pool;

static void *page_pool_alloc(int gfp_mask, void *data)
{
	int gfp = gfp_mask | (int) (long) data;

	return alloc_page(gfp);
}

static void page_pool_free(void *page, void *data)
{
	__free_page(page);
}

/*
 * Virtual_count is not a pure "count".
 *  0 means that it is not mapped, and has not been mapped
 *    since a TLB flush - it is usable.
 *  1 means that there are no users, but it has been mapped
 *    since the last TLB flush - so we can't use it.
 *  n means that there are (n-1) current users of it.
 */
#ifdef CONFIG_HIGHMEM
/**
 * Pkmap_count数组包含LAST_PKMAP个计数器，pkmap_page_table页表中每一项都有一个。
 * 它记录了永久内核映射使用了哪些页表项。
 * 它的值可能为：
 *	0：对应的页表项没有映射任何高端内存页框，并且是可用的。
 *	1：对应页表项没有映射任何高端内存，但是它仍然不可用。因为自从它最后一次使用以来，相应的TLB表还没有被刷新。
 *	>1：相应的页表项映射了一个高端内存页框。并且正好有n-1个内核正在使用这个页框。
 */
static int pkmap_count[LAST_PKMAP];
static unsigned int last_pkmap_nr;
static  __cacheline_aligned_in_smp DEFINE_SPINLOCK(kmap_lock);

/**
 * 用于建立永久内核映射的页表。
 * 这样，内核可以长期映射高端内存到内核地址空间中。
 * 页表中的表项数由LAST_PKMAP宏产生，取决于是否打开PAE，它的值可能是512或者1024，
 * 这样可能映射2MB或4MB的永久内核映射。
 */
pte_t * pkmap_page_table;

static DECLARE_WAIT_QUEUE_HEAD(pkmap_map_wait);

static void flush_all_zero_pkmaps(void)
{
	int i;

	flush_cache_kmaps();

	for (i = 0; i < LAST_PKMAP; i++) {
		struct page *page;

		/*
		 * zero means we don't have anything to do,
		 * >1 means that it is still in use. Only
		 * a count of 1 means that it is free but
		 * needs to be unmapped
		 */
		if (pkmap_count[i] != 1)
			continue;
		pkmap_count[i] = 0;

		/* sanity check */
		if (pte_none(pkmap_page_table[i]))
			BUG();

		/*
		 * Don't need an atomic fetch-and-clear op here;
		 * no-one has the page mapped, and cannot get at
		 * its virtual address (and hence PTE) without first
		 * getting the kmap_lock (which is held here).
		 * So no dangers, even with speculative execution.
		 */
		page = pte_page(pkmap_page_table[i]);
		pte_clear(&pkmap_page_table[i]);

		set_page_address(page, NULL);
	}
	flush_tlb_kernel_range(PKMAP_ADDR(0), PKMAP_ADDR(LAST_PKMAP));
}

/**
 * 为建立永久内核映射建立初始映射.
 */
static inline unsigned long map_new_virtual(struct page *page)
{
	unsigned long vaddr;
	int count;

start:
	count = LAST_PKMAP;
	/* Find an empty entry */
	/**
	 * 扫描pkmap_count中的所有计数器值,直到找到一个空值.
	 */
	for (;;) {
		/**
		 * 从上次结束的地方开始搜索.
		 */
		last_pkmap_nr = (last_pkmap_nr + 1) & LAST_PKMAP_MASK;
		/**
		 * 搜索到最后一位了.在从0开始搜索前,刷新计数为1的项.
		 * 当计数值为1表示页表项可用,但是对应的TLB还没有刷新.
		 */
		if (!last_pkmap_nr) {
			flush_all_zero_pkmaps();
			count = LAST_PKMAP;
		}
		/**
		 * 找到计数为0的页表项,表示该页空闲且可用.
		 */
		if (!pkmap_count[last_pkmap_nr])
			break;	/* Found a usable entry */
		/**
		 * count是允许的搜索次数.如果还允许继续搜索下一个页表项.则继续,否则表示没有空闲项,退出.
		 */
		if (--count)
			continue;

		/*
		 * Sleep for somebody else to unmap their entries
		 */
		/**
		 * 运行到这里,表示没有找到空闲页表项.先睡眠一下.
		 * 等待其他线程释放页表项,然后唤醒本线程.
		 */
		{
			DECLARE_WAITQUEUE(wait, current);

			__set_current_state(TASK_UNINTERRUPTIBLE);
			/**
			 * 将当前线程挂到pkmap_map_wait等待队列上.
			 */
			add_wait_queue(&pkmap_map_wait, &wait);
			spin_unlock(&kmap_lock);
			schedule();
			remove_wait_queue(&pkmap_map_wait, &wait);
			spin_lock(&kmap_lock);

			/* Somebody else might have mapped it while we slept */
			/**
			 * 在当前线程等待的过程中,其他线程可能已经将页面进行了映射.
			 * 检测一下,如果已经映射了,就退出.
			 * 注意,这里没有对kmap_lock进行解锁操作.关于kmap_lock锁的操作,需要结合kmap_high来分析.
			 * 总的原则是:进入本函数时保证关锁,然后在本句前面关锁,本句后面解锁.
			 * 在函数返回后,锁仍然是关的.则外层解锁.
			 * 即使在本函数中循环也是这样.
			 * 内核就是这么乱,看久了就习惯了.不过你目前可能必须得学着适应这种代码.
			 */
			if (page_address(page))
				return (unsigned long)page_address(page);

			/* Re-start */
			goto start;
		}
	}
	/**
	 * 不管何种路径运行到这里来,kmap_lock都是锁着的.
	 * 并且last_pkmap_nr对应的是一个空闲且可用的表项.
	 */
	vaddr = PKMAP_ADDR(last_pkmap_nr);
	/**
	 * 设置页表属性,建立虚拟地址和物理地址之间的映射.
	 */
	set_pte(&(pkmap_page_table[last_pkmap_nr]), mk_pte(page, kmap_prot));

	/**
	 * 1表示相应的项可用,但是TLB需要刷新.
	 * 但是我们这里明明建立了映射,为什么还是可用的呢,其他地方不会将占用么
