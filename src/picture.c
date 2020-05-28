/*
 * Copyright (C) 2007 Intel Corporation
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
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

#include "picture.h"
#include "buffer.h"
#include "config.h"
#include "context.h"
#include "request.h"
#include "surface.h"

#include "h264.h"
#include "h265.h"
#include "mpeg2.h"

#include <assert.h>
#include <string.h>

#include <errno.h>

#include <sys/ioctl.h>

#include <linux/videodev2.h>

#include "dmabufs.h"
#include "media.h"
#include "utils.h"
#include "v4l2.h"

#include "autoconfig.h"

static VAStatus codec_store_buffer(struct request_data *driver_data,
				   VAProfile profile,
				   struct object_surface *surface_object,
				   struct object_buffer *buffer_object)
{
	switch (buffer_object->type) {
	case VASliceDataBufferType:
		/*
		 * Since there is no guarantee that the allocation
		 * order is the same as the submission order (via
		 * RenderPicture), we can't use a V4L2 buffer directly
		 * and have to copy from a regular buffer.
		 */
		dmabuf_write_start(surface_object->source_dh);
		memcpy(surface_object->source_data +
			       surface_object->slices_size,
		       buffer_object->data,
		       buffer_object->size * buffer_object->count);
		dmabuf_write_end(surface_object->source_dh);
		surface_object->slices_size +=
			buffer_object->size * buffer_object->count;
		surface_object->slices_count++;
		break;

	case VAPictureParameterBufferType:
		switch (profile) {
		case VAProfileMPEG2Simple:
		case VAProfileMPEG2Main:
			memcpy(&surface_object->params.mpeg2.picture,
			       buffer_object->data,
			       sizeof(surface_object->params.mpeg2.picture));
			break;

		case VAProfileH264Main:
		case VAProfileH264High:
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264MultiviewHigh:
		case VAProfileH264StereoHigh:
			memcpy(&surface_object->params.h264.picture,
			       buffer_object->data,
			       sizeof(surface_object->params.h264.picture));
			break;

		case VAProfileHEVCMain:
		case VAProfileHEVCMain10:
			memcpy(&surface_object->params.h265.picture,
			       buffer_object->data,
			       sizeof(surface_object->params.h265.picture));
			break;

		default:
			break;
		}
		break;

	case VASliceParameterBufferType:
		switch (profile) {
		case VAProfileH264Main:
		case VAProfileH264High:
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264MultiviewHigh:
		case VAProfileH264StereoHigh:
			memcpy(&surface_object->params.h264.slice,
			       buffer_object->data,
			       sizeof(surface_object->params.h264.slice));
			break;

		case VAProfileHEVCMain:
		case VAProfileHEVCMain10:
			memcpy(&surface_object->params.h265.slice,
			       buffer_object->data,
			       sizeof(surface_object->params.h265.slice));
			break;

		default:
			break;
		}
		break;

	case VAIQMatrixBufferType:
		switch (profile) {
		case VAProfileMPEG2Simple:
		case VAProfileMPEG2Main:
			memcpy(&surface_object->params.mpeg2.iqmatrix,
			       buffer_object->data,
			       sizeof(surface_object->params.mpeg2.iqmatrix));
			surface_object->params.mpeg2.iqmatrix_set = true;
			break;

		case VAProfileH264Main:
		case VAProfileH264High:
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264MultiviewHigh:
		case VAProfileH264StereoHigh:
			memcpy(&surface_object->params.h264.matrix,
			       buffer_object->data,
			       sizeof(surface_object->params.h264.matrix));
			break;

		case VAProfileHEVCMain:
		case VAProfileHEVCMain10:
			memcpy(&surface_object->params.h265.iqmatrix,
			       buffer_object->data,
			       sizeof(surface_object->params.h265.iqmatrix));
			surface_object->params.h265.iqmatrix_set = true;
			break;

		default:
			break;
		}
		break;

	default:
		break;
	}

	return VA_STATUS_SUCCESS;
}

static VAStatus codec_set_controls(struct request_data *driver_data,
				   struct object_context *context,
				   VAProfile profile,
				   struct media_request * const mreq,
				   struct object_surface *surface_object)
{
	int rc;

	switch (profile) {
	case VAProfileMPEG2Simple:
	case VAProfileMPEG2Main:
		rc = mpeg2_set_controls(driver_data, context, mreq, surface_object);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		break;

	case VAProfileH264Main:
	case VAProfileH264High:
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264MultiviewHigh:
	case VAProfileH264StereoHigh:
		rc = h264_set_controls(driver_data, context, mreq, surface_object);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		break;

	case VAProfileHEVCMain:
	case VAProfileHEVCMain10:
		rc = h265_set_controls(driver_data, context, mreq, surface_object);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		break;

	default:
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	}

	return VA_STATUS_SUCCESS;
}

static VAStatus flush_data(struct request_data *driver_data,
			   struct object_context *context_object,
			   struct object_config *config_object,
			   struct object_surface *surface_object,
			   bool is_last)
{
	struct video_format *video_format;
	unsigned int output_type, capture_type;
	int rc;
	struct media_request * mreq;

	surface_object->needs_flush = false;

	video_format = driver_data->video_format;
	if (video_format == NULL)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	output_type = v4l2_type_video_output(video_format->v4l2_mplane);
	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

	mreq = media_request_get(driver_data->media_pool);
	if (!mreq) {
		request_log("media_request_get failed\n");
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}

	rc = codec_set_controls(driver_data, context_object,
				config_object->profile, mreq, surface_object);
	if (rc != VA_STATUS_SUCCESS)
		return rc;

	rc = v4l2_queue_dmabuf(driver_data->video_fd, mreq,
			       surface_object->source_dh,
			       output_type,
			       &surface_object->timestamp,
			       surface_object->source_index,
			       surface_object->slices_size, 1, !is_last);
	surface_object->slices_size = 0;
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	if (surface_object->req_one) {
		surface_object->req_one = false;
#if 0
		rc = v4l2_queue_buffer(driver_data->video_fd, NULL, capture_type, NULL,
				       surface_object->destination_index, 0,
				       surface_object->destination_buffers_count, false);
#else
		rc = v4l2_queue_dmabuf(driver_data->video_fd, NULL,
				       surface_object->destination_dh[0],
				       capture_type, NULL,
				       surface_object->destination_index, 0,
				       surface_object->destination_buffers_count, false);
#endif
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	if (media_request_start(mreq)) {
		request_log("media_request_start failed\n");
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	return queue_await_completion(driver_data, surface_object, is_last);
}

VAStatus RequestBeginPicture(VADriverContextP context, VAContextID context_id,
			     VASurfaceID surface_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_context *context_object;
	struct object_surface *surface_object;

	context_object = CONTEXT(driver_data, context_id);
	if (context_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	if (surface_object->status == VASurfaceRendering)
		RequestSyncSurface(context, surface_id);

	surface_object->status = VASurfaceRendering;
	context_object->render_surface_id = surface_id;

	gettimeofday(&surface_object->timestamp, NULL);

	surface_object->req_one = true;
	surface_object->needs_flush = false;

	return VA_STATUS_SUCCESS;
}


VAStatus RequestRenderPicture(VADriverContextP context, VAContextID context_id,
			      VABufferID *buffers_ids, int buffers_count)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_context *context_object;
	struct object_config *config_object;
	struct object_surface *surface_object;
	struct object_buffer *buffer_object;
	VAStatus rv;
	int rc;
	int i;

	context_object = CONTEXT(driver_data, context_id);
	if (context_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	config_object = CONFIG(driver_data, context_object->config_id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	surface_object =
		SURFACE(driver_data, context_object->render_surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	for (i = 0; i < buffers_count; i++) {
		if (surface_object->needs_flush) {
			rv = flush_data(driver_data, context_object, config_object, surface_object, false);
			if (rv != VA_STATUS_SUCCESS)
				return rv;
		}

		buffer_object = BUFFER(driver_data, buffers_ids[i]);
		if (buffer_object == NULL)
			return VA_STATUS_ERROR_INVALID_BUFFER;

		rc = codec_store_buffer(driver_data, config_object->profile,
					surface_object, buffer_object);
		if (rc != VA_STATUS_SUCCESS)
			return rc;

		if (buffer_object->type == VASliceDataBufferType)
			surface_object->needs_flush = true;
	}

	return VA_STATUS_SUCCESS;
}

VAStatus RequestEndPicture(VADriverContextP context, VAContextID context_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_context *context_object;
	struct object_config *config_object;
	struct object_surface *surface_object;
	struct video_format *video_format;
	VAStatus status;

	video_format = driver_data->video_format;
	if (video_format == NULL)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	context_object = CONTEXT(driver_data, context_id);
	if (context_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	config_object = CONFIG(driver_data, context_object->config_id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	surface_object =
		SURFACE(driver_data, context_object->render_surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	status = flush_data(driver_data, context_object, config_object, surface_object, true);

	context_object->render_surface_id = VA_INVALID_ID;

	return status;
}
