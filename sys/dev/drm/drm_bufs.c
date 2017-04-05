/*
 * Legacy: Generic DRM Buffer Management
 *
 * Copyright 1999, 2000 Precision Insight, Inc., Cedar Park, Texas.
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
 * All Rights Reserved.
 *
 * Author: Rickard E. (Rik) Faith <faith@valinux.com>
 * Author: Gareth Hughes <gareth@valinux.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sys/conf.h>
#include <bus/pci/pcireg.h>
#include <linux/types.h>
#include <linux/export.h>
#include <drm/drmP.h>
#include "drm_legacy.h"

int drm_legacy_addmap(struct drm_device * dev, resource_size_t offset,
		      unsigned int size, enum drm_map_type type,
		      enum drm_map_flags flags, struct drm_local_map **map_ptr)
{
	struct drm_local_map *map;
	struct drm_map_list *entry = NULL;
	drm_dma_handle_t *dmah;

	/* Allocate a new map structure, fill it in, and do any type-specific
	 * initialization necessary.
	 */
	map = kmalloc(sizeof(*map), M_DRM, M_ZERO | M_WAITOK | M_NULLOK);
	if (!map) {
		return -ENOMEM;
	}

	map->offset = offset;
	map->size = size;
	map->type = type;
	map->flags = flags;

	/* Only allow shared memory to be removable since we only keep enough
	 * book keeping information about shared memory to allow for removal
	 * when processes fork.
	 */
	if ((flags & _DRM_REMOVABLE) && type != _DRM_SHM) {
		DRM_ERROR("Requested removable map for non-DRM_SHM\n");
		kfree(map);
		return -EINVAL;
	}
	if ((offset & PAGE_MASK) || (size & PAGE_MASK)) {
		DRM_ERROR("offset/size not page aligned: 0x%jx/0x%04x\n",
		    (uintmax_t)offset, size);
		kfree(map);
		return -EINVAL;
	}
	if (offset + size < offset) {
		DRM_ERROR("offset and size wrap around: 0x%jx/0x%04x\n",
		    (uintmax_t)offset, size);
		kfree(map);
		return -EINVAL;
	}

	DRM_DEBUG("offset = 0x%08llx, size = 0x%08lx, type = %d\n",
		  (unsigned long long)map->offset, map->size, map->type);

	/* Check if this is just another version of a kernel-allocated map, and
	 * just hand that back if so.
	 */
	if (type == _DRM_REGISTERS || type == _DRM_FRAME_BUFFER ||
	    type == _DRM_SHM) {
		list_for_each_entry(entry, &dev->maplist, head) {
			if (entry->map->type == type && (entry->map->offset == offset ||
			    (entry->map->type == _DRM_SHM &&
			    entry->map->flags == _DRM_CONTAINS_LOCK))) {
				entry->map->size = size;
				DRM_DEBUG("Found kernel map %d\n", type);
				goto done;
			}
		}
	}

	switch (map->type) {
	case _DRM_REGISTERS:
	case _DRM_FRAME_BUFFER:

		if (map->type == _DRM_FRAME_BUFFER ||
		    (map->flags & _DRM_WRITE_COMBINING)) {
			map->mtrr =
				arch_phys_wc_add(map->offset, map->size);
		}
		if (map->type == _DRM_REGISTERS) {
			if (map->flags & _DRM_WRITE_COMBINING)
				map->handle = ioremap_wc(map->offset,
							 map->size);
			else
				map->handle = ioremap(map->offset, map->size);
			if (!map->handle) {
				kfree(map);
				return -ENOMEM;
			}
		}

		break;
	case _DRM_SHM:
		map->handle = kmalloc(map->size, M_DRM, M_WAITOK | M_NULLOK);
		DRM_DEBUG("%lu %d %p\n",
			  map->size, order_base_2(map->size), map->handle);
		if (!map->handle) {
			kfree(map);
			return -ENOMEM;
		}
		map->offset = (unsigned long)map->handle;
		if (map->flags & _DRM_CONTAINS_LOCK) {
			/* Prevent a 2nd X Server from creating a 2nd lock */
			DRM_LOCK(dev);
			if (dev->lock.hw_lock != NULL) {
				DRM_UNLOCK(dev);
				kfree(map->handle);
				kfree(map);
				return -EBUSY;
			}
			dev->lock.hw_lock = map->handle; /* Pointer to lock */
			DRM_UNLOCK(dev);
		}
		break;
	case _DRM_AGP:

		if (!dev->agp) {
			kfree(map);
			return -EINVAL;
		}
		/*valid = 0;*/
		/* In some cases (i810 driver), user space may have already
		 * added the AGP base itself, because dev->agp->base previously
		 * only got set during AGP enable.  So, only add the base
		 * address if the map's offset isn't already within the
		 * aperture.
		 */
		if (map->offset < dev->agp->base ||
		    map->offset > dev->agp->base +
		    dev->agp->agp_info.ai_aperture_size - 1) {
			map->offset += dev->agp->base;
		}
		map->mtrr   = dev->agp->agp_mtrr; /* for getmap */
		/*for (entry = dev->agp->memory; entry; entry = entry->next) {
			if ((map->offset >= entry->bound) &&
			    (map->offset + map->size <=
			    entry->bound + entry->pages * PAGE_SIZE)) {
				valid = 1;
				break;
			}
		}
		if (!valid) {
			kfree(map);
			return -EACCES;
		}*/
		break;
	case _DRM_SCATTER_GATHER:
		if (!dev->sg) {
			kfree(map);
			return -EINVAL;
		}
		map->handle = (void *)(uintptr_t)(dev->sg->vaddr + offset);
		map->offset = dev->sg->vaddr + offset;
		break;
	case _DRM_CONSISTENT:
		/* dma_addr_t is 64bit on i386 with CONFIG_HIGHMEM64G,
		 * As we're limiting the address to 2^32-1 (or less),
		 * casting it down to 32 bits is no problem, but we
		 * need to point to a 64bit variable first. */
		dmah = drm_pci_alloc(dev, map->size, map->size);
		if (!dmah) {
			kfree(map);
			return -ENOMEM;
		}
		map->handle = dmah->vaddr;
		map->offset = dmah->busaddr;
		break;
	default:
		DRM_ERROR("Bad map type %d\n", map->type);
		kfree(map);
		return -EINVAL;
	}

	list_add(&entry->head, &dev->maplist);

done:
	/* Jumped to, with lock held, when a kernel map is found. */

	DRM_DEBUG("Added map %d 0x%lx/0x%lx\n", map->type, map->offset,
	    map->size);

	*map_ptr = map;

	return 0;
}

/**
 * Ioctl to specify a range of memory that is available for mapping by a
 * non-root process.
 *
 * \param inode device inode.
 * \param file_priv DRM file private.
 * \param cmd command.
 * \param arg pointer to a drm_map structure.
 * \return zero on success or a negative value on error.
 *
 */
int drm_legacy_addmap_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file_priv)
{
	struct drm_map *request = data;
	drm_local_map_t *map;
	int err;

	if (!(dev->flags & (FREAD|FWRITE)))
		return -EACCES; /* Require read/write */

	if (!capable(CAP_SYS_ADMIN) && request->type != _DRM_AGP)
		return -EACCES;

	DRM_LOCK(dev);
	err = drm_legacy_addmap(dev, request->offset, request->size, request->type,
	    request->flags, &map);
	DRM_UNLOCK(dev);
	if (err != 0)
		return err;

	request->offset = map->offset;
	request->size = map->size;
	request->type = map->type;
	request->flags = map->flags;
	request->mtrr   = map->mtrr;
	request->handle = (void *)map->handle;

	return 0;
}

/*
 * Get a mapping information.
 *
 * \param inode device inode.
 * \param file_priv DRM file private.
 * \param cmd command.
 * \param arg user argument, pointing to a drm_map structure.
 *
 * \return zero on success or a negative number on failure.
 *
 * Searches for the mapping with the specified offset and copies its information
 * into userspace
 */
int drm_legacy_getmap_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file_priv)
{
	struct drm_map *map = data;
	struct drm_map_list *r_list = NULL;
	struct list_head *list;
	int idx;
	int i;

	if (!drm_core_check_feature(dev, DRIVER_KMS_LEGACY_CONTEXT) &&
	    drm_core_check_feature(dev, DRIVER_MODESET))
		return -EINVAL;

	idx = map->offset;
	if (idx < 0)
		return -EINVAL;

	i = 0;
	mutex_lock(&dev->struct_mutex);
	list_for_each(list, &dev->maplist) {
		if (i == idx) {
			r_list = list_entry(list, struct drm_map_list, head);
			break;
		}
		i++;
	}
	if (!r_list || !r_list->map) {
		mutex_unlock(&dev->struct_mutex);
		return -EINVAL;
	}

	map->offset = r_list->map->offset;
	map->size = r_list->map->size;
	map->type = r_list->map->type;
	map->flags = r_list->map->flags;
	map->handle = (void *)(unsigned long) r_list->user_token;
	map->mtrr = r_list->map->mtrr;

	mutex_unlock(&dev->struct_mutex);

	return 0;
}

/**
 * Remove a map private from list and deallocate resources if the mapping
 * isn't in use.
 *
 * Searches the map on drm_device::maplist, removes it from the list, see if
 * its being used, and free any associate resource (such as MTRR's) if it's not
 * being on use.
 *
 * \sa drm_legacy_addmap
 */
int drm_legacy_rmmap_locked(struct drm_device *dev, struct drm_local_map *map)
{
	struct drm_map_list *r_list = NULL, *list_t;
	drm_dma_handle_t dmah;
	int found = 0;

	/* Find the list entry for the map and remove it */
	list_for_each_entry_safe(r_list, list_t, &dev->maplist, head) {
		if (r_list->map == map) {
			list_del(&r_list->head);
			kfree(r_list);
			found = 1;
			break;
		}
	}

	if (!found)
		return -EINVAL;

	switch (map->type) {
	case _DRM_REGISTERS:
		drm_legacy_ioremapfree(map, dev);
		/* FALLTHROUGH */
	case _DRM_FRAME_BUFFER:
		arch_phys_wc_del(map->mtrr);
		break;
	case _DRM_SHM:
		kfree(map->handle);
		break;
	case _DRM_AGP:
	case _DRM_SCATTER_GATHER:
		break;
	case _DRM_CONSISTENT:
		dmah.vaddr = map->handle;
		dmah.busaddr = map->offset;
		dmah.size = map->size;
		__drm_legacy_pci_free(dev, &dmah);
		break;
	}
	kfree(map);

	return 0;
}

int drm_legacy_rmmap(struct drm_device *dev, struct drm_local_map *map)
{
	int ret;

	if (!drm_core_check_feature(dev, DRIVER_KMS_LEGACY_CONTEXT) &&
	    drm_core_check_feature(dev, DRIVER_MODESET))
		return -EINVAL;

	mutex_lock(&dev->struct_mutex);
	ret = drm_legacy_rmmap_locked(dev, map);
	mutex_unlock(&dev->struct_mutex);

	return ret;
}
EXPORT_SYMBOL(drm_legacy_rmmap);

/* The rmmap ioctl appears to be unnecessary.  All mappings are torn down on
 * the last close of the device, and this is necessary for cleanup when things
 * exit uncleanly.  Therefore, having userland manually remove mappings seems
 * like a pointless exercise since they're going away anyway.
 *
 * One use case might be after addmap is allowed for normal users for SHM and
 * gets used by drivers that the server doesn't need to care about.  This seems
 * unlikely.
 *
 * \param inode device inode.
 * \param file_priv DRM file private.
 * \param cmd command.
 * \param arg pointer to a struct drm_map structure.
 * \return zero on success or a negative value on error.
 */
int drm_legacy_rmmap_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file_priv)
{
	struct drm_map *request = data;
	struct drm_local_map *map = NULL;
	struct drm_map_list *r_list;

	DRM_LOCK(dev);
	list_for_each_entry(r_list, &dev->maplist, head) {
		if (r_list->map &&
		    r_list->user_token == (unsigned long)request->handle &&
		    r_list->map->flags & _DRM_REMOVABLE) {
			map = r_list->map;
			break;
		}
	}

	/* List has wrapped around to the head pointer, or its empty we didn't
	 * find anything.
	 */
	if (list_empty(&dev->maplist) || !map) {
		DRM_UNLOCK(dev);
		return -EINVAL;
	}

	/* Register and framebuffer maps are permanent */
	if ((map->type == _DRM_REGISTERS) || (map->type == _DRM_FRAME_BUFFER)) {
		DRM_UNLOCK(dev);
		return 0;
	}

	drm_legacy_rmmap(dev, map);

	DRM_UNLOCK(dev);

	return 0;
}

/**
 * Cleanup after an error on one of the addbufs() functions.
 *
 * \param dev DRM device.
 * \param entry buffer entry where the error occurred.
 *
 * Frees any pages and buffers associated with the given entry.
 */
static void drm_cleanup_buf_error(struct drm_device * dev,
				  struct drm_buf_entry * entry)
{
	int i;

	if (entry->seg_count) {
		for (i = 0; i < entry->seg_count; i++) {
			drm_pci_free(dev, entry->seglist[i]);
		}
		kfree(entry->seglist);

		entry->seg_count = 0;
	}

	if (entry->buf_count) {
		for (i = 0; i < entry->buf_count; i++) {
			kfree(entry->buflist[i].dev_private);
		}
		kfree(entry->buflist);

		entry->buf_count = 0;
	}
}

static int drm_do_addbufs_agp(struct drm_device *dev, struct drm_buf_desc *request)
{
	struct drm_device_dma *dma = dev->dma;
	struct drm_buf_entry *entry;
	/* struct drm_agp_mem *agp_entry; */
	/* int valid */
	struct drm_buf *buf;
	unsigned long offset;
	unsigned long agp_offset;
	int count;
	int order;
	int size;
	int alignment;
	int page_order;
	int total;
	int byte_count;
	int i;
	struct drm_buf **temp_buflist;

	count = request->count;
	order = order_base_2(request->size);
	size = 1 << order;

	alignment  = (request->flags & _DRM_PAGE_ALIGN)
	    ? round_page(size) : size;
	page_order = order - PAGE_SHIFT > 0 ? order - PAGE_SHIFT : 0;
	total = PAGE_SIZE << page_order;

	byte_count = 0;
	agp_offset = dev->agp->base + request->agp_start;

	DRM_DEBUG("count:      %d\n",  count);
	DRM_DEBUG("order:      %d\n",  order);
	DRM_DEBUG("size:       %d\n",  size);
	DRM_DEBUG("agp_offset: 0x%lx\n", agp_offset);
	DRM_DEBUG("alignment:  %d\n",  alignment);
	DRM_DEBUG("page_order: %d\n",  page_order);
	DRM_DEBUG("total:      %d\n",  total);

	/* Make sure buffers are located in AGP memory that we own */
	/* Breaks MGA due to drm_alloc_agp not setting up entries for the
	 * memory.  Safe to ignore for now because these ioctls are still
	 * root-only.
	 */
	/*valid = 0;
	for (agp_entry = dev->agp->memory; agp_entry;
	    agp_entry = agp_entry->next) {
		if ((agp_offset >= agp_entry->bound) &&
		    (agp_offset + total * count <=
		    agp_entry->bound + agp_entry->pages * PAGE_SIZE)) {
			valid = 1;
			break;
		}
	}
	if (!valid) {
		DRM_DEBUG("zone invalid\n");
		return -EINVAL;
	}*/

	entry = &dma->bufs[order];

	entry->buflist = kmalloc(count * sizeof(*entry->buflist), M_DRM,
				 M_WAITOK | M_NULLOK | M_ZERO);
	if (!entry->buflist) {
		return -ENOMEM;
	}

	entry->buf_size = size;
	entry->page_order = page_order;

	offset = 0;

	while (entry->buf_count < count) {
		buf          = &entry->buflist[entry->buf_count];
		buf->idx     = dma->buf_count + entry->buf_count;
		buf->total   = alignment;
		buf->order   = order;
		buf->used    = 0;

		buf->offset  = (dma->byte_count + offset);
		buf->bus_address = agp_offset + offset;
		buf->address = (void *)(agp_offset + offset);
		buf->next    = NULL;
		buf->pending = 0;
		buf->file_priv = NULL;

		buf->dev_priv_size = dev->driver->dev_priv_size;
		buf->dev_private = kmalloc(buf->dev_priv_size, M_DRM,
					   M_WAITOK | M_NULLOK | M_ZERO);
		if (buf->dev_private == NULL) {
			/* Set count correctly so we free the proper amount. */
			entry->buf_count = count;
			drm_cleanup_buf_error(dev, entry);
			return -ENOMEM;
		}

		offset += alignment;
		entry->buf_count++;
		byte_count += PAGE_SIZE << page_order;
	}

	DRM_DEBUG("byte_count: %d\n", byte_count);

	temp_buflist = krealloc(dma->buflist,
	    (dma->buf_count + entry->buf_count) * sizeof(*dma->buflist),
	    M_DRM, M_WAITOK | M_NULLOK);
	if (temp_buflist == NULL) {
		/* Free the entry because it isn't valid */
		drm_cleanup_buf_error(dev, entry);
		return -ENOMEM;
	}
	dma->buflist = temp_buflist;

	for (i = 0; i < entry->buf_count; i++) {
		dma->buflist[i + dma->buf_count] = &entry->buflist[i];
	}

	dma->buf_count += entry->buf_count;
	dma->byte_count += byte_count;

	DRM_DEBUG("dma->buf_count : %d\n", dma->buf_count);
	DRM_DEBUG("entry->buf_count : %d\n", entry->buf_count);

	request->count = entry->buf_count;
	request->size = size;

	dma->flags = _DRM_DMA_USE_AGP;

	return 0;
}

static int drm_do_addbufs_pci(struct drm_device *dev, struct drm_buf_desc *request)
{
	struct drm_device_dma *dma = dev->dma;
	int count;
	int order;
	int size;
	int total;
	int page_order;
	struct drm_buf_entry *entry;
	drm_dma_handle_t *dmah;
	struct drm_buf *buf;
	int alignment;
	unsigned long offset;
	int i;
	int byte_count;
	int page_count;
	unsigned long *temp_pagelist;
	struct drm_buf **temp_buflist;

	count = request->count;
	order = order_base_2(request->size);
	size = 1 << order;

	DRM_DEBUG("count=%d, size=%d (%d), order=%d\n",
	    request->count, request->size, size, order);

	alignment = (request->flags & _DRM_PAGE_ALIGN)
	    ? round_page(size) : size;
	page_order = order - PAGE_SHIFT > 0 ? order - PAGE_SHIFT : 0;
	total = PAGE_SIZE << page_order;

	entry = &dma->bufs[order];

	entry->buflist = kmalloc(count * sizeof(*entry->buflist), M_DRM,
				 M_WAITOK | M_NULLOK | M_ZERO);
	entry->seglist = kmalloc(count * sizeof(*entry->seglist), M_DRM,
				 M_WAITOK | M_NULLOK | M_ZERO);

	/* Keep the original pagelist until we know all the allocations
	 * have succeeded
	 */
	temp_pagelist = kmalloc((dma->page_count + (count << page_order)) *
				sizeof(*dma->pagelist),
				M_DRM, M_WAITOK | M_NULLOK);

	if (entry->buflist == NULL || entry->seglist == NULL || 
	    temp_pagelist == NULL) {
		kfree(temp_pagelist);
		kfree(entry->seglist);
		kfree(entry->buflist);
		return -ENOMEM;
	}

	memcpy(temp_pagelist, dma->pagelist, dma->page_count * 
	    sizeof(*dma->pagelist));

	DRM_DEBUG("pagelist: %d entries\n",
	    dma->page_count + (count << page_order));

	entry->buf_size	= size;
	entry->page_order = page_order;
	byte_count = 0;
	page_count = 0;

	while (entry->buf_count < count) {
		spin_unlock(&dev->dma_lock);
		dmah = drm_pci_alloc(dev, PAGE_SIZE << page_order, 0x1000);
		spin_lock(&dev->dma_lock);

		if (!dmah) {
			/* Set count correctly so we free the proper amount. */
			entry->buf_count = count;
			entry->seg_count = count;
			drm_cleanup_buf_error(dev, entry);
			kfree(temp_pagelist);
			return -ENOMEM;
		}
		entry->seglist[entry->seg_count++] = dmah;
		for (i = 0; i < (1 << page_order); i++) {
			DRM_DEBUG("page %d @ 0x%08lx\n",
				  dma->page_count + page_count,
				  (unsigned long)dmah->vaddr + PAGE_SIZE * i);
			temp_pagelist[dma->page_count + page_count++]
				= (unsigned long)dmah->vaddr + PAGE_SIZE * i;
		}
		for (offset = 0;
		    offset + size <= total && entry->buf_count < count;
		    offset += alignment, ++entry->buf_count) {
			buf	     = &entry->buflist[entry->buf_count];
			buf->idx     = dma->buf_count + entry->buf_count;
			buf->total   = alignment;
			buf->order   = order;
			buf->used    = 0;
			buf->offset  = (dma->byte_count + byte_count + offset);
			buf->address = ((char *)dmah->vaddr + offset);
			buf->bus_address = dmah->busaddr + offset;
			buf->next    = NULL;
			buf->pending = 0;
			buf->file_priv = NULL;

			buf->dev_priv_size = dev->driver->dev_priv_size;
			buf->dev_private = kmalloc(buf->dev_priv_size,
						   M_DRM,
						   M_WAITOK | M_NULLOK |
						    M_ZERO);
			if (buf->dev_private == NULL) {
				/* Set count correctly so we free the proper amount. */
				entry->buf_count = count;
				entry->seg_count = count;
				drm_cleanup_buf_error(dev, entry);
				kfree(temp_pagelist);
				return -ENOMEM;
			}

			DRM_DEBUG("buffer %d @ %p\n",
			    entry->buf_count, buf->address);
		}
		byte_count += PAGE_SIZE << page_order;
	}

	temp_buflist = krealloc(dma->buflist,
	    (dma->buf_count + entry->buf_count) * sizeof(*dma->buflist),
	    M_DRM, M_WAITOK | M_NULLOK);
	if (temp_buflist == NULL) {
		/* Free the entry because it isn't valid */
		drm_cleanup_buf_error(dev, entry);
		kfree(temp_pagelist);
		return -ENOMEM;
	}
	dma->buflist = temp_buflist;

	for (i = 0; i < entry->buf_count; i++) {
		dma->buflist[i + dma->buf_count] = &entry->buflist[i];
	}

	/* No allocations failed, so now we can replace the original pagelist
	 * with the new one.
	 */
	kfree(dma->pagelist);
	dma->pagelist = temp_pagelist;

	dma->buf_count += entry->buf_count;
	dma->seg_count += entry->seg_count;
	dma->page_count += entry->seg_count << page_order;
	dma->byte_count += PAGE_SIZE * (entry->seg_count << page_order);

	request->count = entry->buf_count;
	request->size = size;

	return 0;

}

static int drm_do_addbufs_sg(struct drm_device *dev, struct drm_buf_desc *request)
{
	struct drm_device_dma *dma = dev->dma;
	struct drm_buf_entry *entry;
	struct drm_buf *buf;
	unsigned long offset;
	unsigned long agp_offset;
	int count;
	int order;
	int size;
	int alignment;
	int page_order;
	int total;
	int byte_count;
	int i;
	struct drm_buf **temp_buflist;

	count = request->count;
	order = order_base_2(request->size);
	size = 1 << order;

	alignment  = (request->flags & _DRM_PAGE_ALIGN)
	    ? round_page(size) : size;
	page_order = order - PAGE_SHIFT > 0 ? order - PAGE_SHIFT : 0;
	total = PAGE_SIZE << page_order;

	byte_count = 0;
	agp_offset = request->agp_start;

	DRM_DEBUG("count:      %d\n",  count);
	DRM_DEBUG("order:      %d\n",  order);
	DRM_DEBUG("size:       %d\n",  size);
	DRM_DEBUG("agp_offset: %ld\n", agp_offset);
	DRM_DEBUG("alignment:  %d\n",  alignment);
	DRM_DEBUG("page_order: %d\n",  page_order);
	DRM_DEBUG("total:      %d\n",  total);

	entry = &dma->bufs[order];

	entry->buflist = kmalloc(count * sizeof(*entry->buflist), M_DRM,
				 M_WAITOK | M_NULLOK | M_ZERO);
	if (entry->buflist == NULL)
		return -ENOMEM;

	entry->buf_size = size;
	entry->page_order = page_order;

	offset = 0;

	while (entry->buf_count < count) {
		buf          = &entry->buflist[entry->buf_count];
		buf->idx     = dma->buf_count + entry->buf_count;
		buf->total   = alignment;
		buf->order   = order;
		buf->used    = 0;

		buf->offset  = (dma->byte_count + offset);
		buf->bus_address = agp_offset + offset;
		buf->address = (void *)(agp_offset + offset + dev->sg->vaddr);
		buf->next    = NULL;
		buf->pending = 0;
		buf->file_priv = NULL;

		buf->dev_priv_size = dev->driver->dev_priv_size;
		buf->dev_private = kmalloc(buf->dev_priv_size, M_DRM,
					   M_WAITOK | M_NULLOK | M_ZERO);
		if (buf->dev_private == NULL) {
			/* Set count correctly so we free the proper amount. */
			entry->buf_count = count;
			drm_cleanup_buf_error(dev, entry);
			return -ENOMEM;
		}

		DRM_DEBUG("buffer %d @ %p\n",
		    entry->buf_count, buf->address);

		offset += alignment;
		entry->buf_count++;
		byte_count += PAGE_SIZE << page_order;
	}

	DRM_DEBUG("byte_count: %d\n", byte_count);

	temp_buflist = krealloc(dma->buflist,
	    (dma->buf_count + entry->buf_count) * sizeof(*dma->buflist),
	    M_DRM, M_WAITOK | M_NULLOK);
	if (temp_buflist == NULL) {
		/* Free the entry because it isn't valid */
		drm_cleanup_buf_error(dev, entry);
		return -ENOMEM;
	}
	dma->buflist = temp_buflist;

	for (i = 0; i < entry->buf_count; i++) {
		dma->buflist[i + dma->buf_count] = &entry->buflist[i];
	}

	dma->buf_count += entry->buf_count;
	dma->byte_count += byte_count;

	DRM_DEBUG("dma->buf_count : %d\n", dma->buf_count);
	DRM_DEBUG("entry->buf_count : %d\n", entry->buf_count);

	request->count = entry->buf_count;
	request->size = size;

	dma->flags = _DRM_DMA_USE_SG;

	return 0;
}

/**
 * Add AGP buffers for DMA transfers.
 *
 * \param dev struct drm_device to which the buffers are to be added.
 * \param request pointer to a struct drm_buf_desc describing the request.
 * \return zero on success or a negative number on failure.
 *
 * After some sanity checks creates a drm_buf structure for each buffer and
 * reallocates the buffer list of the same size order to accommodate the new
 * buffers.
 */
int drm_legacy_addbufs_agp(struct drm_device * dev, struct drm_buf_desc * request)
{
	int order, ret;

	if (request->count < 0 || request->count > 4096)
		return -EINVAL;
	
	order = order_base_2(request->size);
	if (order < DRM_MIN_ORDER || order > DRM_MAX_ORDER)
		return -EINVAL;

	spin_lock(&dev->dma_lock);

	/* No more allocations after first buffer-using ioctl. */
	if (dev->buf_use != 0) {
		spin_unlock(&dev->dma_lock);
		return -EBUSY;
	}
	/* No more than one allocation per order */
	if (dev->dma->bufs[order].buf_count != 0) {
		spin_unlock(&dev->dma_lock);
		return -ENOMEM;
	}

	ret = drm_do_addbufs_agp(dev, request);

	spin_unlock(&dev->dma_lock);

	return ret;
}

static int drm_legacy_addbufs_sg(struct drm_device * dev, struct drm_buf_desc * request)
{
	int order, ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	if (request->count < 0 || request->count > 4096)
		return -EINVAL;

	order = order_base_2(request->size);
	if (order < DRM_MIN_ORDER || order > DRM_MAX_ORDER)
		return -EINVAL;

	spin_lock(&dev->dma_lock);

	/* No more allocations after first buffer-using ioctl. */
	if (dev->buf_use != 0) {
		spin_unlock(&dev->dma_lock);
		return -EBUSY;
	}
	/* No more than one allocation per order */
	if (dev->dma->bufs[order].buf_count != 0) {
		spin_unlock(&dev->dma_lock);
		return -ENOMEM;
	}

	ret = drm_do_addbufs_sg(dev, request);

	spin_unlock(&dev->dma_lock);

	return ret;
}

int drm_legacy_addbufs_pci(struct drm_device * dev, struct drm_buf_desc * request)
{
	int order, ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	if (request->count < 0 || request->count > 4096)
		return -EINVAL;

	order = order_base_2(request->size);
	if (order < DRM_MIN_ORDER || order > DRM_MAX_ORDER)
		return -EINVAL;

	spin_lock(&dev->dma_lock);

	/* No more allocations after first buffer-using ioctl. */
	if (dev->buf_use != 0) {
		spin_unlock(&dev->dma_lock);
		return -EBUSY;
	}
	/* No more than one allocation per order */
	if (dev->dma->bufs[order].buf_count != 0) {
		spin_unlock(&dev->dma_lock);
		return -ENOMEM;
	}

	ret = drm_do_addbufs_pci(dev, request);

	spin_unlock(&dev->dma_lock);

	return ret;
}

/**
 * Add buffers for DMA transfers (ioctl).
 *
 * \param inode device inode.
 * \param file_priv DRM file private.
 * \param cmd command.
 * \param arg pointer to a struct drm_buf_desc request.
 * \return zero on success or a negative number on failure.
 *
 * According with the memory type specified in drm_buf_desc::flags and the
 * build options, it dispatches the call either to addbufs_agp(),
 * addbufs_sg() or addbufs_pci() for AGP, scatter-gather or consistent
 * PCI memory respectively.
 */
int drm_legacy_addbufs(struct drm_device *dev, void *data,
		       struct drm_file *file_priv)
{
	struct drm_buf_desc *request = data;
	int err;

	if (request->flags & _DRM_AGP_BUFFER)
		err = drm_legacy_addbufs_agp(dev, request);
	else if (request->flags & _DRM_SG_BUFFER)
		err = drm_legacy_addbufs_sg(dev, request);
	else
		err = drm_legacy_addbufs_pci(dev, request);

	return err;
}

/**
 * Get information about the buffer mappings.
 *
 * This was originally mean for debugging purposes, or by a sophisticated
 * client library to determine how best to use the available buffers (e.g.,
 * large buffers can be used for image transfer).
 *
 * \param inode device inode.
 * \param file_priv DRM file private.
 * \param cmd command.
 * \param arg pointer to a drm_buf_info structure.
 * \return zero on success or a negative number on failure.
 *
 * Increments drm_device::buf_use while holding the drm_device::buf_lock
 * lock, preventing of allocating more buffers after this call. Information
 * about each requested buffer is then copied into user space.
 */
int drm_legacy_infobufs(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_device_dma *dma = dev->dma;
	struct drm_buf_info *request = data;
	int i;
	int count;

	if (drm_core_check_feature(dev, DRIVER_MODESET))
		return -EINVAL;

	if (!drm_core_check_feature(dev, DRIVER_HAVE_DMA))
		return -EINVAL;

	if (!dma)
		return -EINVAL;

	spin_lock(&dev->buf_lock);
	if (atomic_read(&dev->buf_alloc)) {
		spin_unlock(&dev->buf_lock);
		return -EBUSY;
	}
	++dev->buf_use;		/* Can't allocate more after this call */
	spin_unlock(&dev->buf_lock);

	for (i = 0, count = 0; i < DRM_MAX_ORDER + 1; i++) {
		if (dma->bufs[i].buf_count)
			++count;
	}

	DRM_DEBUG("count = %d\n", count);

	if (request->count >= count) {
		for (i = 0, count = 0; i < DRM_MAX_ORDER + 1; i++) {
			if (dma->bufs[i].buf_count) {
				struct drm_buf_desc __user *to =
				    &request->list[count];
				struct drm_buf_entry *from = &dma->bufs[i];
				if (copy_to_user(&to->count,
						 &from->buf_count,
						 sizeof(from->buf_count)) ||
				    copy_to_user(&to->size,
						 &from->buf_size,
						 sizeof(from->buf_size)) ||
				    copy_to_user(&to->low_mark,
						 &from->low_mark,
						 sizeof(from->low_mark)) ||
				    copy_to_user(&to->high_mark,
						 &from->high_mark,
						 sizeof(from->high_mark)))
					return -EFAULT;

				DRM_DEBUG("%d %d %d %d %d\n",
					  i,
					  dma->bufs[i].buf_count,
					  dma->bufs[i].buf_size,
					  dma->bufs[i].low_mark,
					  dma->bufs[i].high_mark);
				++count;
			}
		}
	}
	request->count = count;

	return 0;
}

/**
 * Specifies a low and high water mark for buffer allocation
 *
 * \param inode device inode.
 * \param file_priv DRM file private.
 * \param cmd command.
 * \param arg a pointer to a drm_buf_desc structure.
 * \return zero on success or a negative number on failure.
 *
 * Verifies that the size order is bounded between the admissible orders and
 * updates the respective drm_device_dma::bufs entry low and high water mark.
 *
 * \note This ioctl is deprecated and mostly never used.
 */
int drm_legacy_markbufs(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_device_dma *dma = dev->dma;
	struct drm_buf_desc *request = data;
	int order;
	struct drm_buf_entry *entry;

	if (drm_core_check_feature(dev, DRIVER_MODESET))
		return -EINVAL;

	if (!drm_core_check_feature(dev, DRIVER_HAVE_DMA))
		return -EINVAL;

	if (!dma)
		return -EINVAL;

	DRM_DEBUG("%d, %d, %d\n",
		  request->size, request->low_mark, request->high_mark);
	order = order_base_2(request->size);
	if (order < DRM_MIN_ORDER || order > DRM_MAX_ORDER)
		return -EINVAL;
	entry = &dma->bufs[order];

	if (request->low_mark < 0 || request->low_mark > entry->buf_count)
		return -EINVAL;
	if (request->high_mark < 0 || request->high_mark > entry->buf_count)
		return -EINVAL;

	entry->low_mark = request->low_mark;
	entry->high_mark = request->high_mark;

	return 0;
}

/**
 * Unreserve the buffers in list, previously reserved using drmDMA.
 *
 * \param inode device inode.
 * \param file_priv DRM file private.
 * \param cmd command.
 * \param arg pointer to a drm_buf_free structure.
 * \return zero on success or a negative number on failure.
 *
 * Calls free_buffer() for each used buffer.
 * This function is primarily used for debugging.
 */
int drm_legacy_freebufs(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_device_dma *dma = dev->dma;
	struct drm_buf_free *request = data;
	int i;
	int idx;
	struct drm_buf *buf;
	int retcode = 0;

	DRM_DEBUG("%d\n", request->count);
	
	spin_lock(&dev->dma_lock);
	for (i = 0; i < request->count; i++) {
		if (copy_from_user(&idx, &request->list[i], sizeof(idx))) {
			retcode = -EFAULT;
			break;
		}
		if (idx < 0 || idx >= dma->buf_count) {
			DRM_ERROR("Index %d (of %d max)\n",
			    idx, dma->buf_count - 1);
			retcode = -EINVAL;
			break;
		}
		buf = dma->buflist[idx];
		if (buf->file_priv != file_priv) {
			DRM_ERROR("Process %d freeing buffer not owned\n",
			    DRM_CURRENTPID);
			retcode = -EINVAL;
			break;
		}
		drm_legacy_free_buffer(dev, buf);
	}
	spin_unlock(&dev->dma_lock);

	return retcode;
}

/**
 * Maps all of the DMA buffers into client-virtual space (ioctl).
 *
 * \param inode device inode.
 * \param file_priv DRM file private.
 * \param cmd command.
 * \param arg pointer to a drm_buf_map structure.
 * \return zero on success or a negative number on failure.
 *
 * Maps the AGP, SG or PCI buffer region with vm_mmap(), and copies information
 * about each buffer into user space. For PCI buffers, it calls vm_mmap() with
 * offset equal to 0, which drm_mmap() interpretes as PCI buffers and calls
 * drm_mmap_dma().
 */
int drm_legacy_mapbufs(struct drm_device *dev, void *data,
		       struct drm_file *file_priv)
{
	struct drm_device_dma *dma = dev->dma;
	int retcode = 0;
	const int zero = 0;
	vm_offset_t address;
	struct vmspace *vms;
	vm_ooffset_t foff;
	vm_size_t size;
	vm_offset_t vaddr;
	struct drm_buf_map *request = data;
	int i;

	vms = DRM_CURPROC->td_proc->p_vmspace;

	spin_lock(&dev->dma_lock);
	dev->buf_use++;		/* Can't allocate more after this call */
	spin_unlock(&dev->dma_lock);

	if (request->count < dma->buf_count)
		goto done;

	if ((dev->agp && (dma->flags & _DRM_DMA_USE_AGP)) ||
	    (drm_core_check_feature(dev, DRIVER_SG) &&
	    (dma->flags & _DRM_DMA_USE_SG))) {
		drm_local_map_t *map = dev->agp_buffer_map;

		if (map == NULL) {
			retcode = -EINVAL;
			goto done;
		}
		size = round_page(map->size);
		foff = (unsigned long)map->handle;
	} else {
		size = round_page(dma->byte_count),
		foff = 0;
	}

	vaddr = round_page((vm_offset_t)vms->vm_daddr + MAXDSIZ);
	retcode = -vm_mmap(&vms->vm_map, &vaddr, size, PROT_READ | PROT_WRITE,
	    VM_PROT_ALL, MAP_SHARED | MAP_NOSYNC,
	    SLIST_FIRST(&dev->devnode->si_hlist), foff);
	if (retcode)
		goto done;

	request->virtual = (void *)vaddr;

	for (i = 0; i < dma->buf_count; i++) {
		if (copy_to_user(&request->list[i].idx,
		    &dma->buflist[i]->idx, sizeof(request->list[0].idx))) {
			retcode = -EFAULT;
			goto done;
		}
		if (copy_to_user(&request->list[i].total,
		    &dma->buflist[i]->total, sizeof(request->list[0].total))) {
			retcode = -EFAULT;
			goto done;
		}
		if (copy_to_user(&request->list[i].used, &zero,
		    sizeof(zero))) {
			retcode = -EFAULT;
			goto done;
		}
		address = vaddr + dma->buflist[i]->offset; /* *** */
		if (copy_to_user(&request->list[i].address, &address,
		    sizeof(address))) {
			retcode = -EFAULT;
			goto done;
		}
	}
      done:
	request->count = dma->buf_count;
	DRM_DEBUG("%d buffers, retcode = %d\n", request->count, retcode);

	return retcode;
}

int drm_legacy_dma_ioctl(struct drm_device *dev, void *data,
		  struct drm_file *file_priv)
{
	if (drm_core_check_feature(dev, DRIVER_MODESET))
		return -EINVAL;

	if (dev->driver->dma_ioctl)
		return dev->driver->dma_ioctl(dev, data, file_priv);
	else
		return -EINVAL;
}

struct drm_local_map *drm_legacy_getsarea(struct drm_device *dev)
{
	struct drm_map_list *entry;

	list_for_each_entry(entry, &dev->maplist, head) {
		if (entry->map && entry->map->type == _DRM_SHM &&
		    (entry->map->flags & _DRM_CONTAINS_LOCK)) {
			return entry->map;
		}
	}
	return NULL;
}
EXPORT_SYMBOL(drm_legacy_getsarea);
