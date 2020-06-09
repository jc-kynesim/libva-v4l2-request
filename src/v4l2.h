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

#ifndef _V4L2_H_
#define _V4L2_H_

#include <stdint.h>
#include <stdbool.h>

#define SOURCE_SIZE_MAX						(1024 * 1024)

struct media_request;
struct dmabuf_h;

unsigned int v4l2_type_video_output(bool mplane);
unsigned int v4l2_type_video_capture(bool mplane);
int v4l2_query_capabilities(int video_fd, unsigned int *capabilities);
bool v4l2_find_format(int video_fd, unsigned int type,
		      unsigned int pixelformat);
int v4l2_set_format(int video_fd, unsigned int type, unsigned int pixelformat,
		    unsigned int width, unsigned int height);
int v4l2_get_format(int video_fd, unsigned int type, unsigned int *width,
		    unsigned int *height, unsigned int *bytesperline,
		    unsigned int *sizes, unsigned int *planes_count);
int v4l2_create_buffers(int video_fd, unsigned int type,
			enum v4l2_memory memory,
			unsigned int buffers_count, unsigned int *index_base);
int v4l2_query_buffer(int video_fd, unsigned int type, unsigned int index,
		      unsigned int *lengths, unsigned int *offsets,
		      unsigned int buffers_count);
int v4l2_request_buffers(int video_fd, unsigned int type,
			 unsigned int buffers_count);
int v4l2_queue_buffer(int video_fd, struct media_request *const mreq,
		      unsigned int type,
		      struct timeval *timestamp, unsigned int index,
		      unsigned int size, unsigned int buffers_count,
		      bool hold_flag);
int v4l2_queue_dmabuf(int video_fd, struct media_request *const mreq,
		      struct dmabuf_h *const dh,
		      unsigned int type,
		      struct timeval *timestamp, unsigned int index,
		      unsigned int size, unsigned int buffers_count,
		      bool hold_flag);
int v4l2_dequeue_buffer(int video_fd, int request_fd, unsigned int type,
			unsigned int index, unsigned int buffers_count);
int v4l2_export_buffer(int video_fd, unsigned int type, unsigned int index,
		       unsigned int flags, int *export_fds,
		       unsigned int export_fds_count);
int v4l2_set_control(int video_fd,
		     struct media_request * const mreq,
		     unsigned int id, void *data, unsigned int size);
int v4l2_set_stream(int video_fd, unsigned int type, bool enable);

enum v4l2_buf_type;

struct picdesc {
	/* Requested values */
	unsigned int req_width;
	unsigned int req_height;
	unsigned int req_rtfmt;

	/* Actual values */
	uint32_t     fmt_v4l2;  /* V4l2 actual format */
	enum v4l2_buf_type type_v4l2; /* V4L2 format type */
	unsigned int fmt_drm;   /* DRM export format */
	unsigned int rtfmt_vaapi; /* Vaapi RT format */
	unsigned int fmt_vaapi; /* Vaapi export format, may need conversion */
	unsigned int buffer_count;
	unsigned int plane_count;
	bool is_linear;
	struct bufdesc {
		size_t size;
		uint64_t drm_mod;
	} bufs[VIDEO_MAX_PLANES];
	struct planedesc {
		unsigned int buf;
		unsigned int width;
		unsigned int height;
		unsigned int col_height;
		size_t stride;
		size_t offset;
	} planes[VIDEO_MAX_PLANES];
};

int v4l2_format_to_picdesc(struct picdesc * pd, const struct v4l2_format * fmt);

int v4l2_try_picdesc(struct picdesc * pd,
		     int video_fd, unsigned int type, unsigned int width,
		     unsigned int height, unsigned int pixelformat);
int v4l2_get_picdesc(struct picdesc * pd,
		     int video_fd, unsigned int captype);


#endif
