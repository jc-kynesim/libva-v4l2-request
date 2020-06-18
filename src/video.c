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

#include <h264-ctrls.h>
#include <hevc-ctrls.h>
#include <mpeg2-ctrls.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include "drm_fourcc.h"
#include "utils.h"
#include "video.h"

uint32_t video_profile_to_src_pixfmt(const VAProfile profile)
{
	switch (profile) {

	case VAProfileMPEG2Simple:
	case VAProfileMPEG2Main:
		return V4L2_PIX_FMT_MPEG2_SLICE;

	case VAProfileH264Main:
	case VAProfileH264High:
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264MultiviewHigh:
	case VAProfileH264StereoHigh:
		return V4L2_PIX_FMT_H264_SLICE_RAW;

	case VAProfileHEVCMain:
	case VAProfileHEVCMain10:
		return V4L2_PIX_FMT_HEVC_SLICE;

	default:
		break;
	}
	return 0;
}

VAStatus video_fmt_supported(const uint32_t fmt_v4l2,
			     const enum v4l2_buf_type type_v4l2,
			     const unsigned int rtfmt)
{
	switch (rtfmt) {
	case VA_RT_FORMAT_YUV420:
		switch (type_v4l2) {
		case V4L2_BUF_TYPE_VIDEO_CAPTURE:
			switch (fmt_v4l2) {
			case V4L2_PIX_FMT_NV12:
			case V4L2_PIX_FMT_NV12_COL128:
				return VA_STATUS_SUCCESS;
			default:
				return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
			}
		case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		default:
			return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
		}
	case VA_RT_FORMAT_YUV420_10:
		switch (type_v4l2) {
		case V4L2_BUF_TYPE_VIDEO_CAPTURE:
			switch (fmt_v4l2) {
			case V4L2_PIX_FMT_NV12_10_COL128:
				return VA_STATUS_SUCCESS;
			default:
				return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
			}
		case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		default:
			return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
		}
	default:
		break;
	}
	return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
}


