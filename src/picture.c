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
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include <errno.h>

#include <sys/ioctl.h>

#include <linux/videodev2.h>

#include "dmabufs.h"
#include "media.h"
#include "utils.h"
#include "v4l2.h"

#include "autoconfig.h"


struct bit_block {
	VABufferType buftype;
	size_t offset;  /* As we may realloc our buffer use offset not ptr */
	size_t len;
	unsigned int final_bits;
	bool render_last;
};

struct bit_stash {
	unsigned int block_size;
	unsigned int block_len;
	struct bit_block * blocks;
	size_t data_size;
	size_t data_len;
	uint8_t * data;
};

static void bit_stash_delete(struct bit_stash *const bs)
{
	if (!bs)
		return;
	free(bs->blocks);
	free(bs->data);
	free(bs);
}

static struct bit_stash * bit_stash_new(void)
{
	struct bit_stash *const bs = calloc(1, sizeof(*bs));
	return bs;
}

static void bit_stash_reset(struct bit_stash *const bs)
{
	bs->block_len = 0;
	bs->data_len = 0;
}

static unsigned int bit_blocks(const struct bit_stash *const bs)
{
	return bs->block_len;
}

static VABufferType bit_block_type(const struct bit_stash *const bs,
				   const unsigned int n)
{
	return n >= bs->block_len ? 0 : bs->blocks[n].buftype;
}

static const void * bit_block_data(const struct bit_stash *const bs,
				   const unsigned int n)
{
	return n >= bs->block_len ? NULL : bs->data + bs->blocks[n].offset;
}

static size_t bit_block_len(const struct bit_stash *const bs,
	       		    const unsigned int n)
{
	return n >= bs->block_len ? 0 : bs->blocks[n].len;
}

static bool bit_block_last(const struct bit_stash *const bs,
	       		   const unsigned int n)
{
	return n >= bs->block_len || bs->blocks[n].render_last;
}


static unsigned int sizebits(size_t x)
{
	unsigned int n = 0;
	if ((x >> 16) != 0) {
		x >>= 16;
		n += 16;
	}
	if ((x >> 8) != 0) {
		x >>= 8;
		n += 8;
	}
	if ((x >> 4) != 0) {
		x >>= 4;
		n += 4;
	}
	if ((x >> 2) != 0) {
		x >>= 2;
		n += 2;
	}
	if ((x >> 1) != 0) {
		x >>= 1;
		n += 1;
	}
	return n + x;
}

/* Round up to next pwr of 2
 * if already a pwr of 2 will pick next pwr
 */
static size_t rndup(size_t x)
{
	return (size_t)1 << sizebits(x);
}

#define DATA_ALIGN 64

/* We are going to be doing a lot of copying :-(
 * make it as easy as possible
 */
uint8_t * dataalign(uint8_t * p)
{
	return (uint8_t *)(((uintptr_t)p + DATA_ALIGN - 1) & ~(DATA_ALIGN - 1));
}

static VAStatus bit_block_add(struct bit_stash *const bs,
			      const VABufferType buftype,
			      const void *src, const size_t len,
			      const unsigned int final_bits,
			      const bool render_last)
{
	struct bit_block *bb = bs->blocks;
	uint8_t *dst = dataalign(bs->data + bs->data_len);
	uint8_t *p;
	size_t alen;

	if (bs->block_len <= bs->block_size) {
		unsigned int n = bs->block_len < 8 ? 8 : bs->block_len * 2;
		bb = realloc(bb, n * sizeof(*bb));
		if (!bb)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		bs->block_size = n;
		bs->blocks = bb;
	}

	alen = (dst + len) - bs->data;
	if (!bs->data || alen > bs->data_size) {
		/* Add a little to the alloc size to cope with realloc maybe
		 * not aligning on the boundary we've picked
		*/
		alen = rndup(alen + DATA_ALIGN);
		p = realloc(bs->data, alen);
		if (!p)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		bs->data = p;
		bs->data_size = alen;
		dst = dataalign(p + bs->data_len);
	}

	memcpy(dst, src, len);

	bb[bs->block_len++] = (struct bit_block) {
		.buftype = buftype,
		.offset = dst - bs->data,
		.len = len,
		.final_bits = final_bits,
		.render_last = render_last
	};
	bs->data_len = dst + len - bs->data;

	return VA_STATUS_SUCCESS;
}


static VAStatus codec_store_buffer(struct mediabuf_qent *src_qent,
				   VAProfile profile,
				   struct object_surface *const surf,
				   const VABufferType buftype,
				   const void * data, const size_t len)
{
	request_log("%s: buftype=%d, qent=%p\n", __func__, buftype, src_qent);
	switch (buftype) {
	case VASliceDataBufferType:
		if (src_qent) {
			surf->source_data = data;
			surf->needs_flush = true;
			qent_src_data_copy(src_qent, data, len);
		}
		break;

	case VAPictureParameterBufferType:
		switch (profile) {
		case VAProfileMPEG2Simple:
		case VAProfileMPEG2Main:
			memcpy(&surf->params.mpeg2.picture,
			       data,
			       sizeof(surf->params.mpeg2.picture));
			break;

		case VAProfileH264Main:
		case VAProfileH264High:
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264MultiviewHigh:
		case VAProfileH264StereoHigh:
			memcpy(&surf->params.h264.picture,
			       data,
			       sizeof(surf->params.h264.picture));
			break;

		case VAProfileHEVCMain:
		case VAProfileHEVCMain10:
			memcpy(&surf->params.h265.picture,
			       data,
			       sizeof(surf->params.h265.picture));
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
			memcpy(&surf->params.h264.slice,
			       data,
			       sizeof(surf->params.h264.slice));
			break;

		case VAProfileHEVCMain:
		case VAProfileHEVCMain10:
			memcpy(&surf->params.h265.slice,
			       data,
			       sizeof(surf->params.h265.slice));
			break;

		default:
			break;
		}
		break;

	case VAIQMatrixBufferType:
		switch (profile) {
		case VAProfileMPEG2Simple:
		case VAProfileMPEG2Main:
			memcpy(&surf->params.mpeg2.iqmatrix,
			       data,
			       sizeof(surf->params.mpeg2.iqmatrix));
			surf->params.mpeg2.iqmatrix_set = true;
			break;

		case VAProfileH264Main:
		case VAProfileH264High:
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264MultiviewHigh:
		case VAProfileH264StereoHigh:
			memcpy(&surf->params.h264.matrix,
			       data,
			       sizeof(surf->params.h264.matrix));
			break;

		case VAProfileHEVCMain:
		case VAProfileHEVCMain10:
			memcpy(&surf->params.h265.iqmatrix,
			       data,
			       sizeof(surf->params.h265.iqmatrix));
			surf->params.h265.iqmatrix_set = true;
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
			   struct object_context *ctx,
			   struct object_config *cfg,
			   struct object_surface *surf,
			   struct mediabuf_qent * src_qent,
			   bool is_last)
{
	VAStatus rc;
	struct media_request * mreq;

	surf->needs_flush = false;

	mreq = media_request_get(driver_data->media_pool);
	if (!mreq) {
		request_log("media_request_get failed\n");
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}

	rc = codec_set_controls(driver_data, ctx,
				cfg->profile, mreq, surf);
	if (rc != VA_STATUS_SUCCESS) {
		request_log("codec_set_controls failed\n");
		return rc;
	}

	rc = mediabufs_start_request(ctx->mbc, mreq,
				     src_qent,
				     surf->req_one ? surf->qent : NULL,
				     is_last);
	surf->source_data = NULL;
	surf->req_one = false;
	if (rc != VA_STATUS_SUCCESS) {
		request_log("mediabufs_start_request failed\n");
		return rc;
	}

	rc = queue_await_completion(driver_data, surf, is_last);
	if (rc != VA_STATUS_SUCCESS) {
		request_log("queue_await_completion failed\n");
		return rc;
	}
	return VA_STATUS_SUCCESS;
}

VAStatus RequestBeginPicture(VADriverContextP context, VAContextID context_id,
			     VASurfaceID surface_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_context *ctx;
	struct object_surface *surf;

	ctx = CONTEXT(driver_data, context_id);
	if (ctx == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	surf = SURFACE(driver_data, surface_id);
	if (surf == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	request_log("%s: SID=%#x: status=%d\n", __func__, surface_id, surf->status);

	if (surf->status == VASurfaceRendering)
		RequestSyncSurface(context, surface_id);

	/* *** Move to a better stash point than surface? */
	if (!surf->bit_stash)
		surf->bit_stash = bit_stash_new();
	bit_stash_reset(surf->bit_stash);

	surf->status = VASurfaceRendering;
	ctx->render_surface_id = surface_id;

//	gettimeofday(&surf->timestamp, NULL);

	++ctx->timeseq.tv_sec;
	surf->timestamp = ctx->timeseq;
	surf->req_one = true;
	surf->needs_flush = false;

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
		buffer_object = BUFFER(driver_data, buffers_ids[i]);

		rv = bit_block_add(surface_object->bit_stash,
				   buffer_object->type,
				   buffer_object->data,
				   buffer_object->size * buffer_object->count,
				   0,
				   i + 1 >= buffers_count);
		if (rv != VA_STATUS_SUCCESS)
			return rv;
	}

	return VA_STATUS_SUCCESS;
}
static VAStatus stream_start(struct request_data *const rd,
			     struct object_context *const ctx,
			     const struct object_config *const cfg,
			     struct object_surface *const os)
{
	VAStatus status;
	unsigned int i;

	if (ctx->stream_started)
		return VA_STATUS_SUCCESS;

	/* Set controls onto video handle not request */
	status = codec_set_controls(rd, ctx, cfg->profile, NULL, os);
	if (status != VA_STATUS_SUCCESS)
		return status;

	status = mediabufs_dst_fmt_set(ctx->mbc,
				      os->pd.req_rtfmt,
				      ctx->picture_width, ctx->picture_height);
	if (status != VA_STATUS_SUCCESS)
		return status;

	/* Alloc src buffers */
	status = mediabufs_src_pool_create(ctx->mbc, rd->dmabufs_ctrl, 6);
	if (status != VA_STATUS_SUCCESS)
		return status;

	/* Need dst buffers before stream on */
	for (i = 0; i < ctx->surfaces_count; ++i) {
		struct object_surface *surf;
		surf = SURFACE(rd, ctx->surfaces_ids[i]);
		if (!surf) {
			request_log("Surface %#x attached to context not found\n");
		}
		else {
			surface_attach(surf, ctx->mbc, rd->dmabufs_ctrl, ctx->base.id);
		}
	}


	status = mediabufs_stream_on(ctx->mbc);
	if (status != VA_STATUS_SUCCESS)
		return status;

	ctx->stream_started = true;

	return VA_STATUS_SUCCESS;
}


VAStatus RequestEndPicture(VADriverContextP context, VAContextID context_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_context *ctx;
	struct object_config *cfg;
	struct object_surface *surf;
	VAStatus rv;
	unsigned int n;
	unsigned int i;
	bool first_decode = true;
	struct mediabuf_qent *src_qent = NULL;

	ctx = CONTEXT(driver_data, context_id);
	if (ctx == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	cfg = CONFIG(driver_data, ctx->config_id);
	if (cfg == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	surf = SURFACE(driver_data, ctx->render_surface_id);
	if (surf == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	n = bit_blocks(surf->bit_stash);
	request_log("Has %d bit objects\n", n);

	/* Get SPS/PPS before starting stream */
	if (!ctx->stream_started) {
		for (i = 0; i < n; i++) {
			if (bit_block_type(surf->bit_stash, i) != VAPictureParameterBufferType)
				continue;
			rv = codec_store_buffer(src_qent, cfg->profile,
						surf,
						bit_block_type(surf->bit_stash, i),
						bit_block_data(surf->bit_stash, i),
						bit_block_len(surf->bit_stash, i));
			if (rv != VA_STATUS_SUCCESS)
				return rv;

			request_log("Start stream\n");
			rv = stream_start(driver_data, ctx, cfg, surf);
			if (rv != VA_STATUS_SUCCESS)
				return rv;
			break;
		}
		if (!ctx->stream_started) {
			request_log("No SPS/PPS in picture\n");
			return VA_STATUS_ERROR_INVALID_PARAMETER;
		}
	}

	rv = surface_attach(surf, ctx->mbc, driver_data->dmabufs_ctrl, context_id);
	if (rv != VA_STATUS_SUCCESS)
		return rv;

	for (i = 0; i < n; i++) {
		if (!src_qent) {
			src_qent = mediabufs_src_qent_get(ctx->mbc);
			if (!src_qent) {
				request_log("Failed to get src qent");
				return VA_STATUS_ERROR_ALLOCATION_FAILED;
			}

			qent_src_params_set(src_qent, &surf->timestamp);
		}

		rv = codec_store_buffer(src_qent, cfg->profile,
					surf,
					bit_block_type(surf->bit_stash, i),
					bit_block_data(surf->bit_stash, i),
					bit_block_len(surf->bit_stash, i));
		if (rv != VA_STATUS_SUCCESS)
			return rv;

		if (surf->needs_flush) {
			rv = flush_data(driver_data, ctx, cfg, surf, src_qent, i + 1 >= n);
			src_qent = NULL; /* Src ent consumed by being Qed */
			if (rv != VA_STATUS_SUCCESS)
				return rv;
		}
	}

	if (src_qent) {
		request_log("Yikes! **** SRC_QENT set\n");
	}

	ctx->render_surface_id = VA_INVALID_ID;

	return VA_STATUS_SUCCESS;
}
