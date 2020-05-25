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

#ifndef _MEDIA_H_
#define _MEDIA_H_

struct pollqueue;
struct polltask;

struct polltask *polltask_new(const int fd, const short events,
			      void (*fn)(void *v, short revents), void * v);
void polltask_delete(struct polltask * pt);

void pollqueue_add_task(struct pollqueue *const pq, struct polltask *const pt);
int pollqueue_poll(struct pollqueue *const pq, const int timeout);
struct pollqueue * pollqueue_new(void);
void pollqueue_delete(struct pollqueue * pq);

struct media_request;
struct media_pool;

struct media_pool * media_pool_new(const char * const media_path,
				   struct pollqueue * const pq,
				   const unsigned int n);
void media_pool_delete(struct media_pool * mp);

struct media_request * media_request_get(struct media_pool * const mp);
int media_request_fd(const struct media_request * const req);
int media_request_start(struct media_request * const req);


#endif
