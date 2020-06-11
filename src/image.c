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

#include "image.h"
#include "buffer.h"
#include "request.h"
#include "surface.h"
#include "video.h"

#include <assert.h>
#include <string.h>

#include "dmabufs.h"
#include "tiled_yuv.h"
#include "utils.h"
#include "v4l2.h"
#include "drm_fourcc.h"

VAStatus RequestCreateImage(VADriverContextP context, VAImageFormat *format,
			    int width, int height, VAImage *img)
{
	struct request_data *const driver_data = context->pDriverData;
	const uint32_t rwidth  = (width + 15) & ~15;
	const uint32_t rheight = (height + 15) & ~15;
	const VAImageID id = object_heap_allocate(&driver_data->image_heap);
	struct object_image *const iobj = IMAGE(driver_data, id);
	VAStatus status;

	if (!iobj)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	switch (format->fourcc) {
	case VA_FOURCC_NV12:
		iobj->image = (VAImage){
			.image_id = id,
			.format = *format,
			.width = width,
			.height = height,
			.data_size = rwidth * rheight * 3 / 2,
			.num_planes = 2,
			.pitches = {rwidth, rwidth},
			.offsets = {0, rwidth * rheight}
		};
		break;
	case VA_FOURCC_P010:
		iobj->image = (VAImage){
			.image_id = id,
			.format = *format,
			.width = width,
			.height = height,
			.data_size = rwidth * rheight * 3,
			.num_planes = 2,
			.pitches = {rwidth * 2, rwidth * 2},
			.offsets = {0, rwidth * rheight * 2}
		};
		break;
	default:
		return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
	}

	status = RequestCreateBuffer(context, 0, VAImageBufferType, iobj->image.data_size, 1,
				     NULL, &iobj->image.buf);
	if (status != VA_STATUS_SUCCESS) {
		object_heap_free(&driver_data->image_heap, &iobj->base);
		return status;
	}

	*img = iobj->image;
	return VA_STATUS_SUCCESS;
}

VAStatus RequestDestroyImage(VADriverContextP context, VAImageID image_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_image *image_object;
	VAStatus status;

	image_object = IMAGE(driver_data, image_id);
	if (image_object == NULL)
		return VA_STATUS_ERROR_INVALID_IMAGE;

	status = RequestDestroyBuffer(context, image_object->image.buf);
	if (status != VA_STATUS_SUCCESS)
		return status;

	object_heap_free(&driver_data->image_heap,
			 (struct object_base *)image_object);

	return VA_STATUS_SUCCESS;
}

static void av_rpi_sand_to_planar_y(uint8_t * dst, const unsigned int dst_stride,
                             const uint8_t * src,
                             unsigned int stride1, unsigned int stride2,
                             unsigned int _x, unsigned int y,
                             unsigned int _w, unsigned int h)
{
    const unsigned int x = _x;
    const unsigned int w = _w;
    const unsigned int mask = stride1 - 1;

    if ((x & ~mask) == ((x + w) & ~mask)) {
        // All in one sand stripe
        const uint8_t * p = src + (x & mask) + y * stride1 + (x & ~mask) * stride2;
        for (unsigned int i = 0; i != h; ++i, dst += dst_stride, p += stride1) {
            memcpy(dst, p, w);
        }
    }
    else
    {
        // Two+ stripe
        const unsigned int sstride = stride1 * stride2;
        const uint8_t * p1 = src + (x & mask) + y * stride1 + (x & ~mask) * stride2;
        const uint8_t * p2 = p1 + sstride - (x & mask);
        const unsigned int w1 = stride1 - (x & mask);
        const unsigned int w3 = (x + w) & mask;
        const unsigned int w2 = w - (w1 + w3);

        for (unsigned int i = 0; i != h; ++i, dst += dst_stride, p1 += stride1, p2 += stride1) {
            unsigned int j;
            const uint8_t * p = p2;
            uint8_t * d = dst;
            memcpy(d, p1, w1);
            d += w1;
            for (j = 0; j < w2; j += stride1, d += stride1, p += sstride) {
                memcpy(d, p, stride1);
            }
            memcpy(d, p, w3);
        }
    }
}

// Fetches a single patch - offscreen fixup not done here
// w <= stride1
// unclipped
// _x & _w in pixels, strides in bytes
// P010 has the data in the hi 10 bits of the u16
void av_rpi_sand30_to_p010(uint8_t * dst, const unsigned int dst_stride,
                             const uint8_t * src,
                             unsigned int stride1, unsigned int stride2,
                             unsigned int _x, unsigned int y,
                             unsigned int _w, unsigned int h)
{
    const unsigned int x0 = (_x / 3) * 4; // Byte offset of the word
    const unsigned int xskip0 = _x - (x0 >> 2) * 3;
    const unsigned int x1 = ((_x + _w) / 3) * 4;
    const unsigned int xrem1 = _x + _w - (x1 >> 2) * 3;
    const unsigned int mask = stride1 - 1;
    const uint8_t * p0 = src + (x0 & mask) + y * stride1 + (x0 & ~mask) * stride2;
    const unsigned int slice_inc = ((stride2 - 1) * stride1) >> 2;  // RHS of a stripe to LHS of next in words

    if (x0 == x1) {
        // *******************
        // Partial single word xfer
        return;
    }

    for (unsigned int i = 0; i != h; ++i, dst += dst_stride, p0 += stride1)
    {
        unsigned int x = x0;
        const uint32_t * p = (const uint32_t *)p0;
        uint16_t * d = (uint16_t *)dst;

        if (xskip0 != 0) {
            const uint32_t p3 = *p++;

            if (xskip0 == 1)
                *d++ = ((p3 >> 10) & 0x3ff) << 6;
            *d++ = ((p3 >> 20) & 0x3ff) << 6;

            if (((x += 4) & mask) == 0)
                p += slice_inc;
        }

        while (x != x1) {
            const uint32_t p3 = *p++;
            *d++ = (p3 & 0x3ff) << 6;
            *d++ = ((p3 >> 10) & 0x3ff) << 6;
            *d++ = ((p3 >> 20) & 0x3ff) << 6;

            if (((x += 4) & mask) == 0)
                p += slice_inc;
        }

        if (xrem1 != 0) {
            const uint32_t p3 = *p;

            *d++ = (p3 & 0x3ff) << 6;
            if (xrem1 == 2)
                *d++ = ((p3 >> 10) & 0x3ff) << 6;
        }
    }
}

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

static void sand_to_planar_nv12(struct request_data *driver_data,
			   struct object_surface *const surf,
			   VAImage *image,
			   struct object_buffer *buffer_object,
			   const unsigned int i)
{
	const uint8_t *const s =
		qent_dst_data(surf->qent, surf->pd.planes[i].buf) + surf->pd.planes[i].offset;
	uint8_t *const d = (uint8_t *)buffer_object->data + image->offsets[i];
	unsigned int w = MIN(image->width,  surf->pd.planes[0].width);
	unsigned int h = MIN(i == 0 ? image->height : image->height / 2,
			     surf->pd.planes[i].height);
    request_log("%s:[%d] w=%d, h=%d\n", __func__, i, w, h);
	av_rpi_sand_to_planar_y(d, image->pitches[i],
				s, 128,
				surf->pd.planes[i].col_height,
				0, 0, w, h);
}

static void sand30_to_planar_p010(struct request_data *driver_data,
			   struct object_surface *const surf,
			   VAImage *image,
			   struct object_buffer *buffer_object,
			   const unsigned int i)
{
	const uint8_t *const s =
		qent_dst_data(surf->qent, surf->pd.planes[i].buf) + surf->pd.planes[i].offset;
	uint8_t *const d = (uint8_t *)buffer_object->data + image->offsets[i];
	unsigned int w = MIN(image->width,  surf->pd.planes[0].width);
	unsigned int h = MIN(i == 0 ? image->height : image->height / 2,
			     surf->pd.planes[i].height);

	av_rpi_sand30_to_p010(d, image->pitches[i],
				s, 128,
			      surf->pd.planes[i].col_height,
				0, 0, w, h);
}

static VAStatus copy_surface_to_image (struct request_data *driver_data,
				       struct object_surface *const surf,
				       VAImage *const image)
{
	struct object_buffer *buffer_object;
	unsigned int i;
	VAStatus status = VA_STATUS_SUCCESS;

	buffer_object = BUFFER(driver_data, image->buf);
	if (buffer_object == NULL)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	status = qent_dst_read_start(surf->qent);
	if (status != VA_STATUS_SUCCESS)
		return status;

	for (i = 0; i < surf->pd.plane_count; i++) {
		switch (surf->pd.fmt_v4l2) {
		case V4L2_PIX_FMT_NV12_COL128:
			sand_to_planar_nv12(driver_data, surf,
					    image, buffer_object, i);
			break;
		case V4L2_PIX_FMT_NV12_10_COL128:
			sand30_to_planar_p010(driver_data, surf,
					    image, buffer_object, i);
			break;
		case V4L2_PIX_FMT_SUNXI_TILED_NV12:
			tiled_to_planar(qent_dst_data(surf->qent, surf->pd.planes[i].buf) +
						surf->pd.planes[i].offset,
					buffer_object->data + image->offsets[i],
					image->pitches[i], image->width,
					i == 0 ? image->height :
						 image->height / 2);
			break;
		case V4L2_PIX_FMT_NV12:
			memcpy(buffer_object->data + image->offsets[i],
			       qent_dst_data(surf->qent, surf->pd.planes[i].buf) + surf->pd.planes[i].offset,
			       surf->pd.planes[i].stride * surf->pd.planes[i].height);
			break;
		default:
			status = VA_STATUS_ERROR_UNIMPLEMENTED;
			break;
		}
	}

	qent_dst_read_stop(surf->qent);
	return status;
}

VAStatus RequestDeriveImage(VADriverContextP context, VASurfaceID surface_id,
			    VAImage *image)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;
	struct object_buffer *buffer_object;
	VAImageFormat format;
	VAStatus status;

    return VA_STATUS_ERROR_UNIMPLEMENTED;

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	if (surface_object->status == VASurfaceRendering) {
		status = RequestSyncSurface(context, surface_id);
		if (status != VA_STATUS_SUCCESS)
			return status;
	}

	format.fourcc = VA_FOURCC_NV12;

	status = RequestCreateImage(context, &format, surface_object->pd.req_width,
				    surface_object->pd.req_height, image);
	if (status != VA_STATUS_SUCCESS)
		return status;

	status = copy_surface_to_image (driver_data, surface_object, image);
	if (status != VA_STATUS_SUCCESS)
		return status;

	surface_object->status = VASurfaceReady;

	buffer_object = BUFFER(driver_data, image->buf);
	buffer_object->derived_surface_id = surface_id;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestQueryImageFormats(VADriverContextP context,
				  VAImageFormat *formats, int *formats_count)
{
	VAImageFormat * f = formats;
	(f++)->fourcc = VA_FOURCC_NV12;
	(f++)->fourcc = VA_FOURCC_P010;
	*formats_count = f - formats;

#if V4L2_REQUEST_MAX_IMAGE_FORMATS < 2
#error Image formats overflow
#endif

	return VA_STATUS_SUCCESS;
}

VAStatus RequestSetImagePalette(VADriverContextP context, VAImageID image_id,
				unsigned char *palette)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus RequestGetImage(VADriverContextP context, VASurfaceID surface_id,
			 int x, int y, unsigned int width, unsigned int height,
			 VAImageID image_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;
	struct object_image *image_object;
	VAImage *image;

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	image_object = IMAGE(driver_data, image_id);
	if (image_object == NULL)
		return VA_STATUS_ERROR_INVALID_IMAGE;

	image = &image_object->image;
	if (x != 0 || y != 0 || width != image->width || height != image->height)
		return VA_STATUS_ERROR_UNIMPLEMENTED;

	return copy_surface_to_image (driver_data, surface_object, image);
}

VAStatus RequestPutImage(VADriverContextP context, VASurfaceID surface_id,
			 VAImageID image, int src_x, int src_y,
			 unsigned int src_width, unsigned int src_height,
			 int dst_x, int dst_y, unsigned int dst_width,
			 unsigned int dst_height)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}
