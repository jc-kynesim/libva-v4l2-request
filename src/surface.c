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

#include "request.h"
#include "surface.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <va/va_drmcommon.h>
#include <drm_fourcc.h>
#include <linux/videodev2.h>

#include "media.h"
#include "config.h"
#include "utils.h"
#include "v4l2.h"
#include "video.h"
#include "dmabufs.h"
#include "picture.h"

VAStatus RequestCreateSurfaces2(VADriverContextP context, unsigned int format,
				unsigned int width, unsigned int height,
				VASurfaceID *surfaces_ids,
				unsigned int surfaces_count,
				VASurfaceAttrib *attributes,
				unsigned int attributes_count)
{
	unsigned int i;
	struct request_data *const rd = context->pDriverData;
	const unsigned int seq = atomic_fetch_add(&rd->surface_alloc_seq, 1);

	for (i = 0; i < surfaces_count; i++) {
		struct object_surface *os;
		int id = object_heap_allocate(&rd->surface_heap);

		if (id == -1)
			goto fail;

		os = SURFACE(rd, id);
		/* Zap all our data whilst avoiding the object control stuff */
		memset((char*)os + sizeof(os->base), 0,
		       sizeof(*os) - sizeof(os->base));

		os->alloc_state = SURFACE_NEW;
		os->rd = rd;
		os->context_id = VA_INVALID_ID;
		os->seq = seq;
		os->pd.req_rtfmt  = format;
		os->pd.req_width  = width;
		os->pd.req_height = height;

		surfaces_ids[i] = id;
	}
	return VA_STATUS_SUCCESS;

fail:
	while (i--) {
		struct object_surface *os = SURFACE(rd, surfaces_ids[i]);
		object_heap_free(&rd->surface_heap, &os->base);
	}
	for (i = 0; i < surfaces_count; i++)
		surfaces_ids[i] = VA_INVALID_SURFACE;
	return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

VAStatus surface_attach(struct object_surface *const os,
			struct mediabufs_ctl *const mbc,
			struct dmabufs_ctrl *const dbsc,
			const VAContextID id)
{
	if (os->context_id == id)
		return VA_STATUS_SUCCESS;

	os->qent = mediabufs_dst_qent_alloc(mbc, dbsc);
	if (!os->qent) {
		request_log("Failed to alloc surface dst buffers");
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}

	v4l2_format_to_picdesc(&os->pd, mediabufs_dst_fmt(mbc));

	os->context_id = id;
	return VA_STATUS_SUCCESS;
}

VAStatus RequestCreateSurfaces(VADriverContextP context, int width, int height,
			       int format, int surfaces_count,
			       VASurfaceID *surfaces_ids)
{
	return RequestCreateSurfaces2(context, format, width, height,
				      surfaces_ids, surfaces_count, NULL, 0);
}

VAStatus RequestDestroySurfaces(VADriverContextP context,
				VASurfaceID *surfaces_ids, int surfaces_count)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surf;
	unsigned int i;

	for (i = 0; i < surfaces_count; i++) {
		surf = SURFACE(driver_data, surfaces_ids[i]);
		if (surf == NULL)
			return VA_STATUS_ERROR_INVALID_SURFACE;

		qent_dst_delete(surf->qent);
		bit_stash_delete(surf->bit_stash);

		object_heap_free(&driver_data->surface_heap,
				 (struct object_base *)surf);
	}

	return VA_STATUS_SUCCESS;
}

VAStatus queue_await_completion(struct request_data *driver_data, struct object_surface *surface_object, bool last)
{
	VAStatus status;

	if (last) {
		status = qent_dst_wait(surface_object->qent);
		if (status != VA_STATUS_SUCCESS) {
			request_log("qent_dst_wait failed\n");
			goto error;
		}

		surface_object->status = VASurfaceDisplaying;
	}

	status = VA_STATUS_SUCCESS;
error:
	return status;
}

VAStatus RequestSyncSurface(VADriverContextP context, VASurfaceID surface_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	if (surface_object->status != VASurfaceRendering)
		return VA_STATUS_SUCCESS;

	return queue_await_completion(driver_data, surface_object, true);
}

static int add_pixel_format_attributes(VASurfaceAttrib *const attributes_list,
				       const VAProfile profile)
{
	switch (profile) {
	default:
		attributes_list[0].type = VASurfaceAttribPixelFormat;
		attributes_list[0].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
		attributes_list[0].value.type = VAGenericValueTypeInteger;
		attributes_list[0].value.value.i = VA_FOURCC_NV12;
		return 1;
	case VAProfileHEVCMain10:
		attributes_list[0].type = VASurfaceAttribPixelFormat;
		attributes_list[0].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
		attributes_list[0].value.type = VAGenericValueTypeInteger;
		attributes_list[0].value.value.i = VA_FOURCC_P010;
		return 1;
	}
	return 0;
}


VAStatus RequestQuerySurfaceAttributes(VADriverContextP context,
				       VAConfigID config,
				       VASurfaceAttrib *attributes,
				       unsigned int *attributes_count)
{
	struct request_data *driver_data = context->pDriverData;
	VASurfaceAttrib *attributes_list;
	unsigned int attributes_list_size = V4L2_REQUEST_MAX_CONFIG_ATTRIBUTES *
					    sizeof(*attributes);
	int memory_types;
	unsigned int i = 0;
	struct object_config *config_object;

	config_object = CONFIG(driver_data, config);
	if (config_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	attributes_list = malloc(attributes_list_size);
	memset(attributes_list, 0, attributes_list_size);

	i += add_pixel_format_attributes(attributes_list + i,
					 config_object->profile);

	attributes_list[i].type = VASurfaceAttribMinWidth;
	attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
	attributes_list[i].value.type = VAGenericValueTypeInteger;
	attributes_list[i].value.value.i = 32;
	i++;

	attributes_list[i].type = VASurfaceAttribMaxWidth;
	attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
	attributes_list[i].value.type = VAGenericValueTypeInteger;
	attributes_list[i].value.value.i = 4096;
	i++;

	attributes_list[i].type = VASurfaceAttribMinHeight;
	attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
	attributes_list[i].value.type = VAGenericValueTypeInteger;
	attributes_list[i].value.value.i = 32;
	i++;

	attributes_list[i].type = VASurfaceAttribMaxHeight;
	attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
	attributes_list[i].value.type = VAGenericValueTypeInteger;
	attributes_list[i].value.value.i = 4096;
	i++;

	attributes_list[i].type = VASurfaceAttribMemoryType;
	attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE |
				   VA_SURFACE_ATTRIB_SETTABLE;
	attributes_list[i].value.type = VAGenericValueTypeInteger;

	memory_types = VA_SURFACE_ATTRIB_MEM_TYPE_VA |
		VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;

	/*
	 * First version of DRM prime export does not handle modifiers,
	 * that are required for supporting the tiled output format.
	 *
	 * At this point we haven't nailed down our internal format
	 */
	//	memory_types |= VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;

	attributes_list[i].value.value.i = memory_types;
	i++;

	attributes_list_size = i * sizeof(*attributes);

	if (attributes != NULL)
		memcpy(attributes, attributes_list, attributes_list_size);

	free(attributes_list);

	*attributes_count = i;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestQuerySurfaceStatus(VADriverContextP context,
				   VASurfaceID surface_id,
				   VASurfaceStatus *status)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	*status = surface_object->status;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestPutSurface(VADriverContextP context, VASurfaceID surface_id,
			   void *draw, short src_x, short src_y,
			   unsigned short src_width, unsigned short src_height,
			   short dst_x, short dst_y, unsigned short dst_width,
			   unsigned short dst_height, VARectangle *cliprects,
			   unsigned int cliprects_count, unsigned int flags)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus RequestLockSurface(VADriverContextP context, VASurfaceID surface_id,
			    unsigned int *fourcc, unsigned int *luma_stride,
			    unsigned int *chroma_u_stride,
			    unsigned int *chroma_v_stride,
			    unsigned int *luma_offset,
			    unsigned int *chroma_u_offset,
			    unsigned int *chroma_v_offset,
			    unsigned int *buffer_name, void **buffer)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus RequestUnlockSurface(VADriverContextP context, VASurfaceID surface_id)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus RequestExportSurfaceHandle(VADriverContextP context,
				    VASurfaceID surface_id, uint32_t mem_type,
				    uint32_t flags, void *v_desc)
{
	struct request_data *driver_data = context->pDriverData;
	VADRMPRIMESurfaceDescriptor *const desc = v_desc;
	struct object_surface *surf;
	unsigned int i;

	if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
		return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;

	surf = SURFACE(driver_data, surface_id);
	if (surf == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	RequestSyncSurface(context, surface_id);

	*desc = (VADRMPRIMESurfaceDescriptor) {
		.fourcc         = surf->pd.fmt_vaapi,
		.width          = surf->pd.req_width,
		.height         = surf->pd.req_height,
		.num_objects    = surf->pd.buffer_count,
		.num_layers     = 1,
		.layers = {{
			.drm_format = surf->pd.fmt_drm,
			.num_planes = surf->pd.plane_count
		}}
	};

	for (i = 0; i < surf->pd.buffer_count; i++) {
		desc->objects[i].drm_format_modifier = surf->pd.bufs[i].drm_mod;
		desc->objects[i].size = surf->pd.bufs[i].size;
		desc->objects[i].fd = qent_dst_dup_fd(surf->qent, i);

		if (desc->objects[i].fd == -1) {
			while (i--)
				close(desc->objects[i].fd);
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		}
	}

	for (i = 0; i < surf->pd.plane_count; i++) {
		desc->layers[0].object_index[i] = surf->pd.planes[i].buf;
		desc->layers[0].offset[i] = surf->pd.planes[i].offset;
		desc->layers[0].pitch[i] =  surf->pd.planes[i].stride;
	}

	return VA_STATUS_SUCCESS;
}
