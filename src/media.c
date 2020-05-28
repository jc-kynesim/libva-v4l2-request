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
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <semaphore.h>
#include <pthread.h>

#include <va/va_backend.h>
#include <linux/media.h>
#include "media.h"
#include "utils.h"

struct pollqueue;
struct polltask {
	struct polltask *next;
	struct polltask *prev;
	struct pollqueue *q;

	int fd;
	short events;

	void (*fn)(void *v, short revents);
	void * v;
};

struct pollqueue {
	struct polltask *head;
	struct polltask *tail;

	unsigned int n;
	bool modified;
	unsigned int asize;
	struct pollfd *a;
};

struct polltask *polltask_new(const int fd, const short events,
			      void (*fn)(void *v, short revents), void * v)
{
	struct polltask *const pt = malloc(sizeof(*pt));
	if (!pt)
		return NULL;

	*pt = (struct polltask){
		.next = NULL,
		.prev = NULL,
		.fd = fd,
		.events = events,
		.fn = fn,
		.v = v
	};
	return pt;
}

static void pollqueue_rem_task(struct pollqueue *const pq, struct polltask *const pt)
{
	if (pt->prev)
		pt->prev->next = pt->next;
	else
		pq->head = pt->next;
	if (pt->next)
		pt->next->prev = pt->prev;
	else
		pq->tail = pt->prev;
	pt->next = NULL;
	pt->prev = NULL;
	pt->q = NULL;
	--pq->n;
	pq->modified = true;
}

void polltask_delete(struct polltask * pt)
{
	if (pt->q)
		pollqueue_rem_task(pt->q, pt);
	free(pt);
}

void pollqueue_add_task(struct pollqueue *const pq, struct polltask *const pt)
{
	if (pq->tail)
		pq->tail->next = pt;
	else
		pq->head = pt;
	pt->prev = pq->tail;
	pt->next = NULL;
	pt->q = pq;
	pq->tail = pt;
	++pq->n;
	pq->modified = true;
}

/*
 * -ve error
 * 0   timedout or nothing to do
 * +ve did something
 *
 * *** All of this lot is single-thread only
*/
int pollqueue_poll(struct pollqueue *const pq, int timeout)
{
	int t = 0;
	int rv;

	if (!pq->n)
		return 0;

	do {
		unsigned int i;
		struct polltask *pt;
		int evt;

		if (pq->modified) {
			if (pq->asize < pq->n) {
				free(pq->a);
				pq->asize = pq->asize ? pq->asize * 2 : 4;
				pq->a = malloc(pq->asize * sizeof(*pq->a));
				if (!pq->a) {
					pq->asize = 0;
					return -ENOMEM;
				}
			}

			for (pt = pq->head, i = 0; pt; pt = pt->next, ++i) {
				pq->a[i] = (struct pollfd){
					.fd = pt->fd,
					.events = pt->events
				};
			}
			pq->modified = false;
		}

		while ((rv = poll(pq->a, pq->n, timeout)) == -1) {
			if (errno != EINTR)
				return -errno;
		}

		/* Safe chain follow - adds are safe too as they add to tail */
		pt = pq->head;
		for (i = 0, evt = 0; evt < rv; ++i) {
			struct polltask *const pt_next = pt->next;

			if (pq->a[i].revents) {
				++evt;
				pollqueue_rem_task(pq, pt);
				pt->fn(pt->v, pq->a[i].revents);
			}

			pt = pt_next;
		}

		t += rv;
	} while (rv > 0);

	return t;  /* return total number of things done */
}

struct pollqueue * pollqueue_new(void)
{
	struct pollqueue *pq = malloc(sizeof(*pq));
	if (!pq)
		return NULL;
	*pq = (struct pollqueue){
		.head = NULL,
		.tail = NULL
	};
	return pq;
}

void pollqueue_delete(struct pollqueue * pt)
{
	if (!pt)
		return;
	free(pt->a);
	free(pt);
}


struct media_pool {
	int fd;
	struct media_request * free_reqs;

	struct pollqueue * pq;
};

struct media_request {
	struct media_request * next;
	struct media_pool * mp;
	int fd;
	struct polltask * pt;
};


static struct media_request * mq_get_free(struct media_pool * const mp)
{
	struct media_request *const req = mp->free_reqs;
	if (req) {
		mp->free_reqs = req->next;
		req->next = NULL;
	}
	return req;
}

static void mq_put_free(struct media_pool *const mp, struct media_request *const req)
{
	req->next = mp->free_reqs;
	mp->free_reqs = req;
}

struct media_request * media_request_get(struct media_pool * const mp)
{
	struct media_request *req;

	/* Process anything pending */
	pollqueue_poll(mp->pq, 0);

	while (!(req = mq_get_free(mp))) {
		/* Wait for stuff to happen */
		if (pollqueue_poll(mp->pq, 2000) <= 0)
			return NULL;
	}

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

	pollqueue_add_task(mp->pq, req->pt);
	return 0;
}

static void media_request_done(void *v, short revents)
{
	struct media_request *const req = v;

	if (ioctl(req->fd, MEDIA_REQUEST_IOC_REINIT, NULL) < 0)
		request_log("Unable to reinit media request: %s\n",
			    strerror(errno));

	mq_put_free(req->mp, req);
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
	mp->fd = open(media_path, O_RDWR | O_NONBLOCK);
	if (mp->fd == -1)
		goto fail1;

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

		if (ioctl(mp->fd, MEDIA_IOC_REQUEST_ALLOC, &req->fd) == -1)
			goto fail4;

		req->pt = polltask_new(req->fd, POLLPRI, media_request_done, req);
		if (!req->pt)
			goto fail4;
	}

	return mp;

fail4:
	delete_req_chain(mp->free_reqs);
	close(mp->fd);
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
	free(mp);
}


