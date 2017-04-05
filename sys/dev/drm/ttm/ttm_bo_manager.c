/**************************************************************************
 *
 * Copyright (c) 2007-2010 VMware, Inc., Palo Alto, CA., USA
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
/*
 * Authors: Thomas Hellstrom <thellstrom-at-vmware-dot-com>
 *
 * $FreeBSD: head/sys/dev/drm2/ttm/ttm_bo_manager.c 247835 2013-03-05 09:49:34Z kib $
 */

#include <drm/ttm/ttm_module.h>
#include <drm/ttm/ttm_bo_driver.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/drm_mm.h>
#include <linux/export.h>

/**
 * Currently we use a spinlock for the lock, but a mutex *may* be
 * more appropriate to reduce scheduling latency if the range manager
 * ends up with very fragmented allocation patterns.
 */

struct ttm_range_manager {
	struct drm_mm mm;
	struct lock lock;
};

static int ttm_bo_man_get_node(struct ttm_mem_type_manager *man,
			       struct ttm_buffer_object *bo,
			       const struct ttm_place *place,
			       struct ttm_mem_reg *mem)
{
	struct ttm_range_manager *rman = (struct ttm_range_manager *) man->priv;
	struct drm_mm *mm = &rman->mm;
	struct drm_mm_node *node = NULL;
	enum drm_mm_allocator_flags aflags = DRM_MM_CREATE_DEFAULT;
	unsigned long lpfn;
	int ret;

	lpfn = place->lpfn;
	if (!lpfn)
		lpfn = man->size;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	if (place->flags & TTM_PL_FLAG_TOPDOWN)
		aflags = DRM_MM_CREATE_TOP;

	lockmgr(&rman->lock, LK_EXCLUSIVE);
	ret = drm_mm_insert_node_in_range_generic(mm, node, mem->num_pages,
					  mem->page_alignment, 0,
					  place->fpfn, lpfn,
					  DRM_MM_SEARCH_BEST,
					  aflags);
	lockmgr(&rman->lock, LK_RELEASE);

	if (unlikely(ret)) {
		kfree(node);
	} else {
		mem->mm_node = node;
		mem->start = node->start;
	}

	return 0;
}

static void ttm_bo_man_put_node(struct ttm_mem_type_manager *man,
				struct ttm_mem_reg *mem)
{
	struct ttm_range_manager *rman = (struct ttm_range_manager *) man->priv;

	if (mem->mm_node) {
		lockmgr(&rman->lock, LK_EXCLUSIVE);
		drm_mm_remove_node(mem->mm_node);
		lockmgr(&rman->lock, LK_RELEASE);

		kfree(mem->mm_node);
		mem->mm_node = NULL;
	}
}

static int ttm_bo_man_init(struct ttm_mem_type_manager *man,
			   unsigned long p_size)
{
	struct ttm_range_manager *rman;

	rman = kzalloc(sizeof(*rman), GFP_KERNEL);
	if (!rman)
		return -ENOMEM;

	drm_mm_init(&rman->mm, 0, p_size);
	lockinit(&rman->lock, "ttmrman", 0, LK_CANRECURSE);
	man->priv = rman;
	return 0;
}

static int ttm_bo_man_takedown(struct ttm_mem_type_manager *man)
{
	struct ttm_range_manager *rman = (struct ttm_range_manager *) man->priv;
	struct drm_mm *mm = &rman->mm;

	lockmgr(&rman->lock, LK_EXCLUSIVE);
	if (drm_mm_clean(mm)) {
		drm_mm_takedown(mm);
		lockmgr(&rman->lock, LK_RELEASE);
		kfree(rman);
		man->priv = NULL;
		return 0;
	}
	lockmgr(&rman->lock, LK_RELEASE);
	return -EBUSY;
}

static void ttm_bo_man_debug(struct ttm_mem_type_manager *man,
			     const char *prefix)
{
	struct ttm_range_manager *rman = (struct ttm_range_manager *) man->priv;

	lockmgr(&rman->lock, LK_EXCLUSIVE);
	drm_mm_debug_table(&rman->mm, prefix);
	lockmgr(&rman->lock, LK_RELEASE);
}

const struct ttm_mem_type_manager_func ttm_bo_manager_func = {
	ttm_bo_man_init,
	ttm_bo_man_takedown,
	ttm_bo_man_get_node,
	ttm_bo_man_put_node,
	ttm_bo_man_debug
};
EXPORT_SYMBOL(ttm_bo_manager_func);
