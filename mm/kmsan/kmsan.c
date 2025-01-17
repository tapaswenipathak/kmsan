/*
 * KMSAN runtime library.
 *
 * Copyright (C) 2017-2019 Google LLC
 * Author: Alexander Potapenko <glider@google.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <asm/page.h>
#include <linux/compiler.h>
#include <linux/export.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/kmsan.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/preempt.h>
#include <linux/percpu-defs.h>
#include <linux/mm_types.h>
#include <linux/slab.h>
#include <linux/stackdepot.h>
#include <linux/stacktrace.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

#include <linux/mmzone.h>

#include "../slab.h"
#include "kmsan.h"

/*
 * Some kernel asm() calls mention the non-existing |__force_order| variable
 * in the asm constraints to preserve the order of accesses to control
 * registers. KMSAN turns those mentions into actual memory accesses, therefore
 * the variable is now required to link the kernel.
 */
unsigned long __force_order;

extern char __irqentry_text_end[];
extern char __irqentry_text_start[];
extern char __softirqentry_text_end[];
extern char __softirqentry_text_start[];

bool kmsan_ready = false;
#define KMSAN_STACK_DEPTH 64
#define MAX_CHAIN_DEPTH 7

/*
 * According to Documentation/x86/kernel-stacks, kernel code can run on the
 * following stacks:
 * - regular task stack - when executing the task code
 *  - interrupt stack - when handling external hardware interrupts and softirqs
 *  - NMI stack
 * 0 is for regular interrupts, 1 for softirqs, 2 for NMI.
 * Because interrupts may nest, trying to use a new context for every new interrupt.
 */
/* [0] for dummy per-CPU context. */
DEFINE_PER_CPU(kmsan_context_state[KMSAN_NESTED_CONTEXT_MAX], kmsan_percpu_cstate);
/* 0 for task context, |i>0| for kmsan_context_state[i]. */
DEFINE_PER_CPU(int, kmsan_context_level);
DEFINE_PER_CPU(int, kmsan_in_interrupt);
DEFINE_PER_CPU(bool, kmsan_in_softirq);
DEFINE_PER_CPU(bool, kmsan_in_nmi);
DEFINE_PER_CPU(int, kmsan_in_runtime);
DEFINE_PER_CPU(unsigned long, kmsan_runtime_last_caller);  // TODO(glider): debug-only

kmsan_context_state *task_kmsan_context_state(void)
{
	int cpu = smp_processor_id();
	int level = this_cpu_read(kmsan_context_level);
	kmsan_context_state *ret;

	if (!kmsan_ready || IN_RUNTIME()) {
		ret = &per_cpu(kmsan_percpu_cstate[0], cpu);
		__memset(ret, 0, sizeof(kmsan_context_state));
		return ret;
	}

	if (!level)
		ret = &current->kmsan.cstate;
	else
		ret = &per_cpu(kmsan_percpu_cstate[level], cpu);
	return ret;
}

void inline do_kmsan_task_create(struct task_struct *task)
{
	kmsan_task_state *state = &task->kmsan;

	__memset(state, 0, sizeof(kmsan_task_state));
	state->enabled = true;
	state->allow_reporting = true;
	state->is_reporting = false;
}

inline void kmsan_internal_memset_shadow(void *addr, int b, size_t size, bool checked)
{
	void *shadow_start;
	u64 page_offset, address = (u64)addr;
	size_t to_fill;

	BUG_ON(!metadata_is_contiguous(addr, size, /*is_origin*/false));
	while (size) {
		page_offset = address % PAGE_SIZE;
		to_fill = min(PAGE_SIZE - page_offset, (u64)size);
		shadow_start = kmsan_get_metadata_or_null((void *)address, to_fill, /*is_origin*/false);
		if (!shadow_start) {
			if (checked) {
				current->kmsan.is_reporting = true;
				kmsan_pr_err("WARNING: not memsetting %d bytes starting at %px, because the shadow is NULL\n", to_fill, address);
				current->kmsan.is_reporting = false;
				BUG();
			}
			/* Otherwise just move on. */
		} else {
			__memset(shadow_start, b, to_fill);
		}
		address += to_fill;
		size -= to_fill;
	}
}

void kmsan_internal_poison_shadow(void *address, size_t size,
				gfp_t flags, unsigned int poison_flags)
{
	bool checked = poison_flags & KMSAN_POISON_CHECK;
	depot_stack_handle_t handle;

	kmsan_internal_memset_shadow(address, -1, size, checked);
	handle = kmsan_save_stack_with_flags(flags);
	kmsan_set_origin(address, size, handle, checked);
}

void kmsan_internal_unpoison_shadow(void *address, size_t size, bool checked)
{
	kmsan_internal_memset_shadow(address, 0, size, checked);
	kmsan_set_origin(address, size, 0, checked);
}

static inline int in_irqentry_text(unsigned long ptr)
{
	return (ptr >= (unsigned long)&__irqentry_text_start &&
		ptr < (unsigned long)&__irqentry_text_end) ||
		(ptr >= (unsigned long)&__softirqentry_text_start &&
		 ptr < (unsigned long)&__softirqentry_text_end);
}

/* TODO(glider): this function is shared with KASAN. */
static inline unsigned int filter_irq_stacks(unsigned long *entries,
					     unsigned int nr_entries)
{
	unsigned int i;

	for (i = 0; i < nr_entries; i++) {
		if (in_irqentry_text(entries[i])) {
			/* Include the irqentry function into the stack. */
			return i + 1;
		}
	}
	return nr_entries;
}

/* static */
inline depot_stack_handle_t kmsan_save_stack_with_flags(gfp_t flags)
{
	depot_stack_handle_t handle;
	unsigned long entries[KMSAN_STACK_DEPTH];
	unsigned int nr_entries;

	nr_entries = stack_trace_save(entries, KMSAN_STACK_DEPTH, 0);
	filter_irq_stacks(entries, nr_entries);

	/* Don't sleep (see might_sleep_if() in __alloc_pages_nodemask()). */
	flags &= ~__GFP_DIRECT_RECLAIM;

	handle = stack_depot_save(entries, nr_entries, flags);
	return handle;
}

/*
 * As with the regular memmove, do the following:
 * - if src and dst don't overlap, use memcpy;
 * - if src and dst overlap:
 *   - if src > dst, use memcpy;
 *   - if src < dst, use reverse-memcpy.
 * Why this is correct:
 * - problems may arise if for some part of the overlapping region we
 *   overwrite its shadow with a new value before copying it somewhere.
 *   But there's a 1:1 mapping between the kernel memory and its shadow,
 *   therefore if this doesn't happen with the kernel memory it can't happen
 *   with the shadow.
 */
inline
void kmsan_memcpy_memmove_metadata(void *dst, void *src, size_t n, bool is_memmove)
{
	void *shadow_src, *shadow_dst;
	depot_stack_handle_t *origin_src, *origin_dst;
	int src_slots, dst_slots, i, iter, step;
	depot_stack_handle_t prev_origin = 0, chained_origin, new_origin = 0;
	u32 *align_shadow_src, shadow;
	bool backwards;

	BUG_ON(!metadata_is_contiguous(dst, n, /*is_origin*/false));
	BUG_ON(!metadata_is_contiguous(src, n, /*is_origin*/false));

	shadow_dst = kmsan_get_metadata_or_null(dst, n, /*is_origin*/false);
	if (!shadow_dst)
		return;

	shadow_src = kmsan_get_metadata_or_null(src, n, /*is_origin*/false);
	if (!shadow_src) {
		/* |src| is untracked: zero out destination shadow, ignore the origins. */
		__memset(shadow_dst, 0, n);
		return;
	}
	if (is_memmove)
		__memmove(shadow_dst, shadow_src, n);
	else
		__memcpy(shadow_dst, shadow_src, n);

	origin_dst = kmsan_get_metadata_or_null(dst, n, /*is_origin*/true);
	origin_src = kmsan_get_metadata_or_null(src, n, /*is_origin*/true);
	BUG_ON(!origin_dst || !origin_src);
	BUG_ON(!metadata_is_contiguous(dst, n, /*is_origin*/true));
	BUG_ON(!metadata_is_contiguous(src, n, /*is_origin*/true));
	src_slots = (ALIGN((u64)src + n, ORIGIN_SIZE) - ALIGN_DOWN((u64)src, ORIGIN_SIZE)) / ORIGIN_SIZE;
	dst_slots = (ALIGN((u64)dst + n, ORIGIN_SIZE) - ALIGN_DOWN((u64)dst, ORIGIN_SIZE)) / ORIGIN_SIZE;
	BUG_ON(!src_slots || !dst_slots);
	BUG_ON((src_slots < 1) || (dst_slots < 1));
	BUG_ON((src_slots - dst_slots > 1) || (dst_slots - src_slots < -1));

	backwards = is_memmove && (dst > src);
	i = backwards ? min(src_slots, dst_slots) - 1 : 0;
	iter = backwards ? -1 : 1;

	align_shadow_src = (u32*)ALIGN_DOWN((u64)shadow_src, ORIGIN_SIZE);
	for (step = 0; step < min(src_slots, dst_slots); step++,i+=iter) {
		BUG_ON(i < 0);
		shadow = align_shadow_src[i];
		if (i == 0)
			/*
			 * If |src| isn't aligned on ORIGIN_SIZE, don't
			 * look at the first |src % ORIGIN_SIZE| bytes
			 * of the first shadow slot.
			 */
			shadow = (shadow << ((u64)src % ORIGIN_SIZE)) >> ((u64)src % ORIGIN_SIZE);
		if (i == src_slots - 1)
			/*
			 * If |src + n| isn't aligned on
			 * ORIGIN_SIZE, don't look at the last
			 * |(src + n) % ORIGIN_SIZE| bytes of the
			 * last shadow slot.
			 */
			shadow = (shadow >> (((u64)src + n) % ORIGIN_SIZE)) >> (((u64)src + n) % ORIGIN_SIZE); // TODO
		/*
		 * Overwrite the origin only if the corresponding
		 * shadow is nonempty.
		 */
		if (origin_src[i] && (origin_src[i] != prev_origin) && shadow) {
			prev_origin = origin_src[i];
			chained_origin = kmsan_internal_chain_origin(prev_origin);
			/*
			 * kmsan_internal_chain_origin() may return
			 * NULL, but we don't want to lose the previous
			 * origin value.
			 */
			if (chained_origin)
				new_origin = chained_origin;
			else
				new_origin = prev_origin;
		}
		if (shadow)
			origin_dst[i] = new_origin;
		else
			origin_dst[i] = 0;
	}
}

void kmsan_memcpy_metadata(void *dst, void *src, size_t n)
{
	kmsan_memcpy_memmove_metadata(dst, src, n, /*is_memmove*/false);
}

void kmsan_memmove_metadata(void *dst, void *src, size_t n)
{
	kmsan_memcpy_memmove_metadata(dst, src, n, /*is_memmove*/true);
}

depot_stack_handle_t inline kmsan_internal_chain_origin(depot_stack_handle_t id)
{
	depot_stack_handle_t handle;
	unsigned long entries[3], *old_entries;
	unsigned int nr_old_entries;
	u64 magic = KMSAN_CHAIN_MAGIC_ORIGIN_FULL;
	int depth = 0;
	u64 old_magic;
	static int skipped = 0;

	if (!kmsan_ready)
		return 0;

	if (!id) return id;

	/*
	 * TODO(glider): this is slower, but will save us a lot of memory.
	 * Let us store the chain length in the lowest byte of the magic.
	 * Maybe we can cache the ids somehow to avoid fetching them?
	 */
	nr_old_entries = stack_depot_fetch(id, &old_entries);
	if (!nr_old_entries)
		return id;
	old_magic = old_entries[0];
	if ((old_magic & KMSAN_MAGIC_MASK) == KMSAN_CHAIN_MAGIC_ORIGIN_FULL) {
		depth = old_magic & 0xff;
	}
	if (depth >= MAX_CHAIN_DEPTH) {
		skipped++;
		if (skipped % 10000 == 0) {
			kmsan_pr_err("not chained %d origins\n", skipped);
			dump_stack();
			kmsan_print_origin(id);
		}
		return id;
	}
	depth++;
	/* TODO(glider): how do we figure out we've dropped some frames? */
	entries[0] = magic + depth;
	entries[1] = kmsan_save_stack_with_flags(GFP_ATOMIC);
	entries[2] = id;
	handle = stack_depot_save(entries, ARRAY_SIZE(entries), GFP_ATOMIC);
	return handle;
}

inline
void kmsan_write_aligned_origin(void *var, size_t size, u32 origin)
{
	u32 *var_cast = (u32 *)var;
	int i;

	BUG_ON((u64)var_cast % ORIGIN_SIZE);
	BUG_ON(size % ORIGIN_SIZE);
	for (i = 0; i < size / ORIGIN_SIZE; i++)
		var_cast[i] = origin;
}

/*
 * TODO(glider): writing an initialized byte shouldn't zero out the origin, if
 * the remaining three bytes are uninitialized.
 */
void kmsan_set_origin(void *addr, int size, u32 origin, bool checked)
{
	void *origin_start;
	u64 address = (u64)addr, page_offset;
	size_t to_fill, pad = 0;

	if (!IS_ALIGNED(address, ORIGIN_SIZE)) {
		pad = address % ORIGIN_SIZE;
		address -= pad;
		size += pad;
	}

	while (size > 0) {
		page_offset = address % PAGE_SIZE;
		to_fill = min(PAGE_SIZE - page_offset, (u64)size);
		to_fill = ALIGN(to_fill, ORIGIN_SIZE);
		BUG_ON(!to_fill);
		origin_start = kmsan_get_metadata_or_null((void *)address, to_fill, /*origin*/true);
		if (!origin_start) {
			if (checked) {
				current->kmsan.is_reporting = true;
				kmsan_pr_err("WARNING: not setting origing for %d bytes starting at %px, because the origin is NULL\n", to_fill, address);
				current->kmsan.is_reporting = false;
				BUG();
			}
		} else {
			kmsan_write_aligned_origin(origin_start, to_fill, origin);
		}
		address += to_fill;
		size -= to_fill;
	}
}

struct page *vmalloc_to_page_or_null(void *vaddr)
{
	struct page *page;

	if (!_is_vmalloc_addr(vaddr) && !is_module_addr(vaddr))
		return NULL;
	page = vmalloc_to_page(vaddr);
	if (pfn_valid(page_to_pfn(page)))
		return page;
	else
		return NULL;
}

void kmsan_internal_check_memory(void *addr, size_t size, const void *user_addr, int reason)
{
	unsigned long irq_flags;
	unsigned long addr64 = (unsigned long)addr;
	unsigned char *shadow = NULL;
	depot_stack_handle_t *origin = NULL;
	depot_stack_handle_t cur_origin = 0, new_origin = 0;
	int cur_off_start = -1;
	int i, chunk_size;
	size_t pos = 0;

	BUG_ON(!metadata_is_contiguous(addr, size, /*is_origin*/false));
	if (size <= 0)
		return;
	while (pos < size) {
		chunk_size = min(size - pos, PAGE_SIZE - ((addr64 + pos) % PAGE_SIZE));
		shadow = kmsan_get_metadata_or_null((void *)(addr64 + pos), chunk_size, /*is_origin*/false);
		if (!shadow) {
			/*
			 * This page is untracked. TODO(glider): assert.
			 * If there were uninitialized bytes before, report them.
			 */
			if (cur_origin) {
				ENTER_RUNTIME(irq_flags);
				kmsan_report(cur_origin, addr, size, cur_off_start, pos - 1, user_addr, /*deep*/true, reason);
				LEAVE_RUNTIME(irq_flags);
			}
			cur_origin = 0;
			cur_off_start = -1;
			pos += chunk_size;
			continue;
		}
		for (i = 0; i < chunk_size; i++) {
			if (!shadow[i]) {
				/*
				 * This byte is unpoisoned. If there were
				 * poisoned bytes before, report them.
				 */
				if (cur_origin) {
					ENTER_RUNTIME(irq_flags);
					kmsan_report(cur_origin, addr, size, cur_off_start, pos + i - 1, user_addr, /*deep*/true, reason);
					LEAVE_RUNTIME(irq_flags);
				}
				cur_origin = 0;
				cur_off_start = -1;
				continue;
			}
			origin = kmsan_get_metadata_or_null((void *)(addr64 + pos + i), chunk_size - i, /*is_origin*/true);
			BUG_ON(!origin);
			new_origin = *origin;
			/*
			 * Encountered new origin - report the previous
			 * uninitialized range.
			 */
			if (cur_origin != new_origin) {
				if (cur_origin) {
					ENTER_RUNTIME(irq_flags);
					kmsan_report(cur_origin, addr, size, cur_off_start, pos + i - 1, user_addr, /*deep*/true, reason);
					LEAVE_RUNTIME(irq_flags);
				}
				cur_origin = new_origin;
				cur_off_start = pos + i;
			}
		}
		pos += chunk_size;
	}
	BUG_ON(pos != size);
	if (cur_origin) {
		ENTER_RUNTIME(irq_flags);
		kmsan_report(cur_origin, addr, size, cur_off_start, pos - 1, user_addr, /*deep*/true, reason);
		LEAVE_RUNTIME(irq_flags);
	}
}

/*
 * TODO(glider): this check shouldn't be performed for origin pages, because
 * they're always accessed after the shadow pages.
 * TODO(glider): call this check kmsan_get_metadata_or_null().
 */
bool metadata_is_contiguous(void *addr, size_t size, bool is_origin) {
	u64 cur_addr = (u64)addr, next_addr;
	char *cur_meta = NULL, *next_meta = NULL;
	depot_stack_handle_t *origin_p;
	bool all_untracked = false;
	const char *fname = is_origin ? "origin" : "shadow";

	if (!size)
		return true;

	/* The whole range belongs to the same page. */
	if (ALIGN_DOWN(cur_addr + size - 1, PAGE_SIZE) == ALIGN_DOWN(cur_addr, PAGE_SIZE))
		return true;
	cur_meta = kmsan_get_metadata_or_null((void *)cur_addr, 1, is_origin);
	if (!cur_meta)
		all_untracked = true;
	for (next_addr = cur_addr + PAGE_SIZE; next_addr < (u64)addr + size;
			cur_addr = next_addr, cur_meta = next_meta, next_addr += PAGE_SIZE) {
		next_meta = kmsan_get_metadata_or_null((void *)next_addr, 1, is_origin);
		if (!next_meta) {
			if (!all_untracked)
				goto report;
			continue;
		}
		if ((u64)cur_meta == ((u64)next_meta - PAGE_SIZE))
			continue;
		goto report;
	}
	return true;

report:
	current->kmsan.is_reporting = true;
	kmsan_pr_err("BUG: attempting to access two shadow page ranges.\n");
	dump_stack();
	kmsan_pr_err("\n");
	kmsan_pr_err("Access of size %d at %px.\n", size, addr);
	kmsan_pr_err("Addresses belonging to different ranges are: %px and %px\n", cur_addr, next_addr);
	kmsan_pr_err("page[0].%s: %px, page[1].%s: %px\n", fname, cur_meta, fname, next_meta);
	origin_p = kmsan_get_metadata_or_null(addr, 1, /*is_origin*/true);
	if (origin_p) {
		kmsan_pr_err("Origin: %px\n", *origin_p);
		kmsan_print_origin(*origin_p);
	} else {
		kmsan_pr_err("Origin: unavailable\n");
	}
	current->kmsan.is_reporting = false;
	return false;
}


