/*
e.h
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
struct media_request;
struct media_pool;

struct media_pool * media_pool_new(const char * const media_path,
				   struct pollqueue * const pq,
				   const unsigned int n);
void media_pool_delete(struct media_pool * mp);

struct media_request * media_request_get(struct media_pool * const mp);
int media_request_fd(const struct media_request * const req);
int media_request_start(struct media_request * const req);


struct mediabufs_ctl;
struct qent_src;
struct qent_dst;
struct dmabufs_ctrl;

int qent_src_params_set(struct qent_src *const be, const struct timeval * timestamp);
int qent_src_data_copy(struct qent_src *const be, const void *const src, const size_t len);
int qent_dst_dup_fd(const struct qent_dst *const be, unsigned int plane);
VAStatus qent_dst_wait(struct qent_dst *const be);
void qent_dst_delete(struct qent_dst *const be);
const uint8_t * qent_dst_data(struct qent_dst *const be, unsigned int buf_no);
VAStatus qent_dst_read_start(struct qent_dst *const be);
VAStatus qent_dst_read_stop(struct qent_dst *const be);

VAStatus mediabufs_start_request(struct mediabufs_ctl *const mbc,
			    struct media_request *const mreq,
			    struct qent_src *const src_be,
			    struct qent_dst *const dst_be,
			    const bool is_final);
struct qent_dst* mediabufs_dst_qent_alloc(struct mediabufs_ctl *const mbc,
					       struct dmabufs_ctrl *const dbsc);

VAStatus mediabufs_stream_on(struct mediabufs_ctl *const mbc);
VAStatus mediabufs_stream_off(struct mediabufs_ctl *const mbc);
const struct v4l2_format *mediabufs_dst_fmt(struct mediabufs_ctl *const mbc);
VAStatus mediabufs_dst_fmt_set(struct mediabufs_ctl *const mbc,
			   const unsigned int rtfmt,
			   const unsigned int width,
			   const unsigned int height);
struct qent_src *mediabufs_src_qent_get(struct mediabufs_ctl *const mbc);
VAStatus mediabufs_set_ext_ctrl(struct mediabufs_ctl *const mbc,
				struct media_request * const mreq,
				unsigned int id, void *data,
				unsigned int size);
VAStatus mediabufs_src_fmt_set(struct mediabufs_ctl *const mbc,
			       const uint32_t pixfmt,
			       const uint32_t width, const uint32_t height);
VAStatus mediabufs_src_pool_create(struct mediabufs_ctl *const rw,
			      struct dmabufs_ctrl * const dbsc,
			      unsigned int n);
struct mediabufs_ctl * mediabufs_ctl_new(const VADriverContextP dc,
					 const char *vpath, struct pollqueue *const pq);
void mediabufs_ctl_unref(struct mediabufs_ctl **const pmbc);
void mediabufs_ctl_ref(struct mediabufs_ctl *const mbc);


#endif
