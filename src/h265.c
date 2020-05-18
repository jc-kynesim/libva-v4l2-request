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

#include "mpeg2.h"
#include "context.h"
#include "request.h"
#include "surface.h"

#include <assert.h>
#include <string.h>
#include <inttypes.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>
#include <hevc-ctrls.h>

#include "v4l2.h"

#define H265_NAL_UNIT_TYPE_SHIFT		1
#define H265_NAL_UNIT_TYPE_MASK			((1 << 6) - 1)
#define H265_NUH_TEMPORAL_ID_PLUS1_SHIFT	0
#define H265_NUH_TEMPORAL_ID_PLUS1_MASK		((1 << 3) - 1)

static void h265_fill_pps(VAPictureParameterBufferHEVC *picture,
			  VASliceParameterBufferHEVC *slice,
			  struct v4l2_ctrl_hevc_pps *pps)
{
	unsigned int i;
	memset(pps, 0, sizeof(*pps));

	if (slice->LongSliceFlags.fields.dependent_slice_segment_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT;
	if (picture->slice_parsing_fields.bits.output_flag_present_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT;
	pps->num_extra_slice_header_bits =
		picture->num_extra_slice_header_bits;
	if (picture->pic_fields.bits.sign_data_hiding_enabled_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED;
	if (picture->slice_parsing_fields.bits.cabac_init_present_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT;
	pps->init_qp_minus26 = picture->init_qp_minus26;
	if (picture->pic_fields.bits.constrained_intra_pred_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED;
	if (picture->pic_fields.bits.transform_skip_enabled_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED;
	if (picture->pic_fields.bits.cu_qp_delta_enabled_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED;
	pps->diff_cu_qp_delta_depth = picture->diff_cu_qp_delta_depth;
	pps->pps_cb_qp_offset = picture->pps_cb_qp_offset;
	pps->pps_cr_qp_offset = picture->pps_cr_qp_offset;
	if (picture->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT;
	if (picture->pic_fields.bits.weighted_pred_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED;
	if (picture->pic_fields.bits.weighted_bipred_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED;
	if (picture->pic_fields.bits.transquant_bypass_enabled_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED;
	if (picture->pic_fields.bits.tiles_enabled_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_TILES_ENABLED;
	if (picture->pic_fields.bits.entropy_coding_sync_enabled_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED;
	pps->num_tile_columns_minus1 = picture->num_tile_columns_minus1;
	pps->num_tile_rows_minus1 = picture->num_tile_rows_minus1;
	for (i = 0; i < pps->num_tile_columns_minus1; ++i)
		pps->column_width_minus1[i] = picture->column_width_minus1[i];
	for (i = 0; i < pps->num_tile_rows_minus1; ++i)
		pps->row_height_minus1[i] = picture->row_height_minus1[i];
	if (picture->pic_fields.bits.loop_filter_across_tiles_enabled_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED;
	if (picture->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED;
	if (picture->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED;
	if (picture->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER;
	pps->pps_beta_offset_div2 = picture->pps_beta_offset_div2;
	pps->pps_tc_offset_div2 = picture->pps_tc_offset_div2;
	if (picture->slice_parsing_fields.bits.lists_modification_present_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT;
	pps->log2_parallel_merge_level_minus2 =
		picture->log2_parallel_merge_level_minus2;
}

static void h265_fill_sps(VAPictureParameterBufferHEVC *picture,
			  struct v4l2_ctrl_hevc_sps *sps)
{
	memset(sps, 0, sizeof(*sps));

	sps->chroma_format_idc = picture->pic_fields.bits.chroma_format_idc;
	if (picture->pic_fields.bits.separate_colour_plane_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE;
	sps->pic_width_in_luma_samples = picture->pic_width_in_luma_samples;
	sps->pic_height_in_luma_samples = picture->pic_height_in_luma_samples;
	sps->bit_depth_luma_minus8 = picture->bit_depth_luma_minus8;
	sps->bit_depth_chroma_minus8 = picture->bit_depth_chroma_minus8;
	sps->log2_max_pic_order_cnt_lsb_minus4 =
		picture->log2_max_pic_order_cnt_lsb_minus4;
	sps->sps_max_dec_pic_buffering_minus1 =
		picture->sps_max_dec_pic_buffering_minus1;
	sps->sps_max_num_reorder_pics = 0;
	sps->sps_max_latency_increase_plus1 = 0;
	sps->log2_min_luma_coding_block_size_minus3 =
		picture->log2_min_luma_coding_block_size_minus3;
	sps->log2_diff_max_min_luma_coding_block_size =
		picture->log2_diff_max_min_luma_coding_block_size;
	sps->log2_min_luma_transform_block_size_minus2 =
		picture->log2_min_transform_block_size_minus2;
	sps->log2_diff_max_min_luma_transform_block_size =
		picture->log2_diff_max_min_transform_block_size;
	sps->max_transform_hierarchy_depth_inter =
		picture->max_transform_hierarchy_depth_inter;
	sps->max_transform_hierarchy_depth_intra =
		picture->max_transform_hierarchy_depth_intra;
	if (picture->pic_fields.bits.scaling_list_enabled_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED;
	if (picture->pic_fields.bits.amp_enabled_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_AMP_ENABLED;
	if (picture->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET;
	if (picture->pic_fields.bits.pcm_enabled_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_PCM_ENABLED;
	sps->pcm_sample_bit_depth_luma_minus1 =
		picture->pcm_sample_bit_depth_luma_minus1;
	sps->pcm_sample_bit_depth_chroma_minus1 =
		picture->pcm_sample_bit_depth_chroma_minus1;
	sps->log2_min_pcm_luma_coding_block_size_minus3 =
		picture->log2_min_pcm_luma_coding_block_size_minus3;
	sps->log2_diff_max_min_pcm_luma_coding_block_size =
		picture->log2_diff_max_min_pcm_luma_coding_block_size;
	if (picture->pic_fields.bits.pcm_loop_filter_disabled_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED;
	sps->num_short_term_ref_pic_sets = picture->num_short_term_ref_pic_sets;
	if (picture->slice_parsing_fields.bits.long_term_ref_pics_present_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_LONG_TERM_REF_PICS_PRESENT;
	sps->num_long_term_ref_pics_sps = picture->num_long_term_ref_pic_sps;
	if (picture->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED;
	if (picture->pic_fields.bits.strong_intra_smoothing_enabled_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED;
}

static void h265_fill_slice_params(VAPictureParameterBufferHEVC *picture,
				   VASliceParameterBufferHEVC *slice,
				   struct object_heap *surface_heap,
				   void *source_data,
				   struct v4l2_ctrl_hevc_slice_params *slice_params)
{
	struct object_surface *surface_object;
	VAPictureHEVC *hevc_picture;
	uint8_t nal_unit_type;
	uint8_t nuh_temporal_id_plus1;
	uint32_t data_bit_offset;
	uint8_t pic_struct;
	uint8_t field_pic;
	uint8_t slice_type;
	unsigned int num_active_dpb_entries;
	unsigned int num_rps_poc_st_curr_before;
	unsigned int num_rps_poc_st_curr_after;
	unsigned int num_rps_poc_lt_curr;
	uint8_t *b;
	unsigned int count;
	unsigned int o, i, j;

	/* Extract the missing NAL header information. */

	b = source_data + slice->slice_data_offset;

	nal_unit_type = (b[0] >> H265_NAL_UNIT_TYPE_SHIFT) &
			H265_NAL_UNIT_TYPE_MASK;
	nuh_temporal_id_plus1 = (b[1] >> H265_NUH_TEMPORAL_ID_PLUS1_SHIFT) &
				H265_NUH_TEMPORAL_ID_PLUS1_MASK;

	/*
	 * VAAPI only provides a byte-aligned value for the slice segment data
	 * offset, although it appears that the offset is not always aligned.
	 * Search for the first one bit in the previous byte, that marks the
	 * start of the slice segment to correct the value.
	 */

	b = source_data + (slice->slice_data_offset +
			   slice->slice_data_byte_offset) - 1;

	for (o = 0; o < 8; o++)
		if (*b & (1 << o))
			break;

	/* Include the one bit. */
	o++;

	data_bit_offset = (slice->slice_data_offset +
			   slice->slice_data_byte_offset) * 8 - o;

	memset(slice_params, 0, sizeof(*slice_params));

	slice_params->bit_size = slice->slice_data_size * 8;
	slice_params->data_bit_offset = data_bit_offset;
	slice_params->nal_unit_type = nal_unit_type;
	slice_params->nuh_temporal_id_plus1 = nuh_temporal_id_plus1;
	slice_params->slice_segment_addr = slice->slice_segment_address;

	slice_type = slice->LongSliceFlags.fields.slice_type;

	slice_params->slice_type = slice_type,
	slice_params->colour_plane_id =
		slice->LongSliceFlags.fields.color_plane_id;
	slice_params->slice_pic_order_cnt =
		picture->CurrPic.pic_order_cnt;
	if (slice->LongSliceFlags.fields.dependent_slice_segment_flag)
		slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT;
	if (slice->LongSliceFlags.fields.slice_sao_luma_flag)
		slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA;
	if (slice->LongSliceFlags.fields.slice_sao_chroma_flag)
		slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA;
	if (slice->LongSliceFlags.fields.slice_temporal_mvp_enabled_flag)
		slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED;
	slice_params->num_ref_idx_l0_active_minus1 =
		slice->num_ref_idx_l0_active_minus1;
	slice_params->num_ref_idx_l1_active_minus1 =
		slice->num_ref_idx_l1_active_minus1;
	if (slice->LongSliceFlags.fields.mvd_l1_zero_flag)
		slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO;
	if (slice->LongSliceFlags.fields.cabac_init_flag)
		slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT;
	if (slice->LongSliceFlags.fields.collocated_from_l0_flag)
		slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0;
	slice_params->collocated_ref_idx = slice->collocated_ref_idx;
	slice_params->five_minus_max_num_merge_cand =
		slice->five_minus_max_num_merge_cand;
	slice_params->slice_qp_delta = slice->slice_qp_delta;
	slice_params->slice_cb_qp_offset = slice->slice_cb_qp_offset;
	slice_params->slice_cr_qp_offset = slice->slice_cr_qp_offset;
	slice_params->slice_act_y_qp_offset = 0;
	slice_params->slice_act_cb_qp_offset = 0;
	slice_params->slice_act_cr_qp_offset = 0;
	if (slice->LongSliceFlags.fields.slice_deblocking_filter_disabled_flag)
		slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED;
	slice_params->slice_beta_offset_div2 = slice->slice_beta_offset_div2;
	slice_params->slice_tc_offset_div2 = slice->slice_tc_offset_div2;
	if (slice->LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag)
		slice_params->flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED;

	if (picture->CurrPic.flags & VA_PICTURE_HEVC_FIELD_PIC) {
		if (picture->CurrPic.flags & VA_PICTURE_HEVC_BOTTOM_FIELD)
			pic_struct = 2;
		else
			pic_struct = 1;
	} else {
		pic_struct = 0;
	}

	slice_params->pic_struct = pic_struct;

	num_active_dpb_entries = 0;
	num_rps_poc_st_curr_before = 0;
	num_rps_poc_st_curr_after = 0;
	num_rps_poc_lt_curr = 0;

	/* Some V4L2 decoders (rpivid) need DPB entries even for I-frames
	 * to manage frame aux info alloc/free
	 *
	 * *** If VAAPI can't guarantee a DPB entry V4L2 may need to think
	 *     harder.
	 */
	for (i = 0; i < 15 ; i++) {
		uint64_t timestamp;

		hevc_picture = &picture->ReferenceFrames[i];

		if (hevc_picture->picture_id == VA_INVALID_SURFACE ||
		    (hevc_picture->flags & VA_PICTURE_HEVC_INVALID) != 0)
			break;

		surface_object = (struct object_surface *)
			object_heap_lookup(surface_heap,
					   hevc_picture->picture_id);
		if (surface_object == NULL)
			break;

		timestamp = v4l2_timeval_to_ns(&surface_object->timestamp);
		slice_params->dpb[i].timestamp = timestamp;

		if ((hevc_picture->flags & VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE) != 0) {
			slice_params->dpb[i].rps =
				V4L2_HEVC_DPB_ENTRY_RPS_ST_CURR_BEFORE;
			num_rps_poc_st_curr_before++;
		} else if ((hevc_picture->flags & VA_PICTURE_HEVC_RPS_ST_CURR_AFTER) != 0) {
			slice_params->dpb[i].rps =
				V4L2_HEVC_DPB_ENTRY_RPS_ST_CURR_AFTER;
			num_rps_poc_st_curr_after++;
		} else if ((hevc_picture->flags & VA_PICTURE_HEVC_RPS_LT_CURR) != 0) {
			slice_params->dpb[i].rps =
				V4L2_HEVC_DPB_ENTRY_RPS_LT_CURR;
			num_rps_poc_lt_curr++;
		}

		field_pic = !!(hevc_picture->flags & VA_PICTURE_HEVC_FIELD_PIC);

		slice_params->dpb[i].field_pic = field_pic;

		/* TODO: Interleaved: Get the POC for each field. */
		slice_params->dpb[i].pic_order_cnt[0] =
			hevc_picture->pic_order_cnt;

		num_active_dpb_entries++;
	}

	slice_params->num_active_dpb_entries = num_active_dpb_entries;

	count = slice_params->num_ref_idx_l0_active_minus1 + 1;

	for (i = 0; i < count && slice_type != V4L2_HEVC_SLICE_TYPE_I; i++)
		slice_params->ref_idx_l0[i] = slice->RefPicList[0][i];

	count = slice_params->num_ref_idx_l1_active_minus1 + 1;

	for (i = 0; i < count && slice_type == V4L2_HEVC_SLICE_TYPE_B ; i++)
		slice_params->ref_idx_l1[i] = slice->RefPicList[1][i];

	slice_params->num_rps_poc_st_curr_before = num_rps_poc_st_curr_before;
	slice_params->num_rps_poc_st_curr_after = num_rps_poc_st_curr_after;
	slice_params->num_rps_poc_lt_curr = num_rps_poc_lt_curr;

	slice_params->pred_weight_table.luma_log2_weight_denom =
		slice->luma_log2_weight_denom;
	slice_params->pred_weight_table.delta_chroma_log2_weight_denom =
		slice->delta_chroma_log2_weight_denom;

	for (i = 0; i < 15 && slice_type != V4L2_HEVC_SLICE_TYPE_I; i++) {
		slice_params->pred_weight_table.delta_luma_weight_l0[i] =
			slice->delta_luma_weight_l0[i];
		slice_params->pred_weight_table.luma_offset_l0[i] =
			slice->luma_offset_l0[i];

		for (j = 0; j < 2; j++) {
			slice_params->pred_weight_table.delta_chroma_weight_l0[i][j] =
				slice->delta_chroma_weight_l0[i][j];
			slice_params->pred_weight_table.chroma_offset_l0[i][j] =
				slice->ChromaOffsetL0[i][j];
		}
	}

	for (i = 0; i < 15 && slice_type == V4L2_HEVC_SLICE_TYPE_B; i++) {
		slice_params->pred_weight_table.delta_luma_weight_l1[i] =
			slice->delta_luma_weight_l1[i];
		slice_params->pred_weight_table.luma_offset_l1[i] =
			slice->luma_offset_l1[i];

		for (j = 0; j < 2; j++) {
			slice_params->pred_weight_table.delta_chroma_weight_l1[i][j] =
				slice->delta_chroma_weight_l1[i][j];
			slice_params->pred_weight_table.chroma_offset_l1[i][j] =
				slice->ChromaOffsetL1[i][j];
		}
	}
}

static void h265_fill_scaling_matrix(const VAIQMatrixBufferHEVC * src,
				    struct v4l2_ctrl_hevc_scaling_matrix * dst)
{
	memcpy(dst->scaling_list_4x4, src->ScalingList4x4, sizeof(dst->scaling_list_4x4));
	memcpy(dst->scaling_list_8x8, src->ScalingList8x8, sizeof(dst->scaling_list_8x8));
	memcpy(dst->scaling_list_16x16, src->ScalingList16x16, sizeof(dst->scaling_list_16x16));
	memcpy(dst->scaling_list_32x32, src->ScalingList32x32, sizeof(dst->scaling_list_32x32));
	memcpy(dst->scaling_list_dc_coef_16x16, src->ScalingListDC16x16, sizeof(dst->scaling_list_dc_coef_16x16));
	memcpy(dst->scaling_list_dc_coef_32x32, src->ScalingListDC32x32, sizeof(dst->scaling_list_dc_coef_32x32));
}

int h265_set_controls(struct request_data *driver_data,
		      struct object_context *context_object,
		      struct object_surface *surface_object)
{
	VAPictureParameterBufferHEVC *picture =
		&surface_object->params.h265.picture;
	VASliceParameterBufferHEVC *slice =
		&surface_object->params.h265.slice;
	VAIQMatrixBufferHEVC *iqmatrix =
		&surface_object->params.h265.iqmatrix;
	bool iqmatrix_set = surface_object->params.h265.iqmatrix_set;
	struct v4l2_ctrl_hevc_pps pps;
	struct v4l2_ctrl_hevc_sps sps;
	struct v4l2_ctrl_hevc_slice_params slice_params;
	struct v4l2_ctrl_hevc_scaling_matrix scaling_matrix;
	int rc;

	h265_fill_pps(picture, slice, &pps);

	rc = v4l2_set_control(driver_data->video_fd, surface_object->request_fd,
			      V4L2_CID_MPEG_VIDEO_HEVC_PPS, &pps, sizeof(pps));
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	h265_fill_sps(picture, &sps);

	rc = v4l2_set_control(driver_data->video_fd, surface_object->request_fd,
			      V4L2_CID_MPEG_VIDEO_HEVC_SPS, &sps, sizeof(sps));
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	h265_fill_slice_params(picture, slice, &driver_data->surface_heap,
			       surface_object->source_data, &slice_params);

	rc = v4l2_set_control(driver_data->video_fd, surface_object->request_fd,
			      V4L2_CID_MPEG_VIDEO_HEVC_SLICE_PARAMS,
			      &slice_params, sizeof(slice_params));
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	memset(&scaling_matrix, 0, sizeof(scaling_matrix));
	if (iqmatrix_set)
		h265_fill_scaling_matrix(iqmatrix, &scaling_matrix);

	rc = v4l2_set_control(driver_data->video_fd, surface_object->request_fd,
			      V4L2_CID_MPEG_VIDEO_HEVC_SCALING_MATRIX,
			      &scaling_matrix, sizeof(scaling_matrix));
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;


	return 0;
}
