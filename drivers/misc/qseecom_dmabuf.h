/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Buffer management for the legacy QSEECOM driver.
 *
 * The downstream 4.9 driver received every shared buffer from user space
 * as an ION file descriptor and carved its own work buffers from ION
 * heaps. ION is gone from the kernel; this layer provides the same
 * services on top of DMA-BUF:
 *
 *  - import: pin a user space DMA-BUF fd, map it for device and CPU
 *    access once and cache the result for the lifetime of the buffer,
 *    instead of re-importing per ioctl like the downstream driver did;
 *  - alloc: carve device-coherent work buffers that are handed to
 *    TrustZone by physical address (TA images, kernel client buffers);
 *  - cache maintenance by kernel virtual address for the physically
 *    addressed TrustZone consumers, matching the dmac_flush_range()
 *    behaviour of the downstream driver.
 *
 * All physical addresses handed to TrustZone are true physical addresses:
 * the QSEECOM device has no IOMMU, and TrustZone bypasses any MMU the
 * device might have anyway.
 */

#ifndef __QSEECOM_DMABUF_H
#define __QSEECOM_DMABUF_H

#include <linux/dma-mapping.h>
#include <linux/types.h>

struct device;

/**
 * struct qseecom_buf - an imported or allocated QSEECOM shared buffer
 * @dev:      device the buffer was mapped or allocated against
 * @dmabuf:   the imported DMA-BUF, NULL for allocated buffers
 * @attach:   attachment of @dmabuf to @dev
 * @sgt:      scatter table of the mapped attachment
 * @nents:    number of entries in @sgt->sgl
 * @phys:     physical address of the first scatterlist entry
 * @len:      total length of the buffer in bytes
 * @vaddr:    kernel virtual address of the first scatterlist entry
 * @coherent: buffer is device-coherent, no cache maintenance needed
 */
struct qseecom_buf {
	struct device *dev;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	unsigned int nents;
	phys_addr_t phys;
	size_t len;
	void *vaddr;
	bool coherent;
};

static inline bool qseecom_buf_contiguous(const struct qseecom_buf *buf)
{
	return buf->nents == 1;
}

int qseecom_buf_import(struct device *dev, int fd, struct qseecom_buf **out);
int qseecom_buf_alloc(struct device *dev, size_t size,
		      struct qseecom_buf **out);
void qseecom_buf_free(struct qseecom_buf *buf);

void qseecom_buf_clean_range(void *vaddr, size_t len);
void qseecom_buf_inval_range(void *vaddr, size_t len);
void qseecom_buf_clean_inval_range(void *vaddr, size_t len);

void qseecom_buf_clean(const struct qseecom_buf *buf);
void qseecom_buf_inval(const struct qseecom_buf *buf);
void qseecom_buf_clean_inval(const struct qseecom_buf *buf);

#endif /* __QSEECOM_DMABUF_H */
