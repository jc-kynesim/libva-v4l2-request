/*
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/media.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <va/va_backend.h>

#include "dmabufs.h"
#include "media.h"
#include "pollqueue.h"
#include "utils.h"
#include "v4l2.h"
#include "video.h"

struct media_request;

struct media_pool {
	int fd;
	sem_t sem;
	pthread_mutex_t lock;
	struct media_request * free_reqs;
	struct pollqueue * pq;
};

struct media_request {
	struct media_request * next;
	struct media_pool * mp;
	int fd;
	struct polltask * pt;
};


static inline int do_wait(sem_t *const sem)
{
	while (sem_wait(sem)) {
		if (errno != EINTR)
			return -errno;
	}
	return 0;
}

struct media_request * media_request_get(struct media_pool * const mp)
{
	struct media_request *req = NULL;

	/* Timeout handled by poll code */
	if (do_wait(&mp->sem))
		return NULL;

	pthread_mutex_lock(&mp->lock);
	req = mp->free_reqs;
	if (req) {
		mp->free_reqs = req->next;
		req->next = NULL;
	}
	pthread_mutex_unlock(&mp->lock);
	return req;
}

int media_request_fd(const struct media_request * const req)
{
	return req->fd;
}

int media_request_start(struct media_request * const req)
{
	struct media_pool * const mp = req->mp;

	while (ioctl(req->fd, MEDIA_REQUEST_IOC_QUEUE, NULL) == -1)
	{
		const int err = errno;
		if (err == EINTR)
			continue;
		request_log("%s: Failed to Q media: (%d) %s\n", __func__, err, strerror(err));
		return -err;
	}

	pollqueue_add_task(mp->pq, req->pt, 2000);
	return 0;
}

static void media_request_done(void *v, short revents)
{
	struct media_request *const req = v;
	struct media_pool *const mp = req->mp;

	/* ** Not sure what to do about timeout */

	if (ioctl(req->fd, MEDIA_REQUEST_IOC_REINIT, NULL) < 0)
		request_log("Unable to reinit media request: %s\n",
			    strerror(errno));

	pthread_mutex_lock(&mp->lock);
	req->next = mp->free_reqs;
	mp->free_reqs = req;
	pthread_mutex_unlock(&mp->lock);
	sem_post(&mp->sem);
}

static void delete_req_chain(struct media_request * const chain)
{
	struct media_request * next = chain;
	while (next) {
		struct media_request * const req = next;
		next = req->next;
		if (req->fd != -1)
			close(req->fd);
		free(req);
	}
}

struct media_pool * media_pool_new(const char * const media_path,
				   struct pollqueue * const pq,
				   const unsigned int n)
{
	struct media_pool * const mp = calloc(1, sizeof(*mp));
	unsigned int i;

	if (!mp)
		goto fail0;

	mp->pq = pq;
	pthread_mutex_init(&mp->lock, NULL);
	mp->fd = open(media_path, O_RDWR | O_NONBLOCK);
	if (mp->fd == -1) {
		request_log("Failed to open '%s': %s\n", media_path, strerror(errno));
		goto fail1;
	}

	for (i = 0; i != n; ++i) {
		struct media_request * req = malloc(sizeof(*req));
		if (!req)
			goto fail4;

		*req = (struct media_request){
			.next = mp->free_reqs,
			.mp = mp,
			.fd = -1
		};
		mp->free_reqs = req;

		if (ioctl(mp->fd, MEDIA_IOC_REQUEST_ALLOC, &req->fd) == -1) {
			request_log("Failed to alloc request %d: %s\n", i, strerror(errno));
			goto fail4;
		}

		req->pt = polltask_new(req->fd, POLLPRI, media_request_done, req);
		if (!req->pt)
			goto fail4;
	}

	sem_init(&mp->sem, 0, n);

	return mp;

fail4:
	delete_req_chain(mp->free_reqs);
	close(mp->fd);
	pthread_mutex_destroy(&mp->lock);
fail1:
	free(mp);
fail0:
	return NULL;
}

void media_pool_delete(struct media_pool * mp)
{
	if (!mp)
		return;

	delete_req_chain(mp->free_reqs);
	close(mp->fd);
	sem_destroy(&mp->sem);
	pthread_mutex_destroy(&mp->lock);
	free(mp);
}


#define INDEX_UNSET (~(uint32_t)0)

enum qent_status {
	QENT_NEW,
	QENT_PENDING,
	QENT_WAITING,
	QENT_DONE,
	QENT_ERROR
};

struct mediabuf_qent {
	struct mediabuf_qent *next;
	struct mediabuf_qent *prev;
	enum qent_status status;
	uint32_t index;
	struct dmabuf_h *dh[VIDEO_MAX_PLANES];
	struct timeval timestamp;
	sem_t sem; /* Destination only */
};

struct buf_pool {
	pthread_mutex_t lock;
	sem_t free_sem;
	enum v4l2_buf_type buf_type;
	struct mediabuf_qent *free_head;
	struct mediabuf_qent *free_tail;
	struct mediabuf_qent *inuse_head;
	struct mediabuf_qent *inuse_tail;
};

static void qent_delete(struct mediabuf_qent *const be)
{
	unsigned int i;
	if (!be)
		return;
	for (i = 0; i != VIDEO_MAX_PLANES; ++i)
		dmabuf_free(be->dh[i]);
	sem_destroy(&be->sem);
	free(be);
}

static struct mediabuf_qent *qent_new()
{
	struct mediabuf_qent * be = malloc(sizeof(*be));
	*be = (struct mediabuf_qent) {
		.status = QENT_NEW,
		.index  = INDEX_UNSET
	};
	sem_init(&be->sem, 0, 0);
	return be;
}

static void bq_put_free(struct buf_pool *const bp, struct mediabuf_qent * be)
{
	if (bp->free_tail)
		bp->free_tail->next = be;
	else
		bp->free_head = be;
	be->prev = bp->free_tail;
	be->next = NULL;
	bp->free_tail = be;
}

static struct mediabuf_qent * bq_get_free(struct buf_pool *const bp)
{
	struct mediabuf_qent *be;

	be = bp->free_head;
	if (be) {
		if (be->next)
			be->next->prev = be->prev;
		else
			bp->free_tail = be->prev;
		bp->free_head = be->next;
		be->next = NULL;
		be->prev = NULL;
	}
	return be;
}

static struct mediabuf_qent * bq_extract_inuse(struct buf_pool *const bp, struct mediabuf_qent *const be)
{
	if (be->next)
		be->next->prev = be->prev;
	else
		bp->inuse_tail = be->prev;
	if (be->prev)
		be->prev->next = be->next;
	else
		bp->inuse_head = be->next;
	be->next = NULL;
	be->prev = NULL;
	return be;
}

static void bq_free_all_free(struct buf_pool *const bp)
{
	struct mediabuf_qent *be;
	while ((be = bq_get_free(bp)) != NULL)
		qent_delete(be);
}

static void queue_put_free(struct buf_pool *const bp, struct mediabuf_qent *be)
{
	unsigned int i;

	pthread_mutex_lock(&bp->lock);
	/* Clear out state vars */
	be->timestamp.tv_sec = 0;
	be->timestamp.tv_usec = 0;
	for (i = 0; i < VIDEO_MAX_PLANES && be->dh[i]; ++i)
		dmabuf_len_set(be->dh[i], 0);
	bq_put_free(bp, be);
	pthread_mutex_unlock(&bp->lock);
	sem_post(&bp->free_sem);
}

static bool qent_is_inuse(const struct buf_pool *const bp)
{
	return bp->inuse_tail != NULL;
}

static void queue_put_inuse(struct buf_pool *const bp, struct mediabuf_qent *be)
{
	if (!be)
		return;
	pthread_mutex_lock(&bp->lock);
	if (bp->inuse_tail)
		bp->inuse_tail->next = be;
	else
		bp->inuse_head = be;
	be->prev = bp->inuse_tail;
	be->next = NULL;
	bp->inuse_tail = be;
	be->status = QENT_WAITING;
	pthread_mutex_unlock(&bp->lock);
}

static struct mediabuf_qent *queue_get_free(struct buf_pool *const bp, struct pollqueue *const pq)
{
	struct mediabuf_qent *buf;

	if (do_wait(&bp->free_sem))
		return NULL;
	pthread_mutex_lock(&bp->lock);
	buf = bq_get_free(bp);
	pthread_mutex_unlock(&bp->lock);
	return buf;
}

static struct mediabuf_qent * queue_find_extract_fd(struct buf_pool *const bp, const int fd)
{
	struct mediabuf_qent *be;

	pthread_mutex_lock(&bp->lock);
	/* Expect 1st in Q, but allow anywhere */
	for (be = bp->inuse_head; be; be = be->next) {
		if (dmabuf_fd(be->dh[0]) == fd) {
			bq_extract_inuse(bp, be);
			break;
		}
	}
	pthread_mutex_unlock(&bp->lock);

	return be;
}

static void queue_delete(struct buf_pool *const bp)
{
	if (!bp)
		return;
	sem_destroy(&bp->free_sem);
	pthread_mutex_destroy(&bp->lock);
	free(bp);
}

static struct buf_pool* queue_new(const int vfd, struct pollqueue * pq,
				  enum v4l2_buf_type buftype)
{
	struct buf_pool *bp = calloc(1, sizeof(*bp));
	if (!bp)
		return NULL;
	pthread_mutex_init(&bp->lock, NULL);
	sem_init(&bp->free_sem, 0, 0);
	return bp;
}


struct mediabufs_ctl {
	atomic_int ref_count;  /* 0 is single ref for easier atomics */
	VADriverContextP dc;
	int vfd;
	bool stream_on;
	bool polling;
	pthread_mutex_t lock;
	struct buf_pool * src;
	struct buf_pool * dst;
	struct polltask * pt;
	struct pollqueue * pq;
	struct v4l2_format src_fmt;
	struct v4l2_format dst_fmt;
};

static int qent_v4l2_queue(struct mediabuf_qent *const be,
			   const int vfd, struct media_request *const mreq,
			   const struct v4l2_format *const fmt,
			   const bool is_dst, const bool hold_flag)
{
	struct v4l2_buffer buffer = {
		.type = fmt->type,
		.memory = V4L2_MEMORY_DMABUF,
		.index = be->index
	};
	struct v4l2_plane planes[VIDEO_MAX_PLANES] = {{0}};

	if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type)) {
		unsigned int i;
		for (i = 0; i < VIDEO_MAX_PLANES && be->dh[i]; ++i) {
			if (is_dst)
				dmabuf_len_set(be->dh[i], 0);

			/* *** Really need a pixdesc rather than a format so we can fill in data_offset */
			planes[i].length = dmabuf_size(be->dh[i]);
			planes[i].bytesused = dmabuf_len(be->dh[i]);
			planes[i].m.fd = dmabuf_fd(be->dh[i]);
		}
		buffer.m.planes = planes;
		buffer.length = i;
	}
	else {
		if (is_dst)
			dmabuf_len_set(be->dh[0], 0);

		buffer.bytesused = dmabuf_len(be->dh[0]);
		buffer.length = dmabuf_size(be->dh[0]);
		buffer.m.fd = dmabuf_fd(be->dh[0]);
	}

	if (!is_dst && mreq) {
		buffer.flags |= V4L2_BUF_FLAG_REQUEST_FD;
		buffer.request_fd = media_request_fd(mreq);
		if (hold_flag)
			buffer.flags |= V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;
	}

	if (is_dst)
		be->timestamp = (struct timeval){0,0};

	buffer.timestamp = be->timestamp;

	while (ioctl(vfd, VIDIOC_QBUF, &buffer)) {
		const int err = errno;
		if (err != EINTR) {
			request_log("%s: Failed to Q buffer: err=%d (%s)\n", __func__, err, strerror(err));
			return -err;
		}
	}
	return 0;
}

static struct mediabuf_qent * qent_dequeue(struct buf_pool *const bp,
				     const int vfd,
				     const enum v4l2_buf_type buftype)
{
	int fd;
	struct mediabuf_qent *be;
	int rc;
	const bool mp = V4L2_TYPE_IS_MULTIPLANAR(buftype);
	struct v4l2_plane planes[VIDEO_MAX_PLANES] = {{0}};
	struct v4l2_buffer buffer = {
		.type =  buftype,
		.memory = V4L2_MEMORY_DMABUF
	};
	if (mp)
		buffer.m.planes = planes;

	while ((rc = ioctl(vfd, VIDIOC_DQBUF, &buffer)) != 0 &&
	       errno == EINTR)
		/* Loop */;
	if (rc) {
		request_log("Error DQing buffer\n");
		return NULL;
	}

	fd = mp ? planes[0].m.fd : buffer.m.fd;
	be = queue_find_extract_fd(bp, fd);
	if (!be) {
		request_log("Failed to find fd %d in Q\n", fd);
		return NULL;
	}

	be->status = (buffer.flags & V4L2_BUF_FLAG_ERROR) ? QENT_ERROR : QENT_DONE;
	return be;
}

static bool mediabufs_wants_poll(const struct mediabufs_ctl *const mbc)
{
	return qent_is_inuse(mbc->src) || qent_is_inuse(mbc->dst);
}

static void mediabufs_poll_cb(void * v, short revents)
{
	struct mediabufs_ctl *mbc = v;
	struct mediabuf_qent *src_be = NULL;
	struct mediabuf_qent *dst_be = NULL;
	bool qrun = false;

	if (!revents)
		request_err(mbc->dc, "%s: Timeout\n", __func__);

	pthread_mutex_lock(&mbc->lock);
	mbc->polling = false;

	if ((revents & POLLOUT) != 0)
		src_be = qent_dequeue(mbc->src, mbc->vfd, mbc->src_fmt.type);
	if ((revents & POLLIN) != 0)
		dst_be = qent_dequeue(mbc->dst, mbc->vfd, mbc->dst_fmt.type);

	/* Reschedule */
	if (mediabufs_wants_poll(mbc)) {
		mbc->polling = true;
		pollqueue_add_task(mbc->pq, mbc->pt, 2000);
		qrun = true;
	}
	pthread_mutex_unlock(&mbc->lock);

	if (src_be)
		queue_put_free(mbc->src, src_be);
	if (dst_be)
		sem_post(&dst_be->sem);
	if (!qrun)
		mediabufs_ctl_unref(&mbc);
}

int qent_src_params_set(struct mediabuf_qent *const be, const struct timeval * timestamp)
{
	be->timestamp = *timestamp;
	return 0;
}

int qent_src_data_copy(struct mediabuf_qent *const be, const void *const src, const size_t len)
{
	void * dst;
	if (len > dmabuf_size(be->dh[0])) {
		request_log("%s: Overrun %d > %d\n", __func__, len, dmabuf_size(be->dh[0]));
		return -1;
	}
	dmabuf_write_start(be->dh[0]);
	dst = dmabuf_map(be->dh[0]);
	if (!dst)
		return -1;
	memcpy(dst, src, len);
	dmabuf_len_set(be->dh[0], len);
	dmabuf_write_end(be->dh[0]);
	return 0;
}

int qent_dst_dup_fd(const struct mediabuf_qent *const be, unsigned int plane)
{
	return dup(dmabuf_fd(be->dh[plane]));
}

VAStatus mediabufs_start_request(struct mediabufs_ctl *const mbc,
			    struct media_request *const mreq,
			    struct mediabuf_qent *const src_be,
			    struct mediabuf_qent *const dst_be,
			    const bool is_final)
{
	pthread_mutex_lock(&mbc->lock);

	if (qent_v4l2_queue(src_be, mbc->vfd, mreq, &mbc->src_fmt, false, !is_final))
		goto fail1;
	queue_put_inuse(mbc->src, src_be);

	if (dst_be &&
	    qent_v4l2_queue(dst_be, mbc->vfd, NULL, &mbc->dst_fmt, true, false))
		goto fail1;
	queue_put_inuse(mbc->dst, dst_be);

	if (!mbc->polling && mediabufs_wants_poll(mbc)) {
		mbc->polling = true;
		mediabufs_ctl_ref(mbc);
		pollqueue_add_task(mbc->pq, mbc->pt, 2000);
	}
	pthread_mutex_unlock(&mbc->lock);

	if (media_request_start(mreq))
		return VA_STATUS_ERROR_OPERATION_FAILED;

	return VA_STATUS_SUCCESS;

fail1:
	pthread_mutex_unlock(&mbc->lock);
	return VA_STATUS_ERROR_OPERATION_FAILED;
}


static int qent_alloc_from_fmt(struct mediabuf_qent *const be,
			       struct dmabufs_ctrl *const dbsc,
			       const struct v4l2_format *const fmt)
{
	if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type)) {
		unsigned int i;
		for (i = 0; i != fmt->fmt.pix_mp.num_planes; ++i) {
			be->dh[i] = dmabuf_alloc(dbsc,
				fmt->fmt.pix_mp.plane_fmt[i].sizeimage);
			/* On failure tidy up and die */
			if (!be->dh[i])
			{
				while (i--) {
					dmabuf_free(be->dh[i]);
					be->dh[i] = NULL;
				}
				return -1;
			}
		}
	}
	else {
//		be->dh[0] = dmabuf_alloc(dbsc, fmt->fmt.pix.sizeimage);
		size_t size = fmt->fmt.pix.sizeimage;
#warning Fixed bitbuf size
		if (size < 0x100000)
			size = 0x100000;
		be->dh[0] = dmabuf_alloc(dbsc, size);
		if (!be->dh[0])
			return -1;
	}
	return 0;
}

static VAStatus fmt_set(struct v4l2_format *const fmt, const int fd,
			const enum v4l2_buf_type buftype,
			uint32_t pixfmt,
			const unsigned int width, const unsigned int height)
{
	*fmt = (struct v4l2_format){.type = buftype};

	if (V4L2_TYPE_IS_MULTIPLANAR(buftype)) {
		fmt->fmt.pix_mp.width = width;
		fmt->fmt.pix_mp.height = height;
		fmt->fmt.pix_mp.pixelformat = pixfmt;
	}
	else {
		fmt->fmt.pix.width = width;
		fmt->fmt.pix.height = height;
		fmt->fmt.pix.pixelformat = pixfmt;
	}

	while (ioctl(fd, VIDIOC_S_FMT, fmt))
		if (errno != EINTR)
			return VA_STATUS_ERROR_OPERATION_FAILED;

	return VA_STATUS_SUCCESS;
}

static VAStatus find_fmt_flags(struct v4l2_format *const fmt,
				   const int fd, const unsigned int rtfmt,
				   const unsigned int type_v4l2,
				   const uint32_t flags_must,
				   const uint32_t flags_not,
				   const unsigned int width,
				   const unsigned int height)
{
	unsigned int i;
	VAStatus status;

	for (i = 0;; ++i) {
		struct v4l2_fmtdesc fmtdesc = {
			.index = i,
			.type = type_v4l2
		};
		while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc)) {
			if (errno != EINTR)
				return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
		}
		if ((fmtdesc.flags & flags_must) != flags_must ||
		    (fmtdesc.flags & flags_not))
			continue;
		status = video_fmt_supported(fmtdesc.pixelformat,
					     fmtdesc.type, rtfmt);
		if (status == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT)
			return status;
		if (status != VA_STATUS_SUCCESS)
			continue;

		if (fmt_set(fmt, fd, fmtdesc.type, fmtdesc.pixelformat,
			    width, height) == VA_STATUS_SUCCESS)
			return VA_STATUS_SUCCESS;
	}
	return 0;
}


/* Wait for qent done */
VAStatus qent_dst_wait(struct mediabuf_qent *const be)
{
	enum qent_status estat;

	if (do_wait(&be->sem))
		return VA_STATUS_ERROR_OPERATION_FAILED;

	if (be->status == QENT_WAITING)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	estat = be->status;
	be->status = QENT_PENDING;

	return estat == QENT_DONE ? VA_STATUS_SUCCESS :
		estat == QENT_ERROR ? VA_STATUS_ERROR_DECODING_ERROR :
			VA_STATUS_ERROR_OPERATION_FAILED;
}

const uint8_t * qent_dst_data(struct mediabuf_qent *const be, unsigned int buf_no)
{
	return dmabuf_map(be->dh[buf_no]);
}

VAStatus qent_dst_read_start(struct mediabuf_qent *const be)
{
	unsigned int i;
	for (i = 0; i != VIDEO_MAX_PLANES && be->dh[i]; ++i) {
		if (dmabuf_read_start(be->dh[i])) {
			while (i--)
				dmabuf_read_end(be->dh[i]);
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		}
	}
	return VA_STATUS_SUCCESS;
}

VAStatus qent_dst_read_stop(struct mediabuf_qent *const be)
{
	unsigned int i;
	VAStatus status = VA_STATUS_SUCCESS;

	for (i = 0; i != VIDEO_MAX_PLANES && be->dh[i]; ++i) {
		if (dmabuf_read_end(be->dh[i]))
			status = VA_STATUS_ERROR_OPERATION_FAILED;
	}
	return status;
}

void qent_dst_delete(struct mediabuf_qent *const be)
{
	qent_delete(be);
}

struct mediabuf_qent* mediabufs_dst_qent_alloc(struct mediabufs_ctl *const mbc, struct dmabufs_ctrl *const dbsc)
{
	struct mediabuf_qent *const be = qent_new();
	int rv;

	if (qent_alloc_from_fmt(be, dbsc, &mbc->dst_fmt))
		goto fail;

	rv = v4l2_create_buffers(mbc->vfd, mbc->dst_fmt.type, V4L2_MEMORY_DMABUF, 1, &be->index);
	if (rv < 0)
		goto fail;

	return be;

fail:
	qent_delete(be);
	return NULL;
}

const struct v4l2_format *mediabufs_dst_fmt(struct mediabufs_ctl *const mbc)
{
	return &mbc->dst_fmt;
}

VAStatus mediabufs_dst_fmt_set(struct mediabufs_ctl *const mbc,
			   const unsigned int rtfmt,
			   const unsigned int width,
			   const unsigned int height)
{
	VAStatus status;
	unsigned int i;
	static const struct {
		unsigned int type_v4l2;
		unsigned int flags_must;
		unsigned int flags_not;
	} trys[] = {
		{V4L2_BUF_TYPE_VIDEO_CAPTURE,
			0, V4L2_FMT_FLAG_EMULATED},
		{V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			0, V4L2_FMT_FLAG_EMULATED},
		{V4L2_BUF_TYPE_VIDEO_CAPTURE,
			V4L2_FMT_FLAG_EMULATED, 0},
		{V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			V4L2_FMT_FLAG_EMULATED, 0}
	};
	for (i = 0; i != sizeof(trys)/sizeof(trys[0]); ++i) {
		status = find_fmt_flags(&mbc->dst_fmt, mbc->vfd, rtfmt,
					    trys[i].type_v4l2,
					    trys[i].flags_must,
					    trys[i].flags_not,
					width, height);
		if (status != VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE)
			return status;
	}
	return status;
}

struct mediabuf_qent *mediabufs_src_qent_get(struct mediabufs_ctl *const mbc)
{
	struct mediabuf_qent * buf = queue_get_free(mbc->src, mbc->pq);
	return buf;
}

/* src format must have been set up before this */
VAStatus mediabufs_src_pool_create(struct mediabufs_ctl *const mbc,
			      struct dmabufs_ctrl * const dbsc,
			      unsigned int n)
{
	unsigned int i;
	struct v4l2_requestbuffers req = {
		.count = n,
		.type = mbc->src_fmt.type,
		.memory = V4L2_MEMORY_DMABUF
	};

	bq_free_all_free(mbc->src);
	while (ioctl(mbc->vfd, VIDIOC_REQBUFS, &req) == -1) {
		if (errno != EINTR) {
			request_err(mbc->dc, "%s: Failed to request src bufs\n", __func__);
			return VA_STATUS_ERROR_OPERATION_FAILED;
		}
	}

	if (n > req.count) {
		request_info(mbc->dc, "Only allocated %d of %d src buffers requested\n", req.count, n);
		n = req.count;
	}

	for (i = 0; i != n; ++i) {
		struct mediabuf_qent *const be = qent_new();
		if (!be) {
			request_err(mbc->dc, "Failed to create src be %d\n", i);
			goto fail;
		}
		if (qent_alloc_from_fmt(be, dbsc, &mbc->src_fmt)) {
			qent_delete(be);
			goto fail;
		}
		be->index = i;

		queue_put_free(mbc->src, be);
	}

	return VA_STATUS_SUCCESS;

fail:
	bq_free_all_free(mbc->src);
	req.count = 0;
	while (ioctl(mbc->vfd, VIDIOC_REQBUFS, &req) == -1 &&
	       errno == EINTR)
		/* Loop */;

	return VA_STATUS_ERROR_OPERATION_FAILED;
}



/*
 * Set stuff order:
 *  Set src fmt
 *  Set parameters (sps) on vfd
 *  Negotiate dst format
 *  Create src buffers
*/
VAStatus mediabufs_stream_on(struct mediabufs_ctl *const mbc)
{
	if (mbc->stream_on)
		return VA_STATUS_SUCCESS;

	if (v4l2_set_stream(mbc->vfd, mbc->src_fmt.type, true) < 0) {
		request_log("Failed to set stream on src type %d\n", mbc->src_fmt.type);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	if (v4l2_set_stream(mbc->vfd, mbc->dst_fmt.type, true) < 0) {
		request_log("Failed to set stream on dst type %d\n", mbc->dst_fmt.type);
		v4l2_set_stream(mbc->vfd, mbc->src_fmt.type, false);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	mbc->stream_on = true;
	return VA_STATUS_SUCCESS;
}

VAStatus mediabufs_stream_off(struct mediabufs_ctl *const mbc)
{
	VAStatus status = VA_STATUS_SUCCESS;

	if (!mbc->stream_on)
		return VA_STATUS_SUCCESS;

	if (v4l2_set_stream(mbc->vfd, mbc->src_fmt.type, false) < 0) {
		request_log("Failed to set stream off src type %d\n", mbc->src_fmt.type);
		status = VA_STATUS_ERROR_OPERATION_FAILED;
	}

	if (v4l2_set_stream(mbc->vfd, mbc->dst_fmt.type, false) < 0) {
		request_log("Failed to set stream off dst type %d\n", mbc->dst_fmt.type);
		status = VA_STATUS_ERROR_OPERATION_FAILED;
	}

	return status;
}

VAStatus mediabufs_set_ext_ctrl(struct mediabufs_ctl *const mbc,
				struct media_request * const mreq,
				unsigned int id, void *data,
				unsigned int size)
{
	int rv = v4l2_set_control(mbc->vfd, mreq, id, data, size);
	return !rv ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_OPERATION_FAILED;
}

VAStatus mediabufs_src_fmt_set(struct mediabufs_ctl *const mbc,
			       const uint32_t pixfmt,
			       const uint32_t width, const uint32_t height)
{
	mbc->src_fmt.fmt.pix = (struct v4l2_pix_format){
		.width = width,
		.height = height,
		.pixelformat = pixfmt
	};

	while (ioctl(mbc->vfd, VIDIOC_S_FMT, &mbc->src_fmt))
		if (errno != EINTR) {
			request_log("Failed to set format %#x %dx%d\n", pixfmt, width, height);
			return VA_STATUS_ERROR_OPERATION_FAILED;
		}

	return VA_STATUS_SUCCESS;
}

static void mediabufs_ctl_delete(struct mediabufs_ctl *const mbc)
{
	if (!mbc)
		return;

	polltask_delete(&mbc->pt);

	mediabufs_stream_off(mbc);

	/* Empty v4l2 buffer stash */
	v4l2_request_buffers(mbc->vfd, mbc->src_fmt.type, 0);
	v4l2_request_buffers(mbc->vfd, mbc->dst_fmt.type, 0);

	queue_delete(mbc->dst);
	queue_delete(mbc->src);
	close(mbc->vfd);
	pthread_mutex_destroy(&mbc->lock);

	free(mbc);
}

void mediabufs_ctl_ref(struct mediabufs_ctl *const mbc)
{
	atomic_fetch_add(&mbc->ref_count, 1);
}

void mediabufs_ctl_unref(struct mediabufs_ctl **const pmbc)
{
	struct mediabufs_ctl *const mbc = *pmbc;
	int n;

	if (!mbc)
		return;
	*pmbc = NULL;
	n = atomic_fetch_sub(&mbc->ref_count, 1);
	if (n)
		return;
	mediabufs_ctl_delete(mbc);
}


/* One of these per context */
struct mediabufs_ctl * mediabufs_ctl_new(const VADriverContextP dc, const char * vpath, struct pollqueue *const pq)
{
	struct mediabufs_ctl *const mbc = calloc(1, sizeof(*mbc));

	if (!mbc)
		return NULL;

	mbc->dc = dc;
	mbc->src_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	mbc->dst_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	mbc->pq = pq;
	pthread_mutex_init(&mbc->lock, NULL);

	mbc->vfd = open(vpath, O_RDWR);
	if (mbc->vfd == -1) {
		request_err(dc, "Failed to open video dev '%s': %s\n", vpath, strerror(errno));
		goto fail0;
	}

	mbc->src = queue_new(mbc->vfd, pq, mbc->src_fmt.type);
	if (!mbc->src)
		goto fail1;
	/* Default cap type to mono-planar */
	mbc->dst = queue_new(mbc->vfd, pq, mbc->dst_fmt.type);
	if (!mbc->dst)
		goto fail2;
	mbc->pt = polltask_new(mbc->vfd, POLLIN | POLLOUT, mediabufs_poll_cb, mbc);
	if (!mbc->pt)
		goto fail3;

	/* Cannot add polltask now - polling with nothing pending
	 * generates infinite error polls
	*/
	return mbc;

fail3:
	queue_delete(mbc->dst);
fail2:
	queue_delete(mbc->src);
fail1:
	close(mbc->vfd);
fail0:
	free(mbc);
	request_info(dc, "%s: FAILED\n", __func__);
	return NULL;
}



