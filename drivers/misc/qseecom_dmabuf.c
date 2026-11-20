// SPDX-License-Identifier: GPL-2.0-only
/*
 * Buffer management for the legacy QSEECOM driver.
 *
 * See qseecom_dmabuf.h for the design.
 */

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <asm/cacheflush.h>

#include "qseecom_dmabuf.h"

/**
 * qseecom_buf_import() - Import a user space DMA-BUF fd.
 * @dev: device the buffer is mapped against
 * @fd:  DMA-BUF file descriptor
 * @out: returned buffer handle
 *
 * Pins and maps the buffer once; the mapping stays alive until
 * qseecom_buf_free(). Only the first scatterlist entry is reflected in
 * ->phys and ->vaddr; callers handing the buffer to TrustZone as a single
 * physical address must check qseecom_buf_contiguous().
 */
int qseecom_buf_import(struct device *dev, int fd, struct qseecom_buf **out)
{
	struct qseecom_buf *buf;
	struct scatterlist *sg;
	int i, ret;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf->dev = dev;
	buf->dmabuf = dma_buf_get(fd);
	if (IS_ERR(buf->dmabuf)) {
		ret = PTR_ERR(buf->dmabuf);
		goto err_free_buf;
	}

	buf->attach = dma_buf_attach(buf->dmabuf, dev);
	if (IS_ERR(buf->attach)) {
		ret = PTR_ERR(buf->attach);
		goto err_put_dmabuf;
	}

	buf->sgt = dma_buf_map_attachment_unlocked(buf->attach,
						   DMA_BIDIRECTIONAL);
	if (IS_ERR(buf->sgt)) {
		ret = PTR_ERR(buf->sgt);
		goto err_detach;
	}

	buf->nents = buf->sgt->nents;
	buf->phys = sg_phys(buf->sgt->sgl);
	buf->vaddr = sg_virt(buf->sgt->sgl);

	for_each_sg(buf->sgt->sgl, sg, buf->nents, i)
		buf->len += sg->length;

	*out = buf;
	return 0;

err_detach:
	dma_buf_detach(buf->dmabuf, buf->attach);
err_put_dmabuf:
	dma_buf_put(buf->dmabuf);
err_free_buf:
	kfree(buf);
	return ret;
}

/**
 * qseecom_buf_alloc() - Allocate a device-coherent work buffer.
 * @dev:  device to allocate against
 * @size: requested size in bytes, rounded up to a page
 * @out:  returned buffer handle
 */
int qseecom_buf_alloc(struct device *dev, size_t size,
		      struct qseecom_buf **out)
{
	struct qseecom_buf *buf;
	dma_addr_t dma;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf->dev = dev;
	buf->len = PAGE_ALIGN(size);
	buf->vaddr = dma_alloc_coherent(dev, buf->len, &dma, GFP_KERNEL);
	if (!buf->vaddr) {
		kfree(buf);
		return -ENOMEM;
	}

	buf->phys = (phys_addr_t)dma;
	buf->nents = 1;
	buf->coherent = true;

	*out = buf;
	return 0;
}

/**
 * qseecom_buf_free() - Release a buffer handle.
 */
void qseecom_buf_free(struct qseecom_buf *buf)
{
	if (!buf)
		return;

	if (buf->coherent)
		dma_free_coherent(buf->dev, buf->len, buf->vaddr,
				  (dma_addr_t)buf->phys);

	if (buf->sgt)
		dma_buf_unmap_attachment_unlocked(buf->attach, buf->sgt,
						  DMA_BIDIRECTIONAL);
	if (buf->attach)
		dma_buf_detach(buf->dmabuf, buf->attach);
	if (buf->dmabuf)
		dma_buf_put(buf->dmabuf);

	kfree(buf);
}

/**
 * qseecom_buf_clean_range() - Clean a cached kernel VA range to PoC.
 *
 * Called after the kernel wrote data that TrustZone reads by physical
 * address. No-op for device-coherent buffers.
 */
void qseecom_buf_clean_range(void *vaddr, size_t len)
{
	dcache_clean_poc((unsigned long)vaddr, (unsigned long)vaddr + len);
}

/**
 * qseecom_buf_inval_range() - Invalidate a cached kernel VA range.
 *
 * Called before the kernel reads data TrustZone wrote by physical
 * address. On arm64 invalidating a dirty line also cleans it.
 */
void qseecom_buf_inval_range(void *vaddr, size_t len)
{
	dcache_inval_poc((unsigned long)vaddr, (unsigned long)vaddr + len);
}

/**
 * qseecom_buf_clean_inval_range() - Clean and invalidate a VA range.
 */
void qseecom_buf_clean_inval_range(void *vaddr, size_t len)
{
	dcache_clean_inval_poc((unsigned long)vaddr,
			       (unsigned long)vaddr + len);
}

static bool qseecom_buf_need_cache_op(const struct qseecom_buf *buf)
{
	return !buf->coherent && buf->vaddr;
}

void qseecom_buf_clean(const struct qseecom_buf *buf)
{
	if (qseecom_buf_need_cache_op(buf))
		qseecom_buf_clean_range(buf->vaddr, buf->len);
}

void qseecom_buf_inval(const struct qseecom_buf *buf)
{
	if (qseecom_buf_need_cache_op(buf))
		qseecom_buf_inval_range(buf->vaddr, buf->len);
}

void qseecom_buf_clean_inval(const struct qseecom_buf *buf)
{
	if (qseecom_buf_need_cache_op(buf))
		qseecom_buf_clean_inval_range(buf->vaddr, buf->len);
}
