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

#include "context.h"
#include "config.h"
#include "devscan.h"
#include "request.h"
#include "surface.h"

#include <stdlib.h>
#include <string.h>

#include <assert.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>

#include <mpeg2-ctrls.h>
#include <h264-ctrls.h>
#include <hevc-ctrls.h>

#include "dmabufs.h"
#include "utils.h"
#include "v4l2.h"

#include "autoconfig.h"

static uint32_t vaprofile_to_pixfmt(const VAProfile profile)
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

VAStatus RequestCreateContext(VADriverContextP dc, VAConfigID config_id,
			      int picture_width, int picture_height, int flags,
			      VASurfaceID *surfaces_ids, int surfaces_count,
			      VAContextID *context_id)
{
	struct request_data *driver_data = dc->pDriverData;
	struct object_config *config_object;
	struct object_context *context_object = NULL;
	VAContextID id;
	VAStatus status;
	unsigned int pixelformat;
	const struct decdev *ddev;

	config_object = CONFIG(driver_data, config_id);
	if (config_object == NULL) {
		status = VA_STATUS_ERROR_INVALID_CONFIG;
		goto error;
	}

	id = object_heap_allocate(&driver_data->context_heap);
	context_object = CONTEXT(driver_data, id);
	if (context_object == NULL) {
		status = VA_STATUS_ERROR_ALLOCATION_FAILED;
		goto error;
	}
	memset(&context_object->dpb, 0, sizeof(context_object->dpb));

	pixelformat = vaprofile_to_pixfmt(config_object->profile);
	if (!pixelformat) {
		request_log("%s: Unknown vaprofle: %#x\n", __func__, config_object->profile);
		goto error;
	}

	ddev = devscan_find(driver_data->scan, pixelformat);
	if (!ddev) {
		request_err(dc, "No driver found for pixelformat %#x\n", pixelformat);
		goto error;
	}

	context_object->mbc = mediabufs_ctl_new(dc, decdev_video_path(ddev),
						driver_data->pollqueue);
	if (!context_object->mbc) {
		request_log("%s: Failed to create mediabufs_ctl\n", __func__);
		goto error;
	}

	status = mediabufs_src_fmt_set(context_object->mbc,
				       pixelformat,
				       picture_width, picture_height);
	if (status != VA_STATUS_SUCCESS)
		goto error;

	context_object->config_id = config_id;
	context_object->render_surface_id = VA_INVALID_ID;
	context_object->surfaces_ids = malloc(sizeof(*context_object->surfaces_ids) * surfaces_count);
	context_object->surfaces_count = surfaces_count;
	context_object->picture_width = picture_width;
	context_object->picture_height = picture_height;
	context_object->flags = flags;

	memcpy(context_object->surfaces_ids, surfaces_ids,
	       sizeof(*context_object->surfaces_ids) * surfaces_count);

	*context_id = id;

	request_log("%s: id=%#x\n", __func__, id);

	status = VA_STATUS_SUCCESS;
	goto complete;

error:
	if (context_object != NULL)
		object_heap_free(&driver_data->context_heap,
				 (struct object_base *)context_object);

complete:
	return status;
}

VAStatus RequestDestroyContext(VADriverContextP vdc, VAContextID context_id)
{
	struct request_data *driver_data = vdc->pDriverData;
	struct object_context *ctx;

	request_log("%s: id=%#x\n", __func__, context_id);

	ctx = CONTEXT(driver_data, context_id);
	if (ctx == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

//	for (i = 0; i != ctx->surfaces_count; ++i)
//		RequestSyncSurface(vdc, ctx->surfaces_ids[i]);

	free(ctx->surfaces_ids);

	mediabufs_ctl_unref(&ctx->mbc);

	object_heap_free(&driver_data->context_heap, &ctx->base);

	return VA_STATUS_SUCCESS;
}
