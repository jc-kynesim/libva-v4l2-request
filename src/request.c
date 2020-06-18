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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <va/va_backend.h>

#include "autoconfig.h"
#include "buffer.h"
#include "dmabufs.h"
#include "config.h"
#include "context.h"
#include "devscan.h"
#include "image.h"
#include "media.h"
#include "picture.h"
#include "pollqueue.h"
#include "request.h"
#include "subpicture.h"
#include "surface.h"
#include "utils.h"
#include "v4l2.h"

/* Set default visibility for the init function only. */
VAStatus __attribute__((visibility("default")))
VA_DRIVER_INIT_FUNC(VADriverContextP context);

VAStatus VA_DRIVER_INIT_FUNC(VADriverContextP dc)
{
	struct request_data *dd;
	struct VADriverVTable *const vtable = dc->vtable;
	VAStatus status;
	const struct decdev *decdev;

	dc->version_major = VA_MAJOR_VERSION;
	dc->version_minor = VA_MINOR_VERSION;
	dc->max_profiles = V4L2_REQUEST_MAX_PROFILES;
	dc->max_entrypoints = V4L2_REQUEST_MAX_ENTRYPOINTS;
	dc->max_attributes = V4L2_REQUEST_MAX_CONFIG_ATTRIBUTES;
	dc->max_image_formats = V4L2_REQUEST_MAX_IMAGE_FORMATS;
	dc->max_subpic_formats = V4L2_REQUEST_MAX_SUBPIC_FORMATS;
	dc->max_display_attributes = V4L2_REQUEST_MAX_DISPLAY_ATTRIBUTES;
	dc->str_vendor = V4L2_REQUEST_STR_VENDOR;

	vtable->vaTerminate = RequestTerminate;
	vtable->vaQueryConfigEntrypoints = RequestQueryConfigEntrypoints;
	vtable->vaQueryConfigProfiles = RequestQueryConfigProfiles;
	vtable->vaQueryConfigEntrypoints = RequestQueryConfigEntrypoints;
	vtable->vaQueryConfigAttributes = RequestQueryConfigAttributes;
	vtable->vaCreateConfig = RequestCreateConfig;
	vtable->vaDestroyConfig = RequestDestroyConfig;
	vtable->vaGetConfigAttributes = RequestGetConfigAttributes;
	vtable->vaCreateSurfaces = RequestCreateSurfaces;
	vtable->vaCreateSurfaces2 = RequestCreateSurfaces2;
	vtable->vaDestroySurfaces = RequestDestroySurfaces;
	vtable->vaExportSurfaceHandle = RequestExportSurfaceHandle;
	vtable->vaCreateContext = RequestCreateContext;
	vtable->vaDestroyContext = RequestDestroyContext;
	vtable->vaCreateBuffer = RequestCreateBuffer;
	vtable->vaBufferSetNumElements = RequestBufferSetNumElements;
	vtable->vaMapBuffer = RequestMapBuffer;
	vtable->vaUnmapBuffer = RequestUnmapBuffer;
	vtable->vaDestroyBuffer = RequestDestroyBuffer;
	vtable->vaBufferInfo = RequestBufferInfo;
	vtable->vaAcquireBufferHandle = RequestAcquireBufferHandle;
	vtable->vaReleaseBufferHandle = RequestReleaseBufferHandle;
	vtable->vaBeginPicture = RequestBeginPicture;
	vtable->vaRenderPicture = RequestRenderPicture;
	vtable->vaEndPicture = RequestEndPicture;
	vtable->vaSyncSurface = RequestSyncSurface;
	vtable->vaQuerySurfaceAttributes = RequestQuerySurfaceAttributes;
	vtable->vaQuerySurfaceStatus = RequestQuerySurfaceStatus;
	vtable->vaPutSurface = RequestPutSurface;
	vtable->vaQueryImageFormats = RequestQueryImageFormats;
	vtable->vaCreateImage = RequestCreateImage;
	vtable->vaDeriveImage = RequestDeriveImage;
	vtable->vaDestroyImage = RequestDestroyImage;
	vtable->vaSetImagePalette = RequestSetImagePalette;
	vtable->vaGetImage = RequestGetImage;
	vtable->vaPutImage = RequestPutImage;
	vtable->vaQuerySubpictureFormats = RequestQuerySubpictureFormats;
	vtable->vaCreateSubpicture = RequestCreateSubpicture;
	vtable->vaDestroySubpicture = RequestDestroySubpicture;
	vtable->vaSetSubpictureImage = RequestSetSubpictureImage;
	vtable->vaSetSubpictureChromakey = RequestSetSubpictureChromakey;
	vtable->vaSetSubpictureGlobalAlpha = RequestSetSubpictureGlobalAlpha;
	vtable->vaAssociateSubpicture = RequestAssociateSubpicture;
	vtable->vaDeassociateSubpicture = RequestDeassociateSubpicture;
	vtable->vaQueryDisplayAttributes = RequestQueryDisplayAttributes;
	vtable->vaGetDisplayAttributes = RequestGetDisplayAttributes;
	vtable->vaSetDisplayAttributes = RequestSetDisplayAttributes;
	vtable->vaLockSurface = RequestLockSurface;
	vtable->vaUnlockSurface = RequestUnlockSurface;

	dd = calloc(1, sizeof(*dd));
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	dc->pDriverData = dd;
	dd->dc = dc;

	object_heap_init(&dd->config_heap,
			 sizeof(struct object_config), CONFIG_ID_OFFSET);
	object_heap_init(&dd->context_heap,
			 sizeof(struct object_context), CONTEXT_ID_OFFSET);
	object_heap_init(&dd->surface_heap,
			 sizeof(struct object_surface), SURFACE_ID_OFFSET);
	object_heap_init(&dd->buffer_heap,
			 sizeof(struct object_buffer), BUFFER_ID_OFFSET);
	object_heap_init(&dd->image_heap, sizeof(struct object_image),
			 IMAGE_ID_OFFSET);

	status = devscan_build(dc, &dd->scan);
	if (status != VA_STATUS_SUCCESS)
		goto error;

	decdev = devscan_find(dd->scan, 0);
	if (!decdev) {
		request_err(dc, "Failed to find any usable V4L2 request devices");
		goto error;
	}

	dd->dmabufs_ctrl = dmabufs_ctrl_new();
	if (!dd->dmabufs_ctrl) {
		request_err(dc, "Failed to get dmabufs\n");
		goto error;
	}

	dd->pollqueue = pollqueue_new();
	if (!dd->pollqueue) {
		request_err(dc, "Failed to create pollqueue\n");
		goto error;
	}

	dd->media_pool = media_pool_new(decdev_media_path(decdev),
					dd->pollqueue, 4);
	if (!dd->media_pool) {
		request_err(dc, "Failed to create media pool for '%s'\n",
			    decdev_media_path(decdev));
		goto error;
	}

	return VA_STATUS_SUCCESS;

error:
	RequestTerminate(dc);
	return VA_STATUS_ERROR_OPERATION_FAILED;
}

VAStatus RequestTerminate(VADriverContextP dc)
{
	struct request_data *const dd = dc->pDriverData;
	struct object_buffer *buffer_object;
	struct object_image *image_object;
	struct object_surface *surface_object;
	struct object_context *context_object;
	struct object_config *config_object;
	int iterator;

	/* Cleanup leftover buffers. */

	image_object = (struct object_image *)
		object_heap_first(&dd->image_heap, &iterator);
	while (image_object != NULL) {
		RequestDestroyImage(dc, (VAImageID)image_object->base.id);
		image_object = (struct object_image *)
			object_heap_next(&dd->image_heap, &iterator);
	}

	object_heap_destroy(&dd->image_heap);

	buffer_object = (struct object_buffer *)
		object_heap_first(&dd->buffer_heap, &iterator);
	while (buffer_object != NULL) {
		RequestDestroyBuffer(dc,
				     (VABufferID)buffer_object->base.id);
		buffer_object = (struct object_buffer *)
			object_heap_next(&dd->buffer_heap, &iterator);
	}

	object_heap_destroy(&dd->buffer_heap);

	surface_object = (struct object_surface *)
		object_heap_first(&dd->surface_heap, &iterator);
	while (surface_object != NULL) {
		RequestDestroySurfaces(dc,
				      (VASurfaceID *)&surface_object->base.id, 1);
		surface_object = (struct object_surface *)
			object_heap_next(&dd->surface_heap, &iterator);
	}

	object_heap_destroy(&dd->surface_heap);

	context_object = (struct object_context *)
		object_heap_first(&dd->context_heap, &iterator);
	while (context_object != NULL) {
		RequestDestroyContext(dc,
				      (VAContextID)context_object->base.id);
		context_object = (struct object_context *)
			object_heap_next(&dd->context_heap, &iterator);
	}

	object_heap_destroy(&dd->context_heap);

	config_object = (struct object_config *)
		object_heap_first(&dd->config_heap, &iterator);
	while (config_object != NULL) {
		RequestDestroyConfig(dc,
				     (VAConfigID)config_object->base.id);
		config_object = (struct object_config *)
			object_heap_next(&dd->config_heap, &iterator);
	}

	object_heap_destroy(&dd->config_heap);

	media_pool_delete(dd->media_pool);
	pollqueue_delete(&dd->pollqueue);
	dmabufs_ctrl_delete(dd->dmabufs_ctrl);
	devscan_delete(dd->scan);

	free(dc->pDriverData);
	dc->pDriverData = NULL;

	return VA_STATUS_SUCCESS;
}




//------------------------------------------------------------------------------





