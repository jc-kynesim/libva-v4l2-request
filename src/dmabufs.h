#ifndef DMABUFS_H
#define DMABUFS_H

struct dmabufs_ctrl;
struct dmabuf_h;

struct dmabufs_ctrl * dmabufs_ctrl_new(void);
void dmabufs_ctrl_delete(struct dmabufs_ctrl * const dbsc);

struct dmabuf_h * dmabuf_alloc(struct dmabufs_ctrl * dbsc, size_t size);
void dmabuf_free(struct dmabuf_h * dh);

#endif
