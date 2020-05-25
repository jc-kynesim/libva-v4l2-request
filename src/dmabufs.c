#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/dma-heap.h>

#include "dmabufs.h"
#include "utils.h"

#define DMABUF_NAME1  "/dev/dma_heap/linux,cma"
#define DMABUF_NAME2  "/dev/dma_heap/reserved"


struct dmabufs_ctrl {
	int fd;
};

struct dmabuf_h {
	int fd;
};


struct dmabuf_h * dmabuf_alloc(struct dmabufs_ctrl * dbsc, size_t size)
{
	struct dmabuf_h * dh = malloc(sizeof(*dh));
	struct dma_heap_allocation_data data = {
		.len = size,
		.fd = -1,
		.fd_flags = O_RDWR,
		.heap_flags = 0
	};

	if (!dh)
		return NULL;

	while (ioctl(dbsc->fd, DMA_HEAP_IOCTL_ALLOC, &data) == -1) {
		if (errno == EINTR)
			continue;
		request_log("Failed to alloc %zd from dma-heap\n", size);
		goto fail;
	}

	dh->fd = data.fd;
	return dh;

fail:
	free(dh);
	return NULL;
}

void dmabuf_free(struct dmabuf_h * dh)
{
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
		while ((dbsc->fd = open(DMABUF_NAME1, O_RDWR)) == -1 &&
		       errno == EINTR)
			/* Loop */;
		if (dbsc->fd == -1) {
			request_log("Unable to open either %s or %s\n",
				    DMABUF_NAME1, DMABUF_NAME2);
			free(dbsc);
			return NULL;
		}
	}
	return dbsc;
}

void dmabufs_ctrl_delete(struct dmabufs_ctrl * const dbsc)
{
	if (!dbsc)
		return;

	while (close(dbsc->fd) == -1 && errno == EINTR)
		/* loop */;

	free(dbsc);
}


