/*
 * webp_vp8.h - types shared by the two halves of the lossy VP8 decoder.
 * Internal use only; not installed.
 *
 * The decoder is deliberately restricted to key frames: a WebP still image
 * carries exactly one VP8 frame and that frame has no reference to predict
 * from, so it is always a key frame. Everything that only exists for
 * interframes - motion vectors, reference frame management, sub pixel
 * interpolation - is therefore unreachable and is not implemented.
 */
#ifndef WEBP_VP8_H
#define WEBP_VP8_H

#include "webp_internal.h"

/* Frame header sizes, in bytes: the three byte frame tag plus, for a key
 * frame, the three byte start code and the two dimension words. */
#define VP8_FRAME_HEADER_SZ       3
#define VP8_KEYFRAME_HEADER_SZ    7

#define VP8_MAX_PARTITIONS        8
#define VP8_MAX_MB_SEGMENTS       4
#define VP8_BLOCK_TYPES           4
#define VP8_COEFF_BANDS           8
#define VP8_PREV_CONTEXTS         3
#define VP8_ENTROPY_NODES        11
#define VP8_BLOCK_CONTEXTS        4

/* Number of 4x4 residue blocks in a macroblock: sixteen luma, four U, four V
 * and the second order block that carries the luma DC values. */
#define VP8_NUM_COEFF_BLOCKS     25

/* Out-of-frame prediction pixels are materialised in a border around each
 * plane instead of being special cased per macroblock. Sixteen is what the
 * reference decoder uses; four would already be enough for the above-right
 * pixels of the 4x4 sub block modes. */
#define VP8_BORDER               16

/* 16x16 luma prediction modes. The inter modes of the bitstream guide would
 * follow B_PRED here but cannot occur in a key frame. */
enum {
    VP8_DC_PRED, VP8_V_PRED, VP8_H_PRED, VP8_TM_PRED, VP8_B_PRED
};

/* 4x4 luma sub block modes, used when the macroblock mode is VP8_B_PRED. */
enum {
    VP8_B_DC_PRED, VP8_B_TM_PRED, VP8_B_VE_PRED, VP8_B_HE_PRED,
    VP8_B_LD_PRED, VP8_B_RD_PRED, VP8_B_VR_PRED, VP8_B_VL_PRED,
    VP8_B_HD_PRED, VP8_B_HU_PRED
};

/* Dequantization factor slots, indexed by the kind of block being decoded. */
enum {
    VP8_TOKEN_Y1, VP8_TOKEN_UV, VP8_TOKEN_Y2, VP8_TOKEN_BLOCK_TYPES
};

/* Boolean (binary arithmetic) decoder of the VP8 bitstream guide. Bits are
 * consumed most significant bit first, eight at a time. */
typedef struct {
    const uint8_t* input;
    uint32_t       input_len;
    uint32_t       range;
    uint32_t       value;
    int            bit_count;
} WebpBoolDec;

/* One image plane plus the border that holds the out-of-frame pixels. */
typedef struct {
    uint8_t* base;     /* the allocation, kept for free() */
    uint8_t* pixels;   /* the first visible pixel */
    int      stride;
} WebpPlane;

/* Per macroblock side information. Only what the key frame path produces is
 * kept; a reference frame index would always be zero here. */
typedef struct {
    uint8_t  y_mode;
    uint8_t  uv_mode;
    uint8_t  segment_id;
    uint8_t  skip_coeff;
    uint8_t  b_modes[16];
    uint32_t eob_mask;   /* non-zero when the macroblock carries residue */
} WebpMbInfo;

typedef struct {
    /* frame geometry; width/height are the cropped picture size while the
     * planes and the macroblock grid are rounded up to whole macroblocks */
    int width, height;
    int mb_cols, mb_rows;

    int part0_size;
    WebpBoolDec part0;
    WebpBoolDec tokens[VP8_MAX_PARTITIONS];
    int num_partitions;

    /* segmentation */
    int seg_enabled;
    int seg_update_map;
    int seg_abs;
    int seg_tree_probs[3];
    int seg_lf_level[VP8_MAX_MB_SEGMENTS];
    int seg_quant_idx[VP8_MAX_MB_SEGMENTS];

    /* loop filter */
    int lf_use_simple;
    int lf_level;
    int lf_sharpness;
    int lf_delta_enabled;
    int lf_ref_delta[VP8_BLOCK_CONTEXTS];
    int lf_mode_delta[VP8_BLOCK_CONTEXTS];

    /* quantizer */
    int q_index;
    int y1_dc_delta_q, y2_dc_delta_q, y2_ac_delta_q;
    int uv_dc_delta_q, uv_ac_delta_q;

    /* coefficient probabilities, updated in place from the frame header */
    uint8_t coeff_probs[VP8_BLOCK_TYPES][VP8_COEFF_BANDS]
                       [VP8_PREV_CONTEXTS][VP8_ENTROPY_NODES];
    int     coeff_skip_enabled;
    uint8_t coeff_skip_prob;

    /* [segment][Y1/UV/Y2][DC/AC] */
    int16_t factor[VP8_MAX_MB_SEGMENTS][VP8_TOKEN_BLOCK_TYPES][2];

    WebpPlane y, u, v;
} WebpVp8Dec;

/* webp_vp8_recon.c */

/* Reconstructs one macroblock row: builds the intra predictors, adds the
 * residue to them and extends the right border, which the next row needs for
 * its above-right pixels. mbs is indexed by macroblock column and coeffs
 * holds VP8_NUM_COEFF_BLOCKS blocks of sixteen coefficients per macroblock.
 * coeffs is not const: the inverse Walsh transform of a macroblock's luma DC
 * values is written back as coefficient zero of its sixteen luma blocks. */
void webp_vp8_predict_row(const WebpVp8Dec* dec, int row,
                          const WebpMbInfo* mbs, int16_t* coeffs);

/* Runs the loop filter over an already reconstructed macroblock row. It must
 * lag one row behind the prediction, because intra prediction has to see the
 * unfiltered pixels of the row above. */
void webp_vp8_filter_row(const WebpVp8Dec* dec, int row, const WebpMbInfo* mbs);

/* Converts the cropped YUV 4:2:0 planes into ARGB. */
void webp_vp8_to_argb(const WebpVp8Dec* dec, uint32_t* dst);

#endif /* WEBP_VP8_H */
