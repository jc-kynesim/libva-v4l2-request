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

#if 0
static const struct try_formats {
	unsigned int va_rt;
	unsigned int v4l2_fmt;
} try_formats [] = {
	{VA_RT_FORMAT_YUV420, V4L2_PIX_FMT_NV12_COL128},
	{VA_RT_FORMAT_YUV420, V4L2_PIX_FMT_SUNXI_TILED_NV12},
	{VA_RT_FORMAT_YUV420, V4L2_PIX_FMT_NV12},
	{VA_RT_FORMAT_YUV420_10, V4L2_PIX_FMT_NV12_10_COL128},
	{0, 0}
};
#endif

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

	request_log("%s: %dx%d, seq=%u\n", __func__, width, height, seq);

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
		os->pd.req_width =  width;
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

static bool check_dst_bufs(const struct object_surface *const os)
{
	unsigned int i;
	if (os->destination_buffers_count != os->pd.buffer_count)
		return false;
	for (i = 0; i != os->pd.buffer_count; ++i) {
		if (!os->destination_dh[i] ||
		    dmabuf_size(os->destination_dh[i]) < os->pd.bufs[i].size)
			return false;
	}
	return true;
}

static void free_dst_bufs(struct object_surface *const os)
{
	unsigned int i;
	for (i = 0; i != VIDEO_MAX_PLANES; ++i) {
		dmabuf_free(os->destination_dh[i]);
		os->destination_dh[i] = NULL;
	}
	os->destination_buffers_count = 0;
}

static VAStatus alloc_dst_bufs(struct object_surface *const os)
{
	unsigned int i;
	struct request_data * const rd = os->rd;

	for (i = 0; i != os->pd.buffer_count; ++i) {
		os->destination_dh[i] =
			dmabuf_alloc(rd->dmabufs_ctrl, os->pd.bufs[i].size);
		if (!os->destination_dh[i])
			goto fail;
	}

	os->destination_buffers_count = os->pd.buffer_count;
	return VA_STATUS_SUCCESS;

fail:
	free_dst_bufs(os);
	return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

VAStatus surface_attach(struct object_surface *const os,
			struct mediabufs_ctl *const mbc,
			struct dmabufs_ctrl 8const dbsc,
			const VAContextID id)
{
	VAStatus rv;

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

#if 0
VAStatus surface_alloc(VADriverContextP context, unsigned int format,
				unsigned int width, unsigned int height,
				VASurfaceID *surfaces_ids,
				unsigned int surfaces_count,
				VASurfaceAttrib *attributes,
				unsigned int attributes_count)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;
	struct video_format *video_format = NULL;
	unsigned int destination_sizes[VIDEO_MAX_PLANES];
	unsigned int destination_bytesperlines[VIDEO_MAX_PLANES];
	unsigned int destination_planes_count;
	unsigned int format_width, format_height;
	unsigned int capture_type;
	unsigned int index_base;
	unsigned int index;
	unsigned int i, j;
	struct picdesc pds;
	struct picdesc *const pd = &pds;
	VASurfaceID id;
	bool found;
	int rc;

	request_log("%s: %dx%d\n", __func__, width, height);
//	if (format != VA_RT_FORMAT_YUV420)
//		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

        if (!driver_data->video_format) {
		found = false;
		video_format = NULL;
		for (i = 0; try_formats[i].va_rt != 0; ++i) {
			if (try_formats[i].va_rt != format)
				continue;
			found = true;

			if (v4l2_find_format(driver_data->video_fd,
					     V4L2_BUF_TYPE_VIDEO_CAPTURE,
					     try_formats[i].v4l2_fmt))
			{
				video_format = video_format_find(try_formats[i].v4l2_fmt);
				break;
			}
		}

		if (!found)
			return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

		if (video_format == NULL)
			return VA_STATUS_ERROR_OPERATION_FAILED;

		driver_data->video_format = video_format;

		capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

		rc = v4l2_set_format(driver_data->video_fd, capture_type,
				     video_format->v4l2_format, width, height);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
        } else {
		video_format = driver_data->video_format;
		capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);
	}

	rc = v4l2_try_picdesc(pd, driver_data->video_fd, capture_type, width, height, video_format->v4l2_format);
	request_log("PD: %dx%d -> rc=%d %dx%d*%d size=%zd\n", pd->req_width, pd->req_height,
		    rc, pd->planes[0].width, pd->planes[0].height, pd->plane_count, pd->bufs[0].size);

	rc = v4l2_get_format(driver_data->video_fd, capture_type, &format_width,
			     &format_height, destination_bytesperlines,
			     destination_sizes, NULL);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	destination_planes_count = video_format->planes_count;

	rc = v4l2_create_buffers(driver_data->video_fd, capture_type,
				 V4L2_MEMORY_DMABUF,
				 surfaces_count, &index_base);
	if (rc < 0)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	for (i = 0; i < surfaces_count; i++) {
		index = index_base + i;

		id = object_heap_allocate(&driver_data->surface_heap);
		surface_object = SURFACE(driver_data, id);
		if (surface_object == NULL)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;

		rc = v4l2_query_buffer(driver_data->video_fd, capture_type,
				       index,
				       surface_object->destination_map_lengths,
				       surface_object->destination_map_offsets,
				       video_format->v4l2_buffers_count);
		if (rc < 0)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;

		for (j = 0; j < video_format->v4l2_buffers_count; j++) {
			surface_object->destination_dh[j] = dmabuf_alloc(driver_data->dmabufs_ctrl, pd->bufs[j].size);
			if (!surface_object->destination_dh[j]) {
				request_log("Failed dest surface alloc\n");
				return VA_STATUS_ERROR_ALLOCATION_FAILED;
			}

			surface_object->destination_map[j] =
				dmabuf_map(surface_object->destination_dh[j]);
			if (!surface_object->destination_map[j])
				return VA_STATUS_ERROR_ALLOCATION_FAILED;
		}

		/*
		 * FIXME: Handle this per-pixelformat, trying to generalize it
		 * is not a reasonable approach. The final description should be
		 * in terms of (logical) planes.
		 */

		if (video_format->v4l2_buffers_count == 1) {
			switch (video_format->v4l2_format) {
			case V4L2_PIX_FMT_NV12_COL128:
				/* modifiers are per buffer so only 1 needed */
				surface_object->destination_modifiers[0] = DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT(destination_bytesperlines[0]);
				/*
				 * What do we actually mean by plane size here?
				 * A plausible argument could be made for {<total_size>, 0}
				 * But it is probaly best to fake up something with a
				 * geometry that looks like a 'normal' NV12 layout
				 */
				surface_object->destination_sizes[0] = format_width * format_height;
				surface_object->destination_sizes[1] = destination_sizes[0] - surface_object->destination_sizes[0];
				surface_object->destination_offsets[0] = 0;
				surface_object->destination_offsets[1] = format_height * 128;
				surface_object->destination_data[0] = (unsigned char *)surface_object->destination_map[0];
				surface_object->destination_data[1] = surface_object->destination_data[0] + surface_object->destination_offsets[1];
				/* To work around assertions in some code make bytesperline >= width */
				surface_object->destination_bytesperlines[0] = format_width;
				surface_object->destination_bytesperlines[1] = format_width;
				break;
			case V4L2_PIX_FMT_NV12_10_COL128:
				surface_object->destination_modifiers[0] = DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT(destination_bytesperlines[0]);
				surface_object->destination_sizes[0] = format_width * format_height * 4 / 3;
				surface_object->destination_sizes[1] = destination_sizes[0] - surface_object->destination_sizes[0];
				surface_object->destination_offsets[0] = 0;
				surface_object->destination_offsets[1] = format_height * 128;
				surface_object->destination_data[0] = (unsigned char *)surface_object->destination_map[0];
				surface_object->destination_data[1] = surface_object->destination_data[0] + surface_object->destination_offsets[1];
				surface_object->destination_bytesperlines[0] = format_width * 4 / 3;
				surface_object->destination_bytesperlines[1] = format_width * 4 / 3;
				break;
			default:
				surface_object->destination_modifiers[0] =
					video_format->drm_modifier;

				destination_sizes[0] = destination_bytesperlines[0] *
						       format_height;

				for (j = 1; j < destination_planes_count; j++)
					destination_sizes[j] = destination_sizes[0] / 2;

				for (j = 0; j < destination_planes_count; j++) {
					surface_object->destination_offsets[j] =
						j > 0 ? destination_sizes[j - 1] : 0;
					surface_object->destination_data[j] =
						((unsigned char *)surface_object->destination_map[0] +
						 surface_object->destination_offsets[j]);
					surface_object->destination_sizes[j] =
						destination_sizes[j];
					surface_object->destination_bytesperlines[j] =
						destination_bytesperlines[0];
				}
				break;
			}
		} else if (video_format->v4l2_buffers_count == destination_planes_count) {
			for (j = 0; j < destination_planes_count; j++) {
				surface_object->destination_modifiers[j] =
					video_format->drm_modifier;
				surface_object->destination_offsets[j] = 0;
				surface_object->destination_data[j] =
					surface_object->destination_map[j];
				surface_object->destination_sizes[j] =
					destination_sizes[j];
				surface_object->destination_bytesperlines[j] =
					destination_bytesperlines[j];
			}
		} else {
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		}

		surface_object->status = VASurfaceReady;

		surface_object->source_index = 0;
		surface_object->source_data = NULL;
		surface_object->source_size = 0;

		surface_object->destination_index = index;

		surface_object->destination_planes_count =
			destination_planes_count;
		surface_object->destination_buffers_count =
			video_format->v4l2_buffers_count;

		memset(&surface_object->params, 0,
		       sizeof(surface_object->params));
		surface_object->slices_count = 0;
		surface_object->slices_size = 0;

		surfaces_ids[i] = id;
	}

	return VA_STATUS_SUCCESS;
}
#endif

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
	struct object_surface *surface_object;
	unsigned int i, j;

	for (i = 0; i < surfaces_count; i++) {
		request_log("%s[%d/%d]: id=%#x\n", __func__, i, surfaces_count, surfaces_ids[i]);

		surface_object = SURFACE(driver_data, surfaces_ids[i]);
		if (surface_object == NULL)
			return VA_STATUS_ERROR_INVALID_SURFACE;

		dmabuf_free(surface_object->source_dh);

		for (j = 0; j < surface_object->destination_buffers_count; j++)
			dmabuf_free(surface_object->destination_dh[j]);

		object_heap_free(&driver_data->surface_heap,
				 (struct object_base *)surface_object);
	}

	return VA_STATUS_SUCCESS;
}

VAStatus queue_await_completion(struct request_data *driver_data, struct object_surface *surface_object, bool last)
{
	VAStatus status;
	struct video_format *video_format = driver_data->video_format;
	unsigned int output_type, capture_type;
	int rc;

	output_type = v4l2_type_video_output(video_format->v4l2_mplane);
	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

	rc = v4l2_dequeue_buffer(driver_data->video_fd, -1, output_type,
				 surface_object->source_index, 1);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	if (last) {
		rc = v4l2_dequeue_buffer(driver_data->video_fd, -1, capture_type,
					 surface_object->destination_index,
					 surface_object->destination_buffers_count);
		if (rc < 0) {
			status = VA_STATUS_ERROR_OPERATION_FAILED;
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
	struct video_format *video_format;
	struct object_surface *surface_object;

	video_format = driver_data->video_format;
	if (video_format == NULL)
		return VA_STATUS_ERROR_OPERATION_FAILED;

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
	 */

	if (video_format_is_linear(driver_data->video_format))
		memory_types |= VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;

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
				    uint32_t flags, void *descriptor)
{
	struct request_data *driver_data = context->pDriverData;
	VADRMPRIMESurfaceDescriptor *surface_descriptor = descriptor;
	struct object_surface *surface_object;
	struct video_format *video_format;
	int *export_fds = NULL;
	unsigned int export_fds_count;
	unsigned int planes_count;
	unsigned int capture_type;
	unsigned int size;
	unsigned int i;
	VAStatus status;
	int rc;

	video_format = driver_data->video_format;
	if (video_format == NULL)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
		return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	export_fds_count = surface_object->destination_buffers_count;
	export_fds = malloc(export_fds_count * sizeof(*export_fds));

	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

	rc = v4l2_export_buffer(driver_data->video_fd, capture_type,
				surface_object->destination_index, O_RDONLY,
				export_fds, export_fds_count);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	planes_count = surface_object->destination_planes_count;

	surface_descriptor->fourcc = surface_object->pd.fmt_vaapi;
	surface_descriptor->width = surface_object->pd.req_width;
	surface_descriptor->height = surface_object->pd.req_height;
	surface_descriptor->num_objects = surface_object->pd.buffer_count;

	size = 0;

	if (export_fds_count == 1)
		for (i = 0; i < planes_count; i++)
			size += surface_object->destination_sizes[i];

	for (i = 0; i < export_fds_count; i++) {
		surface_descriptor->objects[i].drm_format_modifier =
		    surface_object[i].destination_modifiers[i];
		surface_descriptor->objects[i].fd = export_fds[i];
		surface_descriptor->objects[i].size = export_fds_count == 1 ?
						      size :
						      surface_object->destination_sizes[i];
	}

	surface_descriptor->num_layers = 1;

	surface_descriptor->layers[0].drm_format = video_format->drm_format;
	surface_descriptor->layers[0].num_planes = planes_count;

	for (i = 0; i < planes_count; i++) {
		surface_descriptor->layers[0].object_index[i] =
			export_fds_count == 1 ? 0 : i;
		surface_descriptor->layers[0].offset[i] =
			surface_object->destination_offsets[i];
		surface_descriptor->layers[0].pitch[i] =
			surface_object->destination_bytesperlines[i];
	}

	status = VA_STATUS_SUCCESS;
	goto complete;

error:
	for (i = 0; i < export_fds_count; i++)
		if (export_fds[i] >= 0)
			close(export_fds[i]);

complete:
	if (export_fds != NULL)
		free(export_fds);

	return status;
}
