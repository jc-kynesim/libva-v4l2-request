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
#include <sys/ioctl.h>

#include <drm_fourcc.h>
#include <linux/videodev2.h>
#include <va/va.h>

#include "dmabufs.h"
#include "utils.h"
#include "v4l2.h"
#include "media.h"

static bool v4l2_type_is_output(unsigned int type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		return true;

	default:
		return false;
	}
}

static bool v4l2_type_is_mplane(unsigned int type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		return true;

	default:
		return false;
	}
}

unsigned int v4l2_type_video_output(bool mplane)
{
	return mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE :
			V4L2_BUF_TYPE_VIDEO_OUTPUT;
}

unsigned int v4l2_type_video_capture(bool mplane)
{
	return mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
			V4L2_BUF_TYPE_VIDEO_CAPTURE;
}

int v4l2_query_capabilities(int video_fd, unsigned int *capabilities)
{
	struct v4l2_capability capability;
	int rc;

	memset(&capability, 0, sizeof(capability));

	rc = ioctl(video_fd, VIDIOC_QUERYCAP, &capability);
	if (rc < 0)
		return -1;

	if (capabilities != NULL) {
		if ((capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0)
			*capabilities = capability.device_caps;
		else
			*capabilities = capability.capabilities;
	}

	return 0;
}

static void v4l2_setup_format(struct v4l2_format *format, unsigned int type,
			      unsigned int width, unsigned int height,
			      unsigned int pixelformat)
{
	unsigned int sizeimage;

	memset(format, 0, sizeof(*format));
	format->type = type;

	sizeimage = v4l2_type_is_output(type) ? SOURCE_SIZE_MAX : 0;

	if (v4l2_type_is_mplane(type)) {
		format->fmt.pix_mp.width = width;
		format->fmt.pix_mp.height = height;
		format->fmt.pix_mp.plane_fmt[0].sizeimage = sizeimage;
		format->fmt.pix_mp.pixelformat = pixelformat;
	} else {
		format->fmt.pix.width = width;
		format->fmt.pix.height = height;
		format->fmt.pix.sizeimage = sizeimage;
		format->fmt.pix.pixelformat = pixelformat;
	}
}

bool v4l2_find_format(int video_fd, unsigned int type, unsigned int pixelformat)
{
	struct v4l2_fmtdesc fmtdesc;
	int rc;

	memset(&fmtdesc, 0, sizeof(fmtdesc));
	fmtdesc.type = type;
	fmtdesc.index = 0;

	do {
		rc = ioctl(video_fd, VIDIOC_ENUM_FMT, &fmtdesc);
		if (rc < 0)
			break;

		if (fmtdesc.pixelformat == pixelformat)
			return true;

		fmtdesc.index++;
	} while (rc >= 0);

	return false;
}

static int v4l2_pix_to_picdesc(struct picdesc * pd, const struct v4l2_pix_format * fmt)
{
	pd->type_v4l2 = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	pd->fmt_v4l2 = fmt->pixelformat;
	pd->buffer_count = 1;

	switch (fmt->pixelformat) {
	case V4L2_PIX_FMT_NV12_COL128:
		pd->fmt_drm = DRM_FORMAT_NV12;
		pd->rtfmt_vaapi = VA_RT_FORMAT_YUV420;
		pd->fmt_vaapi = VA_FOURCC_NV12;
		pd->plane_count = 2;
		pd->is_linear = false;
		pd->bufs[0] = (struct bufdesc){
			.size = fmt->sizeimage,
			.drm_mod = DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT(
			      fmt->bytesperline)
		};
		pd->planes[0] = (struct planedesc){
			.buf = 0,
			.width = fmt->width,
			.height = fmt->height,
			.stride = fmt->width, /* Lies */
			.col_height = fmt->bytesperline,
			.offset = 0
		};
		pd->planes[1] = (struct planedesc){
			.buf = 0,
			.width = fmt->width / 2,
			.height = fmt->height / 2,
			.stride = fmt->width, /* Lies */
			.col_height = fmt->bytesperline,
			.offset = fmt->height * 128
		};
		break;

	case V4L2_PIX_FMT_NV12_10_COL128:
		pd->fmt_drm = DRM_FORMAT_P030;
		pd->rtfmt_vaapi = VA_RT_FORMAT_YUV420_10;
		pd->fmt_vaapi = VA_FOURCC_P010;
		pd->plane_count = 2;
		pd->is_linear = false;
		pd->bufs[0] = (struct bufdesc){
			.size = fmt->sizeimage,
			.drm_mod = DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT(
			      fmt->bytesperline)
		};
		pd->planes[0] = (struct planedesc){
			.buf = 0,
			.width = fmt->width,
			.height = fmt->height,
			.stride = fmt->width * 4 / 3, /* Lies */
			.col_height = fmt->bytesperline,
			.offset = 0
		};
		pd->planes[1] = (struct planedesc){
			.buf = 0,
			.width = fmt->width / 2,
			.height = fmt->height / 2,
			.stride = fmt->width * 4 / 3, /* Lies */
			.col_height = fmt->bytesperline,
			.offset = fmt->height * 128
		};
		break;

	case V4L2_PIX_FMT_NV12:
		pd->fmt_drm = DRM_FORMAT_NV12;
		pd->rtfmt_vaapi = VA_RT_FORMAT_YUV420;
		pd->fmt_vaapi = VA_FOURCC_NV12;
		pd->plane_count = 2;
		pd->is_linear = true;
		pd->bufs[0] = (struct bufdesc){
			.size = fmt->sizeimage,
			.drm_mod = DRM_FORMAT_MOD_NONE
		};
		pd->planes[0] = (struct planedesc){
			.buf = 0,
			.width = fmt->width,
			.height = fmt->height,
			.stride = fmt->bytesperline,
			.offset = 0
		};
		pd->planes[1] = (struct planedesc){
			.buf = 0,
			.width = fmt->width / 2,
			.height = fmt->height / 2,
			.stride = fmt->bytesperline,
			.offset = fmt->height * fmt->bytesperline
		};
		break;

	default:
		return -1;
	}
	return 0;
}

static int v4l2_pix_mp_to_picdesc(struct picdesc * pd, const struct v4l2_pix_format_mplane * fmt)
{
	pd->fmt_v4l2 = fmt->pixelformat;
	pd->type_v4l2 = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	return -1;
}

int v4l2_format_to_picdesc(struct picdesc * pd, const struct v4l2_format * fmt)
{
	switch (fmt->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		return v4l2_pix_to_picdesc(pd, &fmt->fmt.pix);
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		return v4l2_pix_mp_to_picdesc(pd, &fmt->fmt.pix_mp);
	default:
		break;
	}
	return -1;
}


int v4l2_try_picdesc(struct picdesc * pd,
		     int video_fd, unsigned int type, unsigned int width,
		     unsigned int height, unsigned int pixelformat)
{
	struct v4l2_format fmt;
	int rc;

	*pd = (struct picdesc){
		.req_width = width,
		.req_height = height,
	};

	v4l2_setup_format(&fmt, type, width, height, pixelformat);

	rc = ioctl(video_fd, VIDIOC_TRY_FMT, &fmt);
	if (rc < 0) {
		request_log("Unable to try format for type %d: %s\n", type,
			    strerror(errno));
		return -1;
	}

	return v4l2_format_to_picdesc(pd, &fmt);
}

int v4l2_get_picdesc(struct picdesc * pd,
		     int video_fd, unsigned int captype)
{
	struct v4l2_format fmt = {
		.type = captype
	};
	int rc;

	rc = ioctl(video_fd, VIDIOC_G_FMT, &fmt);
	if (rc < 0) {
		request_log("Unable to get format for type %d: %s\n",
			    captype, strerror(errno));
		return -1;
	}

	return v4l2_format_to_picdesc(pd, &fmt);
}

static VAStatus find_picdesc_flags(struct picdesc * pd,
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
		while (!ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc)) {
			if (errno != EINTR)
				return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
		}
		if (!(fmtdesc.flags & flags_must) ||
		    (fmtdesc.flags & flags_not))
			continue;
		status = video_fmt_supported(fmtdesc.pixelformat,
					     fmtdesc.type, rtfmt);
		if (status == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT)
			return status;
		if (status != VA_STATUS_SUCCESS)
			continue;
		if (!v4l2_try_picdesc(pd, fd,
				      V4L2_BUF_TYPE_VIDEO_CAPTURE,
				      width, height, fmtdesc.pixelformat))
			return VA_STATUS_SUCCESS;
	}
	return 0;
}

VAStatus v4l2_find_picdesc(struct picdesc * const pd, const int fd,
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
		status = find_picdesc_flags(pd, fd, rtfmt,
					    trys[i].type_v4l2,
					    trys[i].flags_must,
					    trys[i].flags_not,
					    width, height);
		if (status != VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE)
			return status;
	}
	return status;
}


int v4l2_try_format(int video_fd, unsigned int type, unsigned int width,
		    unsigned int height, unsigned int pixelformat)
{
	struct v4l2_format format;
	int rc;

	v4l2_setup_format(&format, type, width, height, pixelformat);

	rc = ioctl(video_fd, VIDIOC_TRY_FMT, &format);
	if (rc < 0) {
		request_log("Unable to try format for type %d: %s\n", type,
			    strerror(errno));
		return -1;
	}

	return 0;
}

int v4l2_set_format(int video_fd, unsigned int type, unsigned int pixelformat,
		    unsigned int width, unsigned int height)
{
	struct v4l2_format format;
	int rc;

	v4l2_setup_format(&format, type, width, height, pixelformat);

	rc = ioctl(video_fd, VIDIOC_S_FMT, &format);
	if (rc < 0) {
		request_log("Unable to set format for type %d: %s\n", type,
			    strerror(errno));
		return -1;
	}

	request_log("%s: type=%d/%d, width=%d/%d, height=%d/%d, pixfmt=%#x/%#x, size=%#x\n",
		    __func__, type, format.type, width, format.fmt.pix.width,
		    height, format.fmt.pix.height, pixelformat, format.fmt.pix.pixelformat,
		    format.fmt.pix.sizeimage);

	return 0;
}

int v4l2_get_format(int video_fd, unsigned int type, unsigned int *width,
		    unsigned int *height, unsigned int *bytesperline,
		    unsigned int *sizes, unsigned int *planes_count)
{
	struct v4l2_format format;
	unsigned int count;
	unsigned int i;
	int rc;

	memset(&format, 0, sizeof(format));
	format.type = type;

	rc = ioctl(video_fd, VIDIOC_G_FMT, &format);
	if (rc < 0) {
		request_log("Unable to get format for type %d: %s\n", type,
			    strerror(errno));
		return -1;
	}

	if (v4l2_type_is_mplane(type)) {
		count = format.fmt.pix_mp.num_planes;

		if (width != NULL)
			*width = format.fmt.pix_mp.width;

		if (height != NULL)
			*height = format.fmt.pix_mp.height;

		if (planes_count != NULL)
			if (*planes_count > 0 && *planes_count < count)
				count = *planes_count;

		if (bytesperline != NULL)
			for (i = 0; i < count; i++)
				bytesperline[i] =
					format.fmt.pix_mp.plane_fmt[i].bytesperline;

		if (sizes != NULL)
			for (i = 0; i < count; i++)
				sizes[i] = format.fmt.pix_mp.plane_fmt[i].sizeimage;

		if (planes_count != NULL)
			*planes_count = count;
	} else {
		if (width != NULL)
			*width = format.fmt.pix.width;

		if (height != NULL)
			*height = format.fmt.pix.height;

		if (bytesperline != NULL)
			bytesperline[0] = format.fmt.pix.bytesperline;

		if (sizes != NULL)
			sizes[0] = format.fmt.pix.sizeimage;

		if (planes_count != NULL)
			*planes_count = 1;

		request_log("Format=%#x/%#x, width=%d, height=%d, bpl=%d, size=%#x\n",
			    type, format.type, *width, *height, *bytesperline, *sizes);
	}

	return 0;
}

int v4l2_create_buffers(int video_fd, unsigned int type,
			enum v4l2_memory memory,
			unsigned int buffers_count, unsigned int *index_base)
{
	struct v4l2_create_buffers buffers;
	int rc;

	memset(&buffers, 0, sizeof(buffers));
	buffers.format.type = type;
	buffers.memory = memory;
	buffers.count = buffers_count;

	rc = ioctl(video_fd, VIDIOC_G_FMT, &buffers.format);
	if (rc < 0) {
		request_log("Unable to get format for type %d: %s\n", type,
			    strerror(errno));
		return -1;
	}

	rc = ioctl(video_fd, VIDIOC_CREATE_BUFS, &buffers);
	if (rc < 0) {
		request_log("Unable to create buffer for type %d: %s\n", type,
			    strerror(errno));
		return -1;
	}

	if (index_base != NULL)
		*index_base = buffers.index;

	return 0;
}

int v4l2_query_buffer(int video_fd, unsigned int type, unsigned int index,
		      unsigned int *lengths, unsigned int *offsets,
		      unsigned int buffers_count)
{
	struct v4l2_plane planes[buffers_count];
	struct v4l2_buffer buffer;
	unsigned int i;
	int rc;

	memset(planes, 0, sizeof(planes));
	memset(&buffer, 0, sizeof(buffer));

	buffer.type = type;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.index = index;
	buffer.length = buffers_count;
	buffer.m.planes = planes;

	rc = ioctl(video_fd, VIDIOC_QUERYBUF, &buffer);
	if (rc < 0) {
		request_log("Unable to query buffer: %s\n", strerror(errno));
		return -1;
	}

	if (v4l2_type_is_mplane(type)) {
		if (lengths != NULL)
			for (i = 0; i < buffer.length; i++)
				lengths[i] = buffer.m.planes[i].length;

		if (offsets != NULL)
			for (i = 0; i < buffer.length; i++)
				offsets[i] = buffer.m.planes[i].m.mem_offset;
	} else {
		if (lengths != NULL)
			lengths[0] = buffer.length;

		if (offsets != NULL)
			offsets[0] = buffer.m.offset;
	}

	return 0;
}

int v4l2_request_buffers(int video_fd, unsigned int type,
			 unsigned int buffers_count)
{
	struct v4l2_requestbuffers buffers;
	int rc;

	memset(&buffers, 0, sizeof(buffers));
	buffers.type = type;
	buffers.memory = V4L2_MEMORY_MMAP;
	buffers.count = buffers_count;

	rc = ioctl(video_fd, VIDIOC_REQBUFS, &buffers);
	if (rc < 0) {
		request_log("Unable to request buffers: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

int v4l2_queue_buffer(int video_fd,
		      struct media_request *const mreq, unsigned int type,
		      struct timeval *timestamp, unsigned int index,
		      unsigned int size, unsigned int buffers_count,
		      bool hold_flag)
{
	struct v4l2_plane planes[buffers_count];
	struct v4l2_buffer buffer;
	unsigned int i;
	int rc;

	memset(planes, 0, sizeof(planes));
	memset(&buffer, 0, sizeof(buffer));

	buffer.type = type;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.index = index;
	buffer.length = buffers_count;
	buffer.m.planes = planes;

	for (i = 0; i < buffers_count; i++)
		if (v4l2_type_is_mplane(type))
			buffer.m.planes[i].bytesused = size;
		else
			buffer.bytesused = size;

	if (mreq) {
		buffer.flags = V4L2_BUF_FLAG_REQUEST_FD;
		buffer.request_fd = media_request_fd(mreq);
		if (hold_flag)
			buffer.flags |= V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;
	}

	if (timestamp != NULL)
		buffer.timestamp = *timestamp;

	rc = ioctl(video_fd, VIDIOC_QBUF, &buffer);
	if (rc < 0) {
		request_log("Unable to queue buffer: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

int v4l2_queue_dmabuf(int video_fd,
		      struct media_request *const mreq,
		      struct dmabuf_h *const dh,
		      unsigned int type,
		      struct timeval *timestamp, unsigned int index,
		      unsigned int size, unsigned int buffers_count,
		      bool hold_flag)
{
	struct v4l2_plane planes[buffers_count];
	struct v4l2_buffer buffer;
	int rc;

	memset(planes, 0, sizeof(planes));
	memset(&buffer, 0, sizeof(buffer));

	buffer.type = type;
	buffer.memory = V4L2_MEMORY_DMABUF;
	buffer.index = index;
	buffer.length = dmabuf_size(dh);
	buffer.m.fd = dmabuf_fd(dh);

	buffer.bytesused = size;

	if (mreq) {
		buffer.flags = V4L2_BUF_FLAG_REQUEST_FD;
		buffer.request_fd = media_request_fd(mreq);
		if (hold_flag)
			buffer.flags |= V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;
	}

	if (timestamp != NULL)
		buffer.timestamp = *timestamp;

	rc = ioctl(video_fd, VIDIOC_QBUF, &buffer);
	if (rc < 0) {
		request_log("Unable to queue buffer: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}


int v4l2_dequeue_buffer(int video_fd, int request_fd, unsigned int type,
			unsigned int index, unsigned int buffers_count)
{
	struct v4l2_plane planes[buffers_count];
	struct v4l2_buffer buffer;
	int rc;

	memset(planes, 0, sizeof(planes));
	memset(&buffer, 0, sizeof(buffer));

	buffer.type = type;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.index = index;
	buffer.length = buffers_count;
	buffer.m.planes = planes;

	if (request_fd >= 0) {
		buffer.flags = V4L2_BUF_FLAG_REQUEST_FD;
		buffer.request_fd = request_fd;
	}

	rc = ioctl(video_fd, VIDIOC_DQBUF, &buffer);
	if (rc < 0) {
		request_log("Unable to dequeue buffer: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

int v4l2_export_buffer(int video_fd, unsigned int type, unsigned int index,
		       unsigned int flags, int *export_fds,
		       unsigned int export_fds_count)
{
	struct v4l2_exportbuffer exportbuffer;
	unsigned int i;
	int rc;

	for (i = 0; i < export_fds_count; i++) {
		memset(&exportbuffer, 0, sizeof(exportbuffer));
		exportbuffer.type = type;
		exportbuffer.index = index;
		exportbuffer.plane = i;
		exportbuffer.flags = flags;

		rc = ioctl(video_fd, VIDIOC_EXPBUF, &exportbuffer);
		if (rc < 0) {
			request_log("Unable to export buffer: %s\n",
				    strerror(errno));
			return -1;
		}

		export_fds[i] = exportbuffer.fd;
	}

	return 0;
}

int v4l2_set_control(int video_fd,
		     struct media_request * const mreq,
		     unsigned int id, void *data,
		     unsigned int size)
{
	struct v4l2_ext_control control;
	struct v4l2_ext_controls controls;
	int rc;

	memset(&control, 0, sizeof(control));
	memset(&controls, 0, sizeof(controls));

	control.id = id;
	control.ptr = data;
	control.size = size;

	controls.controls = &control;
	controls.count = 1;

	if (mreq) {
		controls.which = V4L2_CTRL_WHICH_REQUEST_VAL;
		controls.request_fd = media_request_fd(mreq);
	}

	rc = ioctl(video_fd, VIDIOC_S_EXT_CTRLS, &controls);
	if (rc < 0) {
		request_log("Unable to set control %d: %s\n", id, strerror(errno));
		return -1;
	}

	return 0;
}

int v4l2_set_stream(int video_fd, unsigned int type, bool enable)
{
	enum v4l2_buf_type buf_type = type;
	int rc;

	rc = ioctl(video_fd, enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF,
		   &buf_type);
	if (rc < 0) {
		request_log("Unable to %sable stream: %s\n",
			    enable ? "en" : "dis", strerror(errno));
		return -1;
	}

	return 0;
}
