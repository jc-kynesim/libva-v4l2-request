#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#include "dmabufs.h"
#include "utils.h"

#define DMABUF_NAME1  "/dev/dma_heap/linux,cma"
#define DMABUF_NAME2  "/dev/dma_heap/reserved"


struct dmabufs_ctrl {
	int fd;
	size_t page_size;
};

struct dmabuf_h {
	int fd;
	size_t size;
	size_t len;
	void * mapptr;
};


struct dmabuf_h * dmabuf_alloc(struct dmabufs_ctrl * dbsc, size_t size)
{
	struct dmabuf_h * dh = malloc(sizeof(*dh));
	struct dma_heap_allocation_data data = {
		.len = (size + dbsc->page_size - 1) & ~(dbsc->page_size - 1),
		.fd = 0,
		.fd_flags = O_RDWR,
		.heap_flags = 0
	};

	if (!dh || !size)
		return NULL;

	while (ioctl(dbsc->fd, DMA_HEAP_IOCTL_ALLOC, &data) == -1) {
		if (errno == EINTR)
			continue;
		request_log("Failed to alloc %" PRIu64 " from dma-heap(fd=%d): %d (%s)\n",
			    data.len,
			    dbsc->fd,
			    errno,
			    strerror(errno));
		goto fail;
	}

	*dh = (struct dmabuf_h){
		.fd = data.fd,
		.size = (size_t)data.len,
		.mapptr = MAP_FAILED
	};

	return dh;

fail:
	free(dh);
	return NULL;
}

int dmabuf_sync(struct dmabuf_h * const dh, unsigned int flags)
{
	struct dma_buf_sync sync = {
		.flags = flags
	};
	while (ioctl(dh->fd, DMA_BUF_IOCTL_SYNC, &sync) == -1) {
		const int err = errno;
		if (errno == EINTR)
			continue;
		request_log("%s: ioctl failed: flags=%#x\n", __func__, flags);
		return -err;
	}
	return 0;
}

int dmabuf_write_start(struct dmabuf_h * const dh)
{
	return dmabuf_sync(dh, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
}

int dmabuf_write_end(struct dmabuf_h * const dh)
{
	return dmabuf_sync(dh, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
}

int dmabuf_read_start(struct dmabuf_h * const dh)
{
	if (!dmabuf_map(dh))
		return -1;
	return dmabuf_sync(dh, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
}

int dmabuf_read_end(struct dmabuf_h * const dh)
{
	return dmabuf_sync(dh, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}


void * dmabuf_map(struct dmabuf_h * const dh)
{
	if (!dh)
		return NULL;
	if (dh->mapptr != MAP_FAILED)
		return dh->mapptr;
	dh->mapptr = mmap(NULL, dh->size,
			  PROT_READ | PROT_WRITE,
			  MAP_SHARED | MAP_POPULATE,
			  dh->fd, 0);
	if (dh->mapptr == MAP_FAILED) {
		request_log("%s: Map failed\n", __func__);
		return NULL;
	}
	return dh->mapptr;
}

int dmabuf_fd(const struct dmabuf_h * const dh)
{
	if (!dh)
		return -1;
	return dh->fd;
}

size_t dmabuf_size(const struct dmabuf_h * const dh)
{
	if (!dh)
		return 0;
	return dh->size;
}

size_t dmabuf_len(const struct dmabuf_h * const dh)
{
	if (!dh)
		return 0;
	return dh->len;
}

void dmabuf_len_set(struct dmabuf_h * const dh, const size_t len)
{
	dh->len = len;
}



void dmabuf_free(struct dmabuf_h * dh)
{
	if (!dh)
		return;

	if (dh->mapptr != MAP_FAILED)
		munmap(dh->mapptr, dh->size);
	while (close(dh->fd) == -1 && errno == EINTR)
		/* loop */;
	free(dh);
}

struct dmabufs_ctrl * dmabufs_ctrl_new(void)
{
	struct dmabufs_ctrl * dbsc = malloc(sizeof(*dbsc));

	if (!dbsc)
		return NULL;

	while ((dbsc->fd = open(DMABUF_NAME1, O_RDWR)) == -1 &&
	       errno == EINTR)
		/* Loop */;

	if (dbsc->fd == -1) {
		while ((dbsc->fd = open(DMABUF_NAME2, O_RDWR)) == -1 &&
		       errno == EINTR)
			/* Loop */;
		if (dbsc->fd == -1) {
			request_log("Unable to open either %s or %s\n",
				    DMABUF_NAME1, DMABUF_NAME2);
			goto fail;
		}
	}

	dbsc->page_size = (size_t)sysconf(_SC_PAGE_SIZE);

	return dbsc;

fail:
	free(dbsc);
	return NULL;
}

void dmabufs_ctrl_delete(struct dmabufs_ctrl * const dbsc)
{
	if (!dbsc)
		return;

	while (close(dbsc->fd) == -1 && errno == EINTR)
		/* loop */;

	free(dbsc);
}


