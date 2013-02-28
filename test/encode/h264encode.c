/*
 * Copyright (c) 2007-2013 Intel Corporation. All Rights Reserved.
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
#include "sysdeps.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <assert.h>
#include <pthread.h>
#include <va/va.h>
#include <va/va_enc_h264.h>
#include "va_display.h"

#define CHECK_VASTATUS(va_status,func)                                  \
    if (va_status != VA_STATUS_SUCCESS) {                               \
        fprintf(stderr,"%s:%s (%d) failed,exit\n", __func__, func, __LINE__); \
        exit(1);                                                        \
    }

#include "../loadsurface.h"

#define NAL_REF_IDC_NONE        0
#define NAL_REF_IDC_LOW         1
#define NAL_REF_IDC_MEDIUM      2
#define NAL_REF_IDC_HIGH        3

#define NAL_NON_IDR             1
#define NAL_IDR                 5
#define NAL_SPS                 7
#define NAL_PPS                 8
#define NAL_SEI			6

#define SLICE_TYPE_P            0
#define SLICE_TYPE_B            1
#define SLICE_TYPE_I            2

#define ENTROPY_MODE_CAVLC      0
#define ENTROPY_MODE_CABAC      1

#define PROFILE_IDC_BASELINE    66
#define PROFILE_IDC_MAIN        77
#define PROFILE_IDC_HIGH        100
   
#define BITSTREAM_ALLOCATE_STEPPING     4096


#define SRC_SURFACE_NUM 16 /* 16 surfaces for source YUV */
#define REF_SURFACE_NUM 16 /* 16 surfaces for reference */
static  VADisplay va_dpy;
static  VAProfile h264_profile;
static  VAConfigAttrib attrib[VAConfigAttribTypeMax];
static  VAConfigAttrib config_attrib[VAConfigAttribTypeMax];
static  int config_attrib_num = 0;
static  VASurfaceID src_surface[SRC_SURFACE_NUM];
static  VABufferID coded_buf[SRC_SURFACE_NUM];
static  VASurfaceID ref_surface[REF_SURFACE_NUM];
static  VAConfigID config_id;
static  VAContextID context_id;
static  VAEncSequenceParameterBufferH264 seq_param;
static  VAEncPictureParameterBufferH264 pic_param;
static  VAEncSliceParameterBufferH264 slice_param;
static  VAPictureH264 LLastCurrPic, LastCurrPic, CurrentCurrPic; /* One reference for P and two for B */

static  int constraint_set_flag = 0;
static  int h264_packedheader = 0; /* support pack header? */
static  int h264_maxref = 3;
static  char *coded_fn = NULL, *srcyuv_fn = NULL, *recyuv_fn = NULL;
static  FILE *coded_fp = NULL, *srcyuv_fp = NULL, *recyuv_fp = NULL;
static  unsigned long long srcyuv_frames = 0;
static  unsigned int srcyuv_fourcc = VA_FOURCC_NV12;

static  int frame_width = 176;
static  int frame_height = 144;
static  int frame_rate = 30;
static  unsigned int frame_count = 60;
static  unsigned int frame_coded = 0;
static  unsigned int frame_bitrate = 0;
static  unsigned int frame_slices = 1;
static  int initial_qp = 28;
static  int minimal_qp = 0;
static  int intra_period = 30;
static  int intra_idr_period = 60;
static  int ip_period = 1;
static  int rc_mode = VA_RC_VBR;
static  int slice_refoverride = 0;
static  unsigned long long current_frame_encoding = 0;
static  unsigned long long current_frame_display = 0;
static  int current_frame_num = 0;
static  int current_frame_type;

/* thread to save coded data/upload source YUV */
struct storage_task_t {
    void *next;
    unsigned long long display_order;
    unsigned long long encode_order;
};
static  struct storage_task_t *storage_task_header = NULL, *storage_task_tail = NULL;
#define SRC_SURFACE_IN_ENCODING 0
#define SRC_SURFACE_IN_STORAGE  1
static  int srcsurface_status[SRC_SURFACE_NUM];
static  int encode_syncmode = 0;
static  pthread_mutex_t encode_mutex = PTHREAD_MUTEX_INITIALIZER;
static  pthread_cond_t  encode_cond = PTHREAD_COND_INITIALIZER;
static  pthread_t encode_thread;

/* for performance profiling */
static unsigned int UploadPictureTicks=0;
static unsigned int BeginPictureTicks=0;
static unsigned int RenderPictureTicks=0;
static unsigned int EndPictureTicks=0;
static unsigned int SyncPictureTicks=0;
static unsigned int SavePictureTicks=0;
static unsigned int TotalTicks=0;

struct __bitstream {
    unsigned int *buffer;
    int bit_offset;
    int max_size_in_dword;
};
typedef struct __bitstream bitstream;


static unsigned int 
va_swap32(unsigned int val)
{
    unsigned char *pval = (unsigned char *)&val;

    return ((pval[0] << 24)     |
            (pval[1] << 16)     |
            (pval[2] << 8)      |
            (pval[3] << 0));
}

static void
bitstream_start(bitstream *bs)
{
    bs->max_size_in_dword = BITSTREAM_ALLOCATE_STEPPING;
    bs->buffer = calloc(bs->max_size_in_dword * sizeof(int), 1);
    bs->bit_offset = 0;
}

static void
bitstream_end(bitstream *bs)
{
    int pos = (bs->bit_offset >> 5);
    int bit_offset = (bs->bit_offset & 0x1f);
    int bit_left = 32 - bit_offset;

    if (bit_offset) {
        bs->buffer[pos] = va_swap32((bs->buffer[pos] << bit_left));
    }
}
 
static void
bitstream_put_ui(bitstream *bs, unsigned int val, int size_in_bits)
{
    int pos = (bs->bit_offset >> 5);
    int bit_offset = (bs->bit_offset & 0x1f);
    int bit_left = 32 - bit_offset;

    if (!size_in_bits)
        return;

    bs->bit_offset += size_in_bits;

    if (bit_left > size_in_bits) {
        bs->buffer[pos] = (bs->buffer[pos] << size_in_bits | val);
    } else {
        size_in_bits -= bit_left;
        bs->buffer[pos] = (bs->buffer[pos] << bit_left) | (val >> size_in_bits);
        bs->buffer[pos] = va_swap32(bs->buffer[pos]);

        if (pos + 1 == bs->max_size_in_dword) {
            bs->max_size_in_dword += BITSTREAM_ALLOCATE_STEPPING;
            bs->buffer = realloc(bs->buffer, bs->max_size_in_dword * sizeof(unsigned int));
        }

        bs->buffer[pos + 1] = val;
    }
}

static void
bitstream_put_ue(bitstream *bs, unsigned int val)
{
    int size_in_bits = 0;
    int tmp_val = ++val;

    while (tmp_val) {
        tmp_val >>= 1;
        size_in_bits++;
    }

    bitstream_put_ui(bs, 0, size_in_bits - 1); // leading zero
    bitstream_put_ui(bs, val, size_in_bits);
}

static void
bitstream_put_se(bitstream *bs, int val)
{
    unsigned int new_val;

    if (val <= 0)
        new_val = -2 * val;
    else
        new_val = 2 * val - 1;

    bitstream_put_ue(bs, new_val);
}

static void
bitstream_byte_aligning(bitstream *bs, int bit)
{
    int bit_offset = (bs->bit_offset & 0x7);
    int bit_left = 8 - bit_offset;
    int new_val;

    if (!bit_offset)
        return;

    assert(bit == 0 || bit == 1);

    if (bit)
        new_val = (1 << bit_left) - 1;
    else
        new_val = 0;

    bitstream_put_ui(bs, new_val, bit_left);
}

static void 
rbsp_trailing_bits(bitstream *bs)
{
    bitstream_put_ui(bs, 1, 1);
    bitstream_byte_aligning(bs, 0);
}

static void nal_start_code_prefix(bitstream *bs)
{
    bitstream_put_ui(bs, 0x00000001, 32);
}

static void nal_header(bitstream *bs, int nal_ref_idc, int nal_unit_type)
{
    bitstream_put_ui(bs, 0, 1);                /* forbidden_zero_bit: 0 */
    bitstream_put_ui(bs, nal_ref_idc, 2);
    bitstream_put_ui(bs, nal_unit_type, 5);
}

static void sps_rbsp(bitstream *bs)
{
    int profile_idc = PROFILE_IDC_BASELINE;

    if (h264_profile  == VAProfileH264High)
        profile_idc = PROFILE_IDC_HIGH;
    else if (h264_profile  == VAProfileH264Main)
        profile_idc = PROFILE_IDC_MAIN;

    bitstream_put_ui(bs, profile_idc, 8);               /* profile_idc */
    bitstream_put_ui(bs, !!(constraint_set_flag & 1), 1);                         /* constraint_set0_flag */
    bitstream_put_ui(bs, !!(constraint_set_flag & 2), 1);                         /* constraint_set1_flag */
    bitstream_put_ui(bs, !!(constraint_set_flag & 4), 1);                         /* constraint_set2_flag */
    bitstream_put_ui(bs, !!(constraint_set_flag & 8), 1);                         /* constraint_set3_flag */
    bitstream_put_ui(bs, 0, 4);                         /* reserved_zero_4bits */
    bitstream_put_ui(bs, seq_param.level_idc, 8);      /* level_idc */
    bitstream_put_ue(bs, seq_param.seq_parameter_set_id);      /* seq_parameter_set_id */

    if ( profile_idc == PROFILE_IDC_HIGH) {
        bitstream_put_ue(bs, 1);        /* chroma_format_idc = 1, 4:2:0 */ 
        bitstream_put_ue(bs, 0);        /* bit_depth_luma_minus8 */
        bitstream_put_ue(bs, 0);        /* bit_depth_chroma_minus8 */
        bitstream_put_ui(bs, 0, 1);     /* qpprime_y_zero_transform_bypass_flag */
        bitstream_put_ui(bs, 0, 1);     /* seq_scaling_matrix_present_flag */
    }

    bitstream_put_ue(bs, seq_param.seq_fields.bits.log2_max_frame_num_minus4); /* log2_max_frame_num_minus4 */
    bitstream_put_ue(bs, seq_param.seq_fields.bits.pic_order_cnt_type);        /* pic_order_cnt_type */

    if (seq_param.seq_fields.bits.pic_order_cnt_type == 0)
        bitstream_put_ue(bs, seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);     /* log2_max_pic_order_cnt_lsb_minus4 */
    else {
        assert(0);
    }

    bitstream_put_ue(bs, seq_param.max_num_ref_frames);        /* num_ref_frames */
    bitstream_put_ui(bs, 0, 1);                                 /* gaps_in_frame_num_value_allowed_flag */

    bitstream_put_ue(bs, seq_param.picture_width_in_mbs - 1);  /* pic_width_in_mbs_minus1 */
    bitstream_put_ue(bs, seq_param.picture_height_in_mbs - 1); /* pic_height_in_map_units_minus1 */
    bitstream_put_ui(bs, seq_param.seq_fields.bits.frame_mbs_only_flag, 1);    /* frame_mbs_only_flag */

    if (!seq_param.seq_fields.bits.frame_mbs_only_flag) {
        assert(0);
    }

    bitstream_put_ui(bs, seq_param.seq_fields.bits.direct_8x8_inference_flag, 1);      /* direct_8x8_inference_flag */
    bitstream_put_ui(bs, seq_param.frame_cropping_flag, 1);            /* frame_cropping_flag */

    if (seq_param.frame_cropping_flag) {
        bitstream_put_ue(bs, seq_param.frame_crop_left_offset);        /* frame_crop_left_offset */
        bitstream_put_ue(bs, seq_param.frame_crop_right_offset);       /* frame_crop_right_offset */
        bitstream_put_ue(bs, seq_param.frame_crop_top_offset);         /* frame_crop_top_offset */
        bitstream_put_ue(bs, seq_param.frame_crop_bottom_offset);      /* frame_crop_bottom_offset */
    }
    
    //if ( frame_bit_rate < 0 ) { //TODO EW: the vui header isn't correct
    if ( 1 ) {
        bitstream_put_ui(bs, 0, 1); /* vui_parameters_present_flag */
    } else {
        bitstream_put_ui(bs, 1, 1); /* vui_parameters_present_flag */
        bitstream_put_ui(bs, 0, 1); /* aspect_ratio_info_present_flag */
        bitstream_put_ui(bs, 0, 1); /* overscan_info_present_flag */
        bitstream_put_ui(bs, 0, 1); /* video_signal_type_present_flag */
        bitstream_put_ui(bs, 0, 1); /* chroma_loc_info_present_flag */
        bitstream_put_ui(bs, 1, 1); /* timing_info_present_flag */
        {
            bitstream_put_ui(bs, 15, 32);
            bitstream_put_ui(bs, 900, 32);
            bitstream_put_ui(bs, 1, 1);
        }
        bitstream_put_ui(bs, 1, 1); /* nal_hrd_parameters_present_flag */
        {
            // hrd_parameters 
            bitstream_put_ue(bs, 0);    /* cpb_cnt_minus1 */
            bitstream_put_ui(bs, 4, 4); /* bit_rate_scale */
            bitstream_put_ui(bs, 6, 4); /* cpb_size_scale */
           
            bitstream_put_ue(bs, frame_bitrate - 1); /* bit_rate_value_minus1[0] */
            bitstream_put_ue(bs, frame_bitrate*8 - 1); /* cpb_size_value_minus1[0] */
            bitstream_put_ui(bs, 1, 1);  /* cbr_flag[0] */

            bitstream_put_ui(bs, 23, 5);   /* initial_cpb_removal_delay_length_minus1 */
            bitstream_put_ui(bs, 23, 5);   /* cpb_removal_delay_length_minus1 */
            bitstream_put_ui(bs, 23, 5);   /* dpb_output_delay_length_minus1 */
            bitstream_put_ui(bs, 23, 5);   /* time_offset_length  */
        }
        bitstream_put_ui(bs, 0, 1);   /* vcl_hrd_parameters_present_flag */
        bitstream_put_ui(bs, 0, 1);   /* low_delay_hrd_flag */ 

        bitstream_put_ui(bs, 0, 1); /* pic_struct_present_flag */
        bitstream_put_ui(bs, 0, 1); /* bitstream_restriction_flag */
    }

    rbsp_trailing_bits(bs);     /* rbsp_trailing_bits */
}


static void pps_rbsp(bitstream *bs)
{
    bitstream_put_ue(bs, pic_param.pic_parameter_set_id);      /* pic_parameter_set_id */
    bitstream_put_ue(bs, pic_param.seq_parameter_set_id);      /* seq_parameter_set_id */

    bitstream_put_ui(bs, pic_param.pic_fields.bits.entropy_coding_mode_flag, 1);  /* entropy_coding_mode_flag */

    bitstream_put_ui(bs, 0, 1);                         /* pic_order_present_flag: 0 */

    bitstream_put_ue(bs, 0);                            /* num_slice_groups_minus1 */

    bitstream_put_ue(bs, pic_param.num_ref_idx_l0_active_minus1);      /* num_ref_idx_l0_active_minus1 */
    bitstream_put_ue(bs, pic_param.num_ref_idx_l1_active_minus1);      /* num_ref_idx_l1_active_minus1 1 */

    bitstream_put_ui(bs, pic_param.pic_fields.bits.weighted_pred_flag, 1);     /* weighted_pred_flag: 0 */
    bitstream_put_ui(bs, pic_param.pic_fields.bits.weighted_bipred_idc, 2);	/* weighted_bipred_idc: 0 */

    bitstream_put_se(bs, pic_param.pic_init_qp - 26);  /* pic_init_qp_minus26 */
    bitstream_put_se(bs, 0);                            /* pic_init_qs_minus26 */
    bitstream_put_se(bs, 0);                            /* chroma_qp_index_offset */

    bitstream_put_ui(bs, pic_param.pic_fields.bits.deblocking_filter_control_present_flag, 1); /* deblocking_filter_control_present_flag */
    bitstream_put_ui(bs, 0, 1);                         /* constrained_intra_pred_flag */
    bitstream_put_ui(bs, 0, 1);                         /* redundant_pic_cnt_present_flag */
    
    /* more_rbsp_data */
    bitstream_put_ui(bs, pic_param.pic_fields.bits.transform_8x8_mode_flag, 1);    /*transform_8x8_mode_flag */
    bitstream_put_ui(bs, 0, 1);                         /* pic_scaling_matrix_present_flag */
    bitstream_put_se(bs, pic_param.second_chroma_qp_index_offset );    /*second_chroma_qp_index_offset */

    rbsp_trailing_bits(bs);
}


static int
build_packed_pic_buffer(unsigned char **header_buffer)
{
    bitstream bs;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);
    nal_header(&bs, NAL_REF_IDC_HIGH, NAL_PPS);
    pps_rbsp(&bs);
    bitstream_end(&bs);

    *header_buffer = (unsigned char *)bs.buffer;
    return bs.bit_offset;
}

static int
build_packed_seq_buffer(unsigned char **header_buffer)
{
    bitstream bs;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);
    nal_header(&bs, NAL_REF_IDC_HIGH, NAL_SPS);
    sps_rbsp(&bs);
    bitstream_end(&bs);

    *header_buffer = (unsigned char *)bs.buffer;
    return bs.bit_offset;
}

static int 
build_packed_sei_buffer_timing(unsigned int init_cpb_removal_length,
				unsigned int init_cpb_removal_delay,
				unsigned int init_cpb_removal_delay_offset,
				unsigned int cpb_removal_length,
				unsigned int cpb_removal_delay,
				unsigned int dpb_output_length,
				unsigned int dpb_output_delay,
				unsigned char **sei_buffer)
{
    unsigned char *byte_buf;
    int bp_byte_size, i, pic_byte_size;

    bitstream nal_bs;
    bitstream sei_bp_bs, sei_pic_bs;

    bitstream_start(&sei_bp_bs);
    bitstream_put_ue(&sei_bp_bs, 0);       /*seq_parameter_set_id*/
    bitstream_put_ui(&sei_bp_bs, init_cpb_removal_delay, cpb_removal_length); 
    bitstream_put_ui(&sei_bp_bs, init_cpb_removal_delay_offset, cpb_removal_length); 
    if ( sei_bp_bs.bit_offset & 0x7) {
        bitstream_put_ui(&sei_bp_bs, 1, 1);
    }
    bitstream_end(&sei_bp_bs);
    bp_byte_size = (sei_bp_bs.bit_offset + 7) / 8;
    
    bitstream_start(&sei_pic_bs);
    bitstream_put_ui(&sei_pic_bs, cpb_removal_delay, cpb_removal_length); 
    bitstream_put_ui(&sei_pic_bs, dpb_output_delay, dpb_output_length); 
    if ( sei_pic_bs.bit_offset & 0x7) {
        bitstream_put_ui(&sei_pic_bs, 1, 1);
    }
    bitstream_end(&sei_pic_bs);
    pic_byte_size = (sei_pic_bs.bit_offset + 7) / 8;
    
    bitstream_start(&nal_bs);
    nal_start_code_prefix(&nal_bs);
    nal_header(&nal_bs, NAL_REF_IDC_NONE, NAL_SEI);

	/* Write the SEI buffer period data */    
    bitstream_put_ui(&nal_bs, 0, 8);
    bitstream_put_ui(&nal_bs, bp_byte_size, 8);
    
    byte_buf = (unsigned char *)sei_bp_bs.buffer;
    for(i = 0; i < bp_byte_size; i++) {
        bitstream_put_ui(&nal_bs, byte_buf[i], 8);
    }
    free(byte_buf);
	/* write the SEI timing data */
    bitstream_put_ui(&nal_bs, 0x01, 8);
    bitstream_put_ui(&nal_bs, pic_byte_size, 8);
    
    byte_buf = (unsigned char *)sei_pic_bs.buffer;
    for(i = 0; i < pic_byte_size; i++) {
        bitstream_put_ui(&nal_bs, byte_buf[i], 8);
    }
    free(byte_buf);

    rbsp_trailing_bits(&nal_bs);
    bitstream_end(&nal_bs);

    *sei_buffer = (unsigned char *)nal_bs.buffer; 
   
    return nal_bs.bit_offset;
}



/*
 * Helper function for profiling purposes
 */
static unsigned int GetTickCount()
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL))
        return 0;
    return tv.tv_usec/1000+tv.tv_sec*1000;
}

/*
  Assume frame sequence is: Frame#0,#1,#2,...,#M,...,#X,... (encoding order)
  1) period between Frame #X and Frame #N = #X - #N
  2) 0 means infinite for intra_period/intra_idr_period, and 0 is invalid for ip_period
  3) intra_idr_period % intra_period (intra_period > 0) and intra_period % ip_period must be 0
  4) intra_period and intra_idr_period take precedence over ip_period
  5) if ip_period > 1, intra_period and intra_idr_period are not  the strict periods 
     of I/IDR frames, see bellow examples
  -------------------------------------------------------------------
  intra_period intra_idr_period ip_period frame sequence (intra_period/intra_idr_period/ip_period)
  0            ignored          1          IDRPPPPPPP ...     (No IDR/I any more)
  0            ignored        >=2          IDR(PBB)(PBB)...   (No IDR/I any more)
  1            0                ignored    IDRIIIIIII...      (No IDR any more)
  1            1                ignored    IDR IDR IDR IDR...
  1            >=2              ignored    IDRII IDRII IDR... (1/3/ignore)
  >=2          0                1          IDRPPP IPPP I...   (3/0/1)
  >=2          0              >=2          IDR(PBB)(PBB)(IBB) (6/0/3)
                                              (PBB)(IBB)(PBB)(IBB)... 
  >=2          >=2              1          IDRPPPPP IPPPPP IPPPPP (6/18/1)
                                           IDRPPPPP IPPPPP IPPPPP...
  >=2          >=2              >=2        {IDR(PBB)(PBB)(IBB)(PBB)(IBB)(PBB)} (6/18/3)
                                           {IDR(PBB)(PBB)(IBB)(PBB)(IBB)(PBB)}...
                                           {IDR(PBB)(PBB)(IBB)(PBB)}           (6/12/3)
                                           {IDR(PBB)(PBB)(IBB)(PBB)}...
                                           {IDR(PBB)(PBB)}                     (6/6/3)
                                           {IDR(PBB)(PBB)}.
*/

/*
 * Return displaying order with specified periods and encoding order
 * displaying_order: displaying order
 * frame_type: frame type 
 */
#define FRAME_P 0
#define FRAME_B 1
#define FRAME_I 2
#define FRAME_IDR 7
void encoding2display_order(
    unsigned long long encoding_order,int intra_period,
    int intra_idr_period,int ip_period,
    unsigned long long *displaying_order,
    int *frame_type)
{
    int encoding_order_gop = 0;

    if (intra_period == 1) { /* all are I/IDR frames */
        *displaying_order = encoding_order;
        if (intra_idr_period == 0)
            *frame_type = (encoding_order == 0)?FRAME_IDR:FRAME_I;
        else
            *frame_type = (encoding_order % intra_idr_period == 0)?FRAME_IDR:FRAME_I;
        return;
    }

    if (intra_period == 0)
        intra_idr_period = 0;

    /* new sequence like
     * IDR PPPPP IPPPPP
     * IDR (PBB)(PBB)(IBB)(PBB)
     */
    encoding_order_gop = (intra_idr_period == 0)? encoding_order:
        (encoding_order % (intra_idr_period + ((ip_period == 1)?0:1)));
         
    if (encoding_order_gop == 0) { /* the first frame */
        *frame_type = FRAME_IDR;
        *displaying_order = encoding_order;
    } else if (((encoding_order_gop - 1) % ip_period) != 0) { /* B frames */
	*frame_type = FRAME_B;
        *displaying_order = encoding_order - 1;
    } else if ((intra_period != 0) && /* have I frames */
               (encoding_order_gop >= 2) &&
               ((ip_period == 1 && encoding_order_gop % intra_period == 0) || /* for IDR PPPPP IPPPP */
                /* for IDR (PBB)(PBB)(IBB) */
                (ip_period >= 2 && ((encoding_order_gop - 1) / ip_period % (intra_period / ip_period)) == 0))) {
	*frame_type = FRAME_I;
	*displaying_order = encoding_order + ip_period - 1;
    } else {
	*frame_type = FRAME_P;
	*displaying_order = encoding_order + ip_period - 1;
    }
}


static char *fourcc_to_string(int fourcc)
{
    switch (fourcc) {
    case VA_FOURCC_NV12:
        return "NONE";
    case VA_FOURCC_IYUV:
        return "IYUV";
    case VA_FOURCC_YV12:
        return "YV12";
    case VA_FOURCC_UYVY:
        return "UYVY";
    default:
        return "Unknown";
    }
}

static int string_to_fourcc(char *str)
{
    int fourcc;
    
    if (strncmp(str, "NV12", 4))
        fourcc = VA_FOURCC_NV12;
    else if (strncmp(str, "IYUV", 4))
        fourcc = VA_FOURCC_IYUV;
    else if (strncmp(str, "YV12", 4))
        fourcc = VA_FOURCC_YV12;
    else if (strncmp(str, "UYVY", 4))
        fourcc = VA_FOURCC_UYVY;
    else {
        printf("Unknow FOURCC\n");
        fourcc = -1;
    }
    return fourcc;
}


static char *rc_to_string(int rcmode)
{
    switch (rc_mode) {
    case VA_RC_NONE:
        return "NONE";
    case VA_RC_CBR:
        return "CBR";
    case VA_RC_VBR:
        return "VBR";
    case VA_RC_VCM:
        return "VCM";
    case VA_RC_CQP:
        return "CQP";
    case VA_RC_VBR_CONSTRAINED:
        return "VBR_CONSTRAINED";
    default:
        return "Unknown";
    }
}

static int string_to_rc(char *str)
{
    int rc_mode;
    
    if (strncmp(str, "NONE", 4))
        rc_mode = VA_RC_NONE;
    else if (strncmp(str, "CBR", 3))
        rc_mode = VA_RC_CBR;
    else if (strncmp(str, "VBR", 3))
        rc_mode = VA_RC_VBR;
    else if (strncmp(str, "VCM", 3))
        rc_mode = VA_RC_VCM;
    else if (strncmp(str, "CQP", 3))
        rc_mode = VA_RC_CQP;
    else if (strncmp(str, "VBR_CONSTRAINED", 15))
        rc_mode = VA_RC_VBR_CONSTRAINED;
    else {
        printf("Unknow RC mode\n");
        rc_mode = -1;
    }
    return rc_mode;
}


static int print_help(void)
{
    printf("./h264encode <options>\n");
    printf("   -w <width> -h <height>\n");
    printf("   -n <frame number>\n");
    printf("   -o <coded file>\n");
    printf("   -f <frame rate>\n");
    printf("   --intra_period <number>\n");
    printf("   --idr_period <number>\n");
    printf("   --ip_period <number>\n");
    printf("   --bitrate <bitrate>\n");
    printf("   --initialqp <number>\n");
    printf("   --minqp <number>\n");
    printf("   --rcmode <NONE|CBR|VBR|VCM|CQP|VBR_CONTRAINED>\n");
    printf("   --refoverride: use VAEncSliceParameterBufferH264 to override reference frames\n");
    printf("   --syncmode: sequentially upload source, encoding, save result, no multi-thread\n");
    printf("   --srcyuv <filename> load YUV from a file\n");
    printf("   --fourcc <NV12|IYUV|I420|YV12> source YUV fourcc\n");

    return 0;
}

static int process_cmdline(int argc, char *argv[])
{
    char c;
    const struct option long_opts[] = {
        {"bitrate", required_argument, NULL, 1 },
        {"minqp", required_argument, NULL, 2 },
        {"initialqp", required_argument, NULL, 3 },
        {"intra_period", required_argument, NULL, 4 },
        {"idr_period", required_argument, NULL, 5 },
        {"ip_period", required_argument, NULL, 6 },
        {"rcmode", required_argument, NULL, 7 },
        {"refoverride", no_argument, NULL, 8 },
        {"srcyuv", required_argument, NULL, 9 },
        {"fourcc", required_argument, NULL, 10 },
        {"syncmode", no_argument, NULL, 11 },
        {NULL, no_argument, NULL, 0 }};
    int long_index;
    
    while ((c =getopt_long_only(argc,argv,"w:h:n:f:o:?",long_opts,&long_index)) != EOF) {
        switch (c) {
        case 'w':
            frame_width = atoi(optarg);
            break;
        case 'h':
            frame_height = atoi(optarg);
            break;
        case 'n':
            frame_count = atoi(optarg);
            break;
        case 'f':
            frame_rate = atoi(optarg);
            break;
        case 'o':
            coded_fn = strdup(optarg);
            break;
        case 1:
            frame_bitrate = atoi(optarg);
            break;
        case 2:
            minimal_qp = atoi(optarg);
            break;
        case 3:
            initial_qp = atoi(optarg);
            break;
        case 4:
            intra_period = atoi(optarg);
            break;
        case 5:
            intra_idr_period = atoi(optarg);
            break;
        case 6:
            ip_period = atoi(optarg);
            break;
        case 7:
            rc_mode = string_to_rc(optarg);
            if (rc_mode < 0) {
                print_help();
                exit(1);
            }
            break;
        case 8:
            slice_refoverride = 1;
            break;
        case 9:
            srcyuv_fn = strdup(optarg);
            break;
        case 10:
            srcyuv_fourcc = string_to_fourcc(optarg);
            if (srcyuv_fourcc < 0) {
                print_help();
                exit(1);
            }
            break;
        case 11:
            encode_syncmode = 1;
            break;
        case ':':
        case '?':
            print_help();
            exit(0);
        }
    }

    if (ip_period < 1) {
	printf(" ip_period must be greater than 0\n");
        exit(0);
    }
    if (intra_period != 1 && intra_period % ip_period != 0) {
	printf(" intra_period must be a multiplier of ip_period\n");
        exit(0);        
    }
    if (intra_period != 0 && intra_idr_period % intra_period != 0) {
	printf(" intra_idr_period must be a multiplier of intra_period\n");
        exit(0);        
    }

    if (frame_bitrate == 0) {
        frame_bitrate = frame_width * frame_height * 12 * frame_rate / 50;
        printf("Set bitrate to %dbps\n", frame_bitrate);
    }
    /* open source file */
    if (srcyuv_fn) {
        srcyuv_fp = fopen(srcyuv_fn,"r");
    
        if (srcyuv_fp == NULL)
            printf("Open source YUV file %s failed, use auto-generated YUV data\n", srcyuv_fn);
        else {
            fseek(srcyuv_fp, 0L, SEEK_END);
            srcyuv_frames = ftell(srcyuv_fp) / (frame_width * frame_height * 1.5);
            printf("Source YUV file %s with %llu frames\n", srcyuv_fn, srcyuv_frames);
        }
    }
    
    if (coded_fn == NULL) {
        struct stat buf;
        if (stat("/tmp", &buf) == 0)
            coded_fn = strdup("/tmp/test.264");
        else if (stat("/sdcard", &buf) == 0)
            coded_fn = strdup("/sdcard/test.264");
        else
            coded_fn = strdup("./test.264");
    }
    
    /* store coded data into a file */
    coded_fp = fopen(coded_fn,"w+");
    if (coded_fp == NULL) {
        printf("Open file %s failed, exit\n", coded_fn);
        exit(1);
    }

    
    return 0;
}

static int init_va(void)
{
    VAProfile profile_list[]={VAProfileH264High,VAProfileH264Main,VAProfileH264Baseline,VAProfileH264ConstrainedBaseline};
    VAEntrypoint entrypoints[VAEntrypointMax]={0};
    int num_entrypoints,slice_entrypoint;
    int support_encode = 0;    
    int major_ver, minor_ver;
    VAStatus va_status;
    int i;

    va_dpy = va_open_display();
    va_status = vaInitialize(va_dpy, &major_ver, &minor_ver);
    CHECK_VASTATUS(va_status, "vaInitialize");

    /* use the highest profile */
    for (i = 0; i < sizeof(profile_list)/sizeof(profile_list[0]); i++) {
        h264_profile = profile_list[i];
        vaQueryConfigEntrypoints(va_dpy, h264_profile, entrypoints, &num_entrypoints);
        for (slice_entrypoint = 0; slice_entrypoint < num_entrypoints; slice_entrypoint++) {
            if (entrypoints[slice_entrypoint] == VAEntrypointEncSlice) {
                support_encode = 1;
                break;
            }
        }
        if (support_encode == 1)
            break;
    }
    
    if (support_encode == 0) {
        printf("Can't find VAEntrypointEncSlice for H264 profiles\n");
        exit(1);
    } else {
        switch (h264_profile) {
            case VAProfileH264Baseline:
                printf("Use profile VAProfileH264Baseline\n");
                ip_period = 1;
                constraint_set_flag |= (1 << 0); /* Annex A.2.1 */
                break;
            case VAProfileH264ConstrainedBaseline:
                printf("Use profile VAProfileH264ConstrainedBaseline\n");
                constraint_set_flag |= (1 << 0 | 1 << 1); /* Annex A.2.2 */
                ip_period = 1;
                break;

            case VAProfileH264Main:
                printf("Use profile VAProfileH264Main\n");
                constraint_set_flag |= (1 << 1); /* Annex A.2.2 */
                break;

            case VAProfileH264High:
                constraint_set_flag |= (1 << 3); /* Annex A.2.4 */
                printf("Use profile VAProfileH264High\n");
                break;
            default:
                printf("unknow profile. Set to Baseline");
                h264_profile = VAProfileH264Baseline;
                ip_period = 1;
                constraint_set_flag |= (1 << 0); /* Annex A.2.1 */
                break;
        }
    }

    /* find out the format for the render target, and rate control mode */
    for (i = 0; i < VAConfigAttribTypeMax; i++)
        attrib[i].type = i;

    va_status = vaGetConfigAttributes(va_dpy, h264_profile, VAEntrypointEncSlice,
                                      &attrib[0], VAConfigAttribTypeMax);
    CHECK_VASTATUS(va_status, "vaGetConfigAttributes");
    /* check the interested configattrib */
    if ((attrib[VAConfigAttribRTFormat].value & VA_RT_FORMAT_YUV420) == 0) {
        printf("Not find desired YUV420 RT format\n");
        exit(1);
    } else {
        config_attrib[config_attrib_num].type = VAConfigAttribRTFormat;
        config_attrib[config_attrib_num].value = VA_RT_FORMAT_YUV420;
        config_attrib_num++;
    }
    
    if (attrib[VAConfigAttribRateControl].value != VA_ATTRIB_NOT_SUPPORTED) {
        int tmp = attrib[VAConfigAttribRateControl].value;

        printf("Supported rate control mode (0x%x):", tmp);
        
        if (tmp & VA_RC_NONE)
            printf("NONE ");
        if (tmp & VA_RC_CBR)
            printf("CBR ");
        if (tmp & VA_RC_VBR)
            printf("VBR ");
        if (tmp & VA_RC_VCM)
            printf("VCM ");
        if (tmp & VA_RC_CQP)
            printf("CQP ");
        if (tmp & VA_RC_VBR_CONSTRAINED)
            printf("VBR_CONSTRAINED ");

        printf("\n");

        /* need to check if support rc_mode */
        config_attrib[config_attrib_num].type = VAConfigAttribRateControl;
        config_attrib[config_attrib_num].value = rc_mode;
        config_attrib_num++;
    }
    

    if (attrib[VAConfigAttribEncPackedHeaders].value != VA_ATTRIB_NOT_SUPPORTED) {
        int tmp = attrib[VAConfigAttribEncPackedHeaders].value;

        printf("Support VAConfigAttribEncPackedHeaders\n");
        
        h264_packedheader = 1;
        config_attrib[config_attrib_num].type = VAConfigAttribEncPackedHeaders;
        config_attrib[config_attrib_num].value = VA_ENC_PACKED_HEADER_NONE;
        
        if (tmp & VA_ENC_PACKED_HEADER_SEQUENCE) {
            printf("Support packed sequence headers\n");
            config_attrib[config_attrib_num].value |= VA_ENC_PACKED_HEADER_SEQUENCE;
        }
        
        if (tmp & VA_ENC_PACKED_HEADER_PICTURE) {
            printf("Support packed picture headers\n");
            config_attrib[config_attrib_num].value |= VA_ENC_PACKED_HEADER_PICTURE;
        }
        
        if (tmp & VA_ENC_PACKED_HEADER_SLICE) {
            printf("Support packed slice headers\n");
            config_attrib[config_attrib_num].value |= VA_ENC_PACKED_HEADER_SLICE;
        }
        
        if (tmp & VA_ENC_PACKED_HEADER_MISC) {
            printf("Support packed misc headers\n");
            config_attrib[config_attrib_num].value |= VA_ENC_PACKED_HEADER_MISC;
        }
        
        config_attrib_num++;
    }

    if (attrib[VAConfigAttribEncInterlaced].value != VA_ATTRIB_NOT_SUPPORTED) {
        int tmp = attrib[VAConfigAttribEncInterlaced].value;
        
        printf("Support VAConfigAttribEncInterlaced\n");

        if (tmp & VA_ENC_INTERLACED_FRAME)
            printf("support VA_ENC_INTERLACED_FRAME\n");
        if (tmp & VA_ENC_INTERLACED_FIELD)
            printf("Support VA_ENC_INTERLACED_FIELD\n");
        if (tmp & VA_ENC_INTERLACED_MBAFF)
            printf("Support VA_ENC_INTERLACED_MBAFF\n");
        if (tmp & VA_ENC_INTERLACED_PAFF)
            printf("Support VA_ENC_INTERLACED_PAFF\n");
        
        config_attrib[config_attrib_num].type = VAConfigAttribEncInterlaced;
        config_attrib[config_attrib_num].value = VA_ENC_PACKED_HEADER_NONE;
        config_attrib_num++;
    }
    
    if (attrib[VAConfigAttribEncMaxRefFrames].value != VA_ATTRIB_NOT_SUPPORTED) {
        h264_maxref = attrib[VAConfigAttribEncMaxRefFrames].value;
        
        printf("Support %d reference frames\n", h264_maxref);
    }

    if (attrib[VAConfigAttribEncMaxSlices].value != VA_ATTRIB_NOT_SUPPORTED)
        printf("Support %d slices\n", attrib[VAConfigAttribEncMaxSlices].value);

    if (attrib[VAConfigAttribEncSliceStructure].value != VA_ATTRIB_NOT_SUPPORTED) {
        int tmp = attrib[VAConfigAttribEncSliceStructure].value;
        
        printf("Support VAConfigAttribEncSliceStructure\n");

        if (tmp & VA_ENC_SLICE_STRUCTURE_ARBITRARY_ROWS)
            printf("Support VA_ENC_SLICE_STRUCTURE_ARBITRARY_ROWS\n");
        if (tmp & VA_ENC_SLICE_STRUCTURE_POWER_OF_TWO_ROWS)
            printf("Support VA_ENC_SLICE_STRUCTURE_POWER_OF_TWO_ROWS\n");
        if (tmp & VA_ENC_SLICE_STRUCTURE_ARBITRARY_MACROBLOCKS)
            printf("Support VA_ENC_SLICE_STRUCTURE_ARBITRARY_MACROBLOCKS\n");
    }
    if (attrib[VAConfigAttribEncMacroblockInfo].value != VA_ATTRIB_NOT_SUPPORTED) {
        printf("Support VAConfigAttribEncMacroblockInfo\n");
    }

    return 0;
}

static int setup_encode()
{
    VAStatus va_status;
    VASurfaceID *tmp_surfaceid;
    int codedbuf_size, i;
    
    va_status = vaCreateConfig(va_dpy, h264_profile, VAEntrypointEncSlice,
            &config_attrib[0], config_attrib_num, &config_id);
    CHECK_VASTATUS(va_status, "vaCreateConfig");

    /* create source surfaces */
    va_status = vaCreateSurfaces(va_dpy,
                                 VA_RT_FORMAT_YUV420, frame_width, frame_height,
                                 &src_surface[0], SRC_SURFACE_NUM,
                                 NULL, 0);
    CHECK_VASTATUS(va_status, "vaCreateSurfaces");

    /* create reference surfaces */
    va_status = vaCreateSurfaces(
            va_dpy,
            VA_RT_FORMAT_YUV420, frame_width, frame_height,
            &ref_surface[0], h264_maxref,
            NULL, 0
            );
    CHECK_VASTATUS(va_status, "vaCreateSurfaces");

    tmp_surfaceid = calloc(SRC_SURFACE_NUM + h264_maxref, sizeof(VASurfaceID));
    memcpy(tmp_surfaceid, src_surface, SRC_SURFACE_NUM * sizeof(VASurfaceID));
    memcpy(tmp_surfaceid + SRC_SURFACE_NUM, ref_surface, h264_maxref * sizeof(VASurfaceID));
    
    /* Create a context for this encode pipe */
    va_status = vaCreateContext(va_dpy, config_id,
                                frame_width, frame_height,
                                VA_PROGRESSIVE,
                                tmp_surfaceid, SRC_SURFACE_NUM + h264_maxref,
                                &context_id);
    CHECK_VASTATUS(va_status, "vaCreateContext");
    free(tmp_surfaceid);

    codedbuf_size = (frame_width * frame_height * 400) / (16*16);

    for (i = 0; i < SRC_SURFACE_NUM; i++) {
        /* create coded buffer once for all
         * other VA buffers which won't be used again after vaRenderPicture.
         * so APP can always vaCreateBuffer for every frame
         * but coded buffer need to be mapped and accessed after vaRenderPicture/vaEndPicture
         * so VA won't maintain the coded buffer
         */
        va_status = vaCreateBuffer(va_dpy,context_id,VAEncCodedBufferType,
                codedbuf_size, 1, NULL, &coded_buf[i]);
        CHECK_VASTATUS(va_status,"vaCreateBuffer");
    }
    
    return 0;
}

static int render_sequence(void)
{
    VABufferID seq_param_buf, rc_param_buf;
    VAStatus va_status;
    VAEncMiscParameterBuffer *misc_param;
    VAEncMiscParameterRateControl *misc_rate_ctrl;
    
    seq_param.level_idc = 41 /*SH_LEVEL_3*/;
    seq_param.picture_width_in_mbs = frame_width / 16;
    seq_param.picture_height_in_mbs = frame_height / 16;
    seq_param.bits_per_second = frame_bitrate;

    seq_param.intra_period = intra_period;
    seq_param.intra_idr_period = intra_idr_period;
    seq_param.ip_period = ip_period;

    seq_param.max_num_ref_frames = h264_maxref;
    seq_param.seq_fields.bits.frame_mbs_only_flag = 1;
    seq_param.time_scale = 900;
    seq_param.num_units_in_tick = 15; /* Tc = num_units_in_tick / time_sacle */
    seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 2;
    seq_param.seq_fields.bits.frame_mbs_only_flag = 1;
    
    va_status = vaCreateBuffer(va_dpy, context_id,
                               VAEncSequenceParameterBufferType,
                               sizeof(seq_param),1,&seq_param,&seq_param_buf);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");
    
    va_status = vaCreateBuffer(va_dpy, context_id,
                               VAEncMiscParameterBufferType,
                               sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl),
                               1,NULL,&rc_param_buf);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");
    
    vaMapBuffer(va_dpy, rc_param_buf,(void **)&misc_param);
    misc_param->type = VAEncMiscParameterTypeRateControl;
    misc_rate_ctrl = (VAEncMiscParameterRateControl *)misc_param->data;
    memset(misc_rate_ctrl, 0, sizeof(*misc_rate_ctrl));
    misc_rate_ctrl->bits_per_second = frame_bitrate;
    misc_rate_ctrl->initial_qp = initial_qp;
    misc_rate_ctrl->min_qp = minimal_qp;
    misc_rate_ctrl->basic_unit_size = 0;
    vaUnmapBuffer(va_dpy, rc_param_buf);

    va_status = vaRenderPicture(va_dpy,context_id, &seq_param_buf, 1);
    CHECK_VASTATUS(va_status,"vaRenderPicture");;

    va_status = vaRenderPicture(va_dpy,context_id, &rc_param_buf, 1);
    CHECK_VASTATUS(va_status,"vaRenderPicture");;

    return 0;
}

static int render_picture(void)
{
    VABufferID pic_param_buf;
    VAStatus va_status;
    int i = 0;

    /* use frame_num as reference frame index */
    pic_param.CurrPic.picture_id = ref_surface[current_frame_num % h264_maxref];
//    pic_param.CurrPic.frame_idx = current_frame_num % h264_maxref;
    pic_param.CurrPic.flags = 0;
    pic_param.CurrPic.TopFieldOrderCnt = 2 * current_frame_display;
    pic_param.CurrPic.BottomFieldOrderCnt = 0;
    if (current_frame_type != FRAME_B)
        CurrentCurrPic = pic_param.CurrPic; /* save it */
    
    if (slice_refoverride) {
        /* always setup all reference frame into encoder */
        for (i = 0; i < h264_maxref; i++) {
            pic_param.ReferenceFrames[i].picture_id = ref_surface[i];
            pic_param.ReferenceFrames[i].frame_idx = i;
            pic_param.ReferenceFrames[i].flags = 0;
            if (pic_param.CurrPic.picture_id == pic_param.ReferenceFrames[i].picture_id) {
                pic_param.ReferenceFrames[i].TopFieldOrderCnt = 2 * current_frame_encoding;
                //pic_param.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
                //pic_param.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;            
            }
            pic_param.ReferenceFrames[i].BottomFieldOrderCnt = 0;
        }
    } else {
        if (current_frame_type == FRAME_I || current_frame_type == FRAME_IDR)
            i = 0;
        else if (current_frame_type == FRAME_P) {
            pic_param.ReferenceFrames[0].picture_id = LastCurrPic.picture_id;
            i = 1;
        } else if (current_frame_type == FRAME_B) {
            pic_param.ReferenceFrames[0] = LLastCurrPic;
            pic_param.ReferenceFrames[1] = LastCurrPic;
            i = 2;
        }
    }
    
    for (; i < REF_SURFACE_NUM; i++) {
        pic_param.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        pic_param.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    
    pic_param.pic_fields.bits.idr_pic_flag = (current_frame_type == FRAME_IDR);
    pic_param.pic_fields.bits.reference_pic_flag = (current_frame_type != FRAME_B);
    pic_param.pic_fields.bits.entropy_coding_mode_flag = 1;
    pic_param.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    pic_param.frame_num = current_frame_num;
    if (current_frame_type != FRAME_B)
        current_frame_num++;
    pic_param.coded_buf = coded_buf[current_frame_display % SRC_SURFACE_NUM];
    pic_param.last_picture = (current_frame_encoding == frame_count);
    pic_param.pic_init_qp = initial_qp;

    va_status = vaCreateBuffer(va_dpy, context_id,VAEncPictureParameterBufferType,
                               sizeof(pic_param),1,&pic_param, &pic_param_buf);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");;

    va_status = vaRenderPicture(va_dpy,context_id, &pic_param_buf, 1);
    CHECK_VASTATUS(va_status,"vaRenderPicture");

    return 0;
}

static int render_packedsequence(void)
{
    VAEncPackedHeaderParameterBuffer packedheader_param_buffer={0};
    VABufferID packedseq_para_bufid, packedseq_data_bufid, render_id[2];
    unsigned int length_in_bits;
    unsigned char *packedseq_buffer = NULL;
    VAStatus va_status;

    length_in_bits = build_packed_seq_buffer(&packedseq_buffer); 
    
    packedheader_param_buffer.type = VAEncPackedHeaderSequence;
    
    packedheader_param_buffer.bit_length = length_in_bits; /*length_in_bits*/
    packedheader_param_buffer.has_emulation_bytes = 0;
    va_status = vaCreateBuffer(va_dpy,
                               context_id,
                               VAEncPackedHeaderParameterBufferType,
                               sizeof(packedheader_param_buffer), 1, &packedheader_param_buffer,
                               &packedseq_para_bufid);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    va_status = vaCreateBuffer(va_dpy,
                               context_id,
                               VAEncPackedHeaderDataBufferType,
                               (length_in_bits + 7) / 8, 1, packedseq_buffer,
                               &packedseq_data_bufid);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    render_id[0] = packedseq_para_bufid;
    render_id[1] = packedseq_data_bufid;
    va_status = vaRenderPicture(va_dpy,context_id, render_id, 2);
    CHECK_VASTATUS(va_status,"vaRenderPicture");
    
    return 0;
}


static int render_packedpicture(void)
{
    VAEncPackedHeaderParameterBuffer packedheader_param_buffer={0};
    VABufferID packedpic_para_bufid, packedpic_data_bufid, render_id[2];
    unsigned int length_in_bits;
    unsigned char *packedpic_buffer = NULL;
    VAStatus va_status;

    length_in_bits = build_packed_pic_buffer(&packedpic_buffer); 
    packedheader_param_buffer.type = VAEncPackedHeaderPicture;
    packedheader_param_buffer.bit_length = length_in_bits;
    packedheader_param_buffer.has_emulation_bytes = 0;

    va_status = vaCreateBuffer(va_dpy,
                               context_id,
                               VAEncPackedHeaderParameterBufferType,
                               sizeof(packedheader_param_buffer), 1, &packedheader_param_buffer,
                               &packedpic_para_bufid);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    va_status = vaCreateBuffer(va_dpy,
                               context_id,
                               VAEncPackedHeaderDataBufferType,
                               (length_in_bits + 7) / 8, 1, packedpic_buffer,
                               &packedpic_data_bufid);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    render_id[0] = packedpic_para_bufid;
    render_id[1] = packedpic_data_bufid;
    va_status = vaRenderPicture(va_dpy,context_id, render_id, 2);
    CHECK_VASTATUS(va_status,"vaRenderPicture");
    
    return 0;
}

static int render_slice(void)
{
    VABufferID slice_param_buf;
    VAStatus va_status;
    int i;
    
    /* one frame, one slice */
    slice_param.macroblock_address = 0;
    slice_param.num_macroblocks = frame_width*frame_height/(16*16); /* Measured by MB */
    slice_param.slice_type = (current_frame_type == FRAME_IDR)?2:current_frame_type;

    for (i = 0; i < 32; i++) {
        slice_param.RefPicList0[i].picture_id = VA_INVALID_SURFACE;
        slice_param.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
        slice_param.RefPicList1[i].picture_id = VA_INVALID_SURFACE;
        slice_param.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
    }

    /* cause issue on some implementation if slice_refoverride = 1 */
    if (slice_refoverride) {
        /* set the real reference frame */
        slice_param.num_ref_idx_active_override_flag = 1;
        if (current_frame_type == FRAME_I || current_frame_type == FRAME_IDR) {
            slice_param.num_ref_idx_l0_active_minus1 = 0;
            slice_param.num_ref_idx_l1_active_minus1 = 0;
        } else if (current_frame_type == FRAME_P) {
            slice_param.num_ref_idx_l0_active_minus1 = 0;
            slice_param.num_ref_idx_l1_active_minus1 = 0;
            slice_param.RefPicList0[0] = LastCurrPic;
        } else if (current_frame_type == FRAME_B) {
            slice_param.num_ref_idx_l0_active_minus1 = 0;
            slice_param.num_ref_idx_l1_active_minus1 = 0;
            slice_param.RefPicList0[0] = LLastCurrPic;
            slice_param.RefPicList1[0] = LastCurrPic;
        }
    }
    slice_param.slice_alpha_c0_offset_div2 = 2;
    slice_param.slice_beta_offset_div2 = 2;
    slice_param.pic_order_cnt_lsb = current_frame_display % 64;
    
    va_status = vaCreateBuffer(va_dpy,context_id,VAEncSliceParameterBufferType,
                               sizeof(slice_param),1,&slice_param,&slice_param_buf);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");;

    va_status = vaRenderPicture(va_dpy,context_id, &slice_param_buf, 1);
    CHECK_VASTATUS(va_status,"vaRenderPicture");

    return 0;
}


static int update_reflist(void)
{
    if (current_frame_type == FRAME_B)
        return 0;

    LLastCurrPic = LastCurrPic;
    LastCurrPic = CurrentCurrPic;

    return 0;
}


static int upload_source_YUV_once_for_all()
{
    int box_width=8;
    int row_shift=0;
    int i;

    for (i = 0; i < SRC_SURFACE_NUM; i++) {
        printf("\rLoading data into surface %d.....", i);
        upload_surface(va_dpy, src_surface[i], box_width, row_shift, 0);

        row_shift++;
        if (row_shift==(2*box_width)) row_shift= 0;
    }
    printf("Completed surface loading\n");

    return 0;
}


static int load_surface(VASurfaceID surface_id, unsigned long long display_order)
{
    VAImage surface_image;
    unsigned char *surface_p, *Y_start, *U_start,*V_start;
    int Y_pitch, U_pitch, row, V_pitch;
    VAStatus va_status;

    if (srcyuv_fp == NULL)
        return 0;
    
    /* rewind the file pointer if encoding more than srcyuv_frames */
    display_order = display_order % srcyuv_frames;
    
    fseek(srcyuv_fp, display_order * frame_width * frame_height * 1.5, SEEK_SET);
    
    va_status = vaDeriveImage(va_dpy,surface_id, &surface_image);
    CHECK_VASTATUS(va_status,"vaDeriveImage");

    vaMapBuffer(va_dpy,surface_image.buf,(void **)&surface_p);
    assert(VA_STATUS_SUCCESS == va_status);

    Y_start = surface_p;
    Y_pitch = surface_image.pitches[0];
    switch (surface_image.format.fourcc) {
    case VA_FOURCC_NV12:
        U_start = (unsigned char *)surface_p + surface_image.offsets[1];
        V_start = U_start + 1;
        U_pitch = surface_image.pitches[1];
        V_pitch = surface_image.pitches[1];
        break;
    case VA_FOURCC_IYUV:
        U_start = (unsigned char *)surface_p + surface_image.offsets[1];
        V_start = (unsigned char *)surface_p + surface_image.offsets[2];
        U_pitch = surface_image.pitches[1];
        V_pitch = surface_image.pitches[2];
        break;
    case VA_FOURCC_YV12:
        U_start = (unsigned char *)surface_p + surface_image.offsets[2];
        V_start = (unsigned char *)surface_p + surface_image.offsets[1];
        U_pitch = surface_image.pitches[2];
        V_pitch = surface_image.pitches[1];
        break;
    case VA_FOURCC_YUY2:
        U_start = surface_p + 1;
        V_start = surface_p + 3;
        U_pitch = surface_image.pitches[0];
        V_pitch = surface_image.pitches[0];
        break;
    default:
        assert(0);
    }

    /* copy Y plane */
    for (row=0;row<surface_image.height;row++) {
        unsigned char *Y_row = Y_start + row * Y_pitch;

        fread(Y_row, 1, surface_image.width, srcyuv_fp);
    }
  
    /* copy UV data, reset file pointer,
     * surface_image.height may not be equal to source YUV height/frame_height
     */
    fseek(srcyuv_fp,
          display_order * frame_width * frame_height * 1.5 + frame_width * frame_height,
          SEEK_SET);
    
    for (row =0; row < surface_image.height/2; row++) {
        unsigned char *U_row = U_start + row * U_pitch;
        //unsigned char *V_row = V_start + row * V_pitch;
        switch (surface_image.format.fourcc) {
        case VA_FOURCC_NV12:
            if (srcyuv_fourcc == VA_FOURCC_NV12)
                fread(U_row, 1, surface_image.width, srcyuv_fp);
            else if (srcyuv_fourcc == VA_FOURCC_IYUV) {
                /* tbd */
            }
            break;
        case VA_FOURCC_YV12:
            /* tbd */
            break;
        case VA_FOURCC_YUY2:
            // see above. it is set with Y update.
            break;
        default:
            printf("unsupported fourcc in load_surface\n");
            assert(0);
        }
    }
        
    vaUnmapBuffer(va_dpy,surface_image.buf);

    vaDestroyImage(va_dpy,surface_image.image_id);

    return 0;
}


static int save_codeddata(unsigned long long display_order, unsigned long long encode_order)
{    
    VACodedBufferSegment *buf_list = NULL;
    VAStatus va_status;
    unsigned int coded_size = 0;

    va_status = vaMapBuffer(va_dpy,coded_buf[display_order % SRC_SURFACE_NUM],(void **)(&buf_list));
    CHECK_VASTATUS(va_status,"vaMapBuffer");
    while (buf_list != NULL) {
        coded_size += fwrite(buf_list->buf, 1, buf_list->size, coded_fp);
        buf_list = (VACodedBufferSegment *) buf_list->next;
    }
    vaUnmapBuffer(va_dpy,coded_buf[display_order % SRC_SURFACE_NUM]);

    printf("\r      "); /* return back to startpoint */
    switch (encode_order % 4) {
        case 0:
            printf("|");
            break;
        case 1:
            printf("/");
            break;
        case 2:
            printf("-");
            break;
        case 3:
            printf("\\");
            break;
    }
    printf("%08lld", encode_order);
    /*
    if (current_frame_encoding % intra_count == 0)
        printf("(I)");
    else
        printf("(P)");
    */
    printf("(%06d bytes coded)",coded_size);
    /* skipped frame ? */
    printf("                                    ");

    return 0;
}


static struct storage_task_t * storage_task_dequque(void)
{
    struct storage_task_t *header;

    pthread_mutex_lock(&encode_mutex);

    header = storage_task_header;    
    if (storage_task_header != NULL) {
        if (storage_task_tail == storage_task_header)
            storage_task_tail = NULL;
        storage_task_header = header->next;
    }
    
    pthread_mutex_unlock(&encode_mutex);
    
    return header;
}

static int storage_task_queue(unsigned long long display_order, unsigned long long encode_order)
{
    struct storage_task_t *tmp;

    tmp = calloc(1, sizeof(struct storage_task_t));
    tmp->display_order = display_order;
    tmp->encode_order = encode_order;

    pthread_mutex_lock(&encode_mutex);
    
    if (storage_task_header == NULL) {
        storage_task_header = tmp;
        storage_task_tail = tmp;
    } else {
        storage_task_tail->next = tmp;
        storage_task_tail = tmp;
    }

    srcsurface_status[display_order % SRC_SURFACE_NUM] = SRC_SURFACE_IN_STORAGE;
    pthread_cond_signal(&encode_cond);
    
    pthread_mutex_unlock(&encode_mutex);
    
    return 0;
}

static void storage_task(unsigned long long display_order, unsigned long encode_order)
{
    unsigned int tmp;
    VAStatus va_status;
    
    tmp = GetTickCount();
    va_status = vaSyncSurface(va_dpy, src_surface[display_order % SRC_SURFACE_NUM]);
    CHECK_VASTATUS(va_status,"vaSyncSurface");
    SyncPictureTicks += GetTickCount() - tmp;
    tmp = GetTickCount();
    save_codeddata(display_order, encode_order);
    SavePictureTicks += GetTickCount() - tmp;
    /* tbd: save reconstructed frame */
        
    /* reload a new frame data */
    tmp = GetTickCount();
    if (srcyuv_fp != NULL)
        load_surface(src_surface[display_order % SRC_SURFACE_NUM], display_order);
    UploadPictureTicks += GetTickCount() - tmp;

    pthread_mutex_lock(&encode_mutex);
    srcsurface_status[display_order % SRC_SURFACE_NUM] = SRC_SURFACE_IN_ENCODING;
    pthread_mutex_unlock(&encode_mutex);
}

        
static void * storage_task_thread(void *t)
{
    while (1) {
        struct storage_task_t *current;
        
        current = storage_task_dequque();
        if (current == NULL) {
            pthread_mutex_lock(&encode_mutex);
            pthread_cond_wait(&encode_cond, &encode_mutex);
            pthread_mutex_unlock(&encode_mutex);
            continue;
        }
        
        storage_task(current->display_order, current->encode_order);
        
        free(current);

        /* all frames are saved, exit the thread */
        if (++frame_coded >= frame_count)
            break;
    }

    return 0;
}


static int encode_frames(void)
{
    unsigned int i, tmp;
    VAStatus va_status;
    //VASurfaceStatus surface_status;

    /* upload RAW YUV data into all surfaces */
    tmp = GetTickCount();
    if (srcyuv_fp != NULL) {
        for (i = 0; i < SRC_SURFACE_NUM; i++)
            load_surface(src_surface[i], i);
    } else
        upload_source_YUV_once_for_all();
    UploadPictureTicks += GetTickCount() - tmp;
    
    /* ready for encoding */
    memset(srcsurface_status, SRC_SURFACE_IN_ENCODING, sizeof(srcsurface_status));
    
    memset(&seq_param, 0, sizeof(seq_param));
    memset(&pic_param, 0, sizeof(pic_param));
    memset(&slice_param, 0, sizeof(slice_param));

    if (encode_syncmode == 0)
        pthread_create(&encode_thread, NULL, storage_task_thread, NULL);
    
    for (current_frame_encoding = 0; current_frame_encoding < frame_count; current_frame_encoding++) {
        encoding2display_order(current_frame_encoding, intra_period, intra_idr_period, ip_period,
                               &current_frame_display, &current_frame_type);

        /* check if the source frame is ready */
        while (srcsurface_status[current_frame_display % SRC_SURFACE_NUM] != SRC_SURFACE_IN_ENCODING);

        tmp = GetTickCount();
        va_status = vaBeginPicture(va_dpy, context_id, src_surface[current_frame_display % SRC_SURFACE_NUM]);
        CHECK_VASTATUS(va_status,"vaBeginPicture");
        BeginPictureTicks += GetTickCount() - tmp;
        
        tmp = GetTickCount();
        if (current_frame_encoding  == 0) {
            render_sequence();
            render_picture();            
            if (h264_packedheader) {
                render_packedsequence();
                render_packedpicture();
            }
        } else {
            //render_sequence();
            render_picture();
        }
        render_slice();
        RenderPictureTicks += GetTickCount() - tmp;
        
        tmp = GetTickCount();
        va_status = vaEndPicture(va_dpy,context_id);
        CHECK_VASTATUS(va_status,"vaEndPicture");;
        EndPictureTicks += GetTickCount() - tmp;

        if (encode_syncmode)
            storage_task(current_frame_display, current_frame_encoding);
        else /* queue the storage task queue */
            storage_task_queue(current_frame_display, current_frame_encoding);

        /* how to process skipped frames
           surface_status = (VASurfaceStatus) 0;
           va_status = vaQuerySurfaceStatus(va_dpy, src_surface[i%SRC_SURFACE_NUM],&surface_status);
           frame_skipped = (surface_status & VASurfaceSkipped);
        */

        update_reflist();        
    }

    if (encode_syncmode == 0) {
        int ret;
        pthread_join(encode_thread, (void **)&ret);
    }
    
    return 0;
}


static int release_encode()
{
    int i;
    
    vaDestroySurfaces(va_dpy,&src_surface[0],SRC_SURFACE_NUM);
    vaDestroySurfaces(va_dpy,&ref_surface[0],h264_maxref);

    for (i = 0; i < SRC_SURFACE_NUM; i++)
        vaDestroyBuffer(va_dpy,coded_buf[i]);
    
    vaDestroyContext(va_dpy,context_id);
    vaDestroyConfig(va_dpy,config_id);

    return 0;
}

static int deinit_va()
{ 
    vaTerminate(va_dpy);

    va_close_display(va_dpy);

    return 0;
}


static int print_input()
{
    printf("\n\nINPUT:Try to encode H264...\n");
    printf("INPUT: RateControl  : %s\n", rc_to_string(rc_mode));
    printf("INPUT: Resolution   : %dx%d, %d frames\n",
           frame_width, frame_height, frame_count);
    printf("INPUT: FrameRate    : %d\n", frame_rate);
    printf("INPUT: Bitrate      : %d\n", frame_bitrate);
    printf("INPUT: Slieces      : %d\n", frame_slices);
    printf("INPUT: IntraPeriod  : %d\n", intra_period);
    printf("INPUT: IDRPeriod    : %d\n", intra_idr_period);
    printf("INPUT: IpPeriod     : %d\n", ip_period);
    printf("INPUT: Initial QP   : %d\n", initial_qp);
    printf("INPUT: Min QP       : %d\n", minimal_qp);
    printf("INPUT: Source YUV   : %s", srcyuv_fn?"FILE":"AUTO generated");
    if (srcyuv_fp) 
        printf(":%s (fourcc %s)\n", srcyuv_fn, fourcc_to_string(srcyuv_fourcc));
    else
        printf("\n");
    printf("INPUT: Coded Clip   : %s\n", coded_fn);
    if (srcyuv_fp == NULL)
        printf("INPUT: Rec   Clip   : %s\n", "Not save reconstructed frame");
    else
        printf("INPUT: Rec   Clip   : Save reconstructed frame into %s (fourcc %s)\n", recyuv_fn,
               fourcc_to_string(srcyuv_fourcc));
    
    printf("\n\n"); /* return back to startpoint */
    
    return 0;
}


static int print_performance(unsigned int PictureCount)
{
    unsigned int others = 0;

    others = TotalTicks - UploadPictureTicks - BeginPictureTicks
        - RenderPictureTicks - EndPictureTicks - SyncPictureTicks - SavePictureTicks;
    
    printf("\n\n");

    printf("PERFORMANCE:   Frame Rate           : %.2f fps (%d frames, %d ms (%.2f ms per frame))\n",
           (double) 1000*PictureCount / TotalTicks, PictureCount,
           TotalTicks, ((double)  TotalTicks) / (double) PictureCount);

    printf("PERFORMANCE:     UploadPicture      : %d ms (%.2f, %.2f%% percent)\n",
           (int) UploadPictureTicks, ((double)  UploadPictureTicks) / (double) PictureCount,
           UploadPictureTicks/(double) TotalTicks/0.01);
    printf("PERFORMANCE:     vaBeginPicture     : %d ms (%.2f, %.2f%% percent)\n",
           (int) BeginPictureTicks, ((double)  BeginPictureTicks) / (double) PictureCount,
           BeginPictureTicks/(double) TotalTicks/0.01);
    printf("PERFORMANCE:     vaRenderHeader     : %d ms (%.2f, %.2f%% percent)\n",
           (int) RenderPictureTicks, ((double)  RenderPictureTicks) / (double) PictureCount,
           RenderPictureTicks/(double) TotalTicks/0.01);
    printf("PERFORMANCE:     vaEndPicture       : %d ms (%.2f, %.2f%% percent)\n",
           (int) EndPictureTicks, ((double)  EndPictureTicks) / (double) PictureCount,
           EndPictureTicks/(double) TotalTicks/0.01);
    printf("PERFORMANCE:     vaSyncSurface      : %d ms (%.2f, %.2f%% percent)\n",
           (int) SyncPictureTicks, ((double) SyncPictureTicks) / (double) PictureCount,
           SyncPictureTicks/(double) TotalTicks/0.01);
    printf("PERFORMANCE:     SavePicture        : %d ms (%.2f, %.2f%% percent)\n",
           (int) SavePictureTicks, ((double)  SavePictureTicks) / (double) PictureCount,
           SavePictureTicks/(double) TotalTicks/0.01);

    printf("PERFORMANCE:     Others             : %d ms (%.2f, %.2f%% percent)\n",
           (int) others, ((double) others) / (double) PictureCount,
           others/(double) TotalTicks/0.01);
    
    return 0;
}


int main(int argc,char **argv)
{
    unsigned int start;
    
    process_cmdline(argc, argv);

    print_input();
    
    start = GetTickCount();
    
    init_va();
    setup_encode();
    
    encode_frames();

    release_encode();
    deinit_va();

    TotalTicks += GetTickCount() - start;
    print_performance(frame_count);
    
    return 0;
}