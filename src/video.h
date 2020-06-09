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

#ifndef _VIDEO_H_
#define _VIDEO_H_

#include <stdint.h>
#include <stdbool.h>
#include <va/va.h>

enum v4l2_buf_type;

struct video_format {
	char *description;
	unsigned int v4l2_format;
	unsigned int v4l2_buffers_count;
	bool v4l2_mplane;
	unsigned int drm_format;
	uint64_t drm_modifier;
	unsigned int planes_count;
	unsigned int bpp;
};

struct video_format *video_format_find(unsigned int pixelformat);
bool video_format_is_linear(struct video_format *format);

/*
 * Returns:
 *  VA_STATUS_SUCCESS                           Supported
 *  VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE      Unsupported buffer for RT
 *  VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT       Bad RT format
*/
VAStatus video_fmt_supported(const uint32_t fmt_v4l2,
			     const enum v4l2_buf_type type_v4l2,
			     const unsigned int rtfmt);


#endif
