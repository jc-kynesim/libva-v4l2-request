#ifndef DMABUFS_H
#define DMABUFS_H

struct dmabufs_ctrl;
struct dmabuf_h;

struct dmabufs_ctrl * dmabufs_ctrl_new(void);
void dmabufs_ctrl_delete(struct dmabufs_ctrl * const dbsc);

struct dmabuf_h * dmabuf_alloc(struct dmabufs_ctrl * dbsc, size_t size);
void * dmabuf_map(struct dmabuf_h * const dh);

/* flags from linux/dmabuf.h DMA_BUF_SYNC_xxx */
int dmabuf_sync(struct dmabuf_h * const dh, unsigned int flags);

int dmabuf_write_start(struct dmabuf_h * const dh);
int dmabuf_write_end(struct dmabuf_h * const dh);
int dmabuf_read_start(struct dmabuf_h * const dh);
int dmabuf_read_end(struct dmabuf_h * const dh);

int dmabuf_fd(const struct dmabuf_h * const dh);
size_t dmabuf_size(const struct dmabuf_h * const dh);
void dmabuf_free(struct dmabuf_h * dh);

#endif
