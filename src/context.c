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

#include <stdlib.h>
#include <string.h>

#include "autoconfig.h"
#include "context.h"
#include "config.h"
#include "dmabufs.h"
#include "devscan.h"
#include "media.h"
#include "request.h"
#include "surface.h"
#include "utils.h"
#include "video.h"

VAStatus RequestCreateContext(VADriverContextP dc, VAConfigID config_id,
			      int picture_width, int picture_height, int flags,
			      VASurfaceID *surfaces_ids, int surfaces_count,
			      VAContextID *context_id)
{
	struct request_data *const dd = dc->pDriverData;
	struct object_config *const cfg = CONFIG(dd, config_id);
	struct object_context *ctx = NULL;
	VAContextID id;
	VAStatus status;
	uint32_t pixelformat;
	const struct decdev *ddev;

	if (cfg == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	id = object_heap_allocate(&dd->context_heap);
	ctx = CONTEXT(dd, id);
	if (ctx == NULL)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	pixelformat = video_profile_to_src_pixfmt(cfg->profile);
	if (!pixelformat) {
		request_info(dc, "%s: Unknown vaprofle: %#x\n", __func__, cfg->profile);
		goto fail;
	}

	ddev = devscan_find(dd->scan, pixelformat);
	if (!ddev) {
		request_err(dc, "No driver found for pixelformat %#x\n", pixelformat);
		goto fail;
	}

	ctx->mbc = mediabufs_ctl_new(dc, decdev_video_path(ddev),
						dd->pollqueue);
	if (!ctx->mbc) {
		request_err(dc, "%s: Failed to create mediabufs_ctl\n", __func__);
		goto fail;
	}

	status = mediabufs_src_fmt_set(ctx->mbc,
				       pixelformat,
				       picture_width, picture_height);
	if (status != VA_STATUS_SUCCESS)
		goto fail2;

	ctx->surfaces_ids = malloc(sizeof(*ctx->surfaces_ids) * surfaces_count);
	if (!ctx->surfaces_ids) {
		status = VA_STATUS_ERROR_ALLOCATION_FAILED;
		goto fail2;
	}
	ctx->surfaces_count = surfaces_count;
	memcpy(ctx->surfaces_ids, surfaces_ids,
	       sizeof(*ctx->surfaces_ids) * surfaces_count);

	ctx->config_id = config_id;
	ctx->render_surface_id = VA_INVALID_ID;
	ctx->picture_width = picture_width;
	ctx->picture_height = picture_height;
	ctx->flags = flags;

	*context_id = id;
	return VA_STATUS_SUCCESS;

fail2:
	mediabufs_ctl_unref(&ctx->mbc);
fail:
	object_heap_free(&dd->context_heap, &ctx->base);
	return status;
}

VAStatus RequestDestroyContext(VADriverContextP vdc, VAContextID context_id)
{
	struct request_data *driver_data = vdc->pDriverData;
	struct object_context *ctx;

	ctx = CONTEXT(driver_data, context_id);
	if (ctx == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	free(ctx->surfaces_ids);

	mediabufs_ctl_unref(&ctx->mbc);

	object_heap_free(&driver_data->context_heap, &ctx->base);

	return VA_STATUS_SUCCESS;
}
