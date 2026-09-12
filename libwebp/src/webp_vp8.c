/*
 * webp_vp8.c - the parse half of the lossy VP8 decoder: the boolean
 * decoder, the frame header, macroblock modes and residue tokens.
 *
 * An arithmetic coder leaves no room for reinterpreting the syntax, so the
 * header and token readers below follow the reference decoder of RFC 6386
 * Section 20 (dixie.c, modemv.c and tokens.c) statement for statement. The
 * reconstruction half lives in webp_vp8_recon.c.
 *
 * Only key frames are handled. A WebP still image is a single VP8 frame with
 * nothing to predict from, so it is always a key frame and the
 * interframe-only syntax - motion vectors, reference frame selection, sub
 * pixel interpolation - is unreachable.
 */
#include "webp_internal.h"
#include "webp_vp8.h"
#include "webp_vp8_tables.h"

/* The residue of one macroblock: 25 blocks of 16 coefficients. */
#define VP8_MB_COEFFS    (VP8_NUM_COEFF_BLOCKS * 16)

/* Entropy context slots per macroblock column: eight block groups plus the
 * second order block. */
#define VP8_CTX_SLOTS    9

/* Byte offset of one probability band inside a [context][node] plane. */
#define VP8_BAND_OFF(b)  ((int)(b) * (VP8_PREV_CONTEXTS * VP8_ENTROPY_NODES))

/* The fixed part of a key frame header: frame tag, start code, dimensions. */
#define VP8_FIXED_HEADER (VP8_FRAME_HEADER_SZ + VP8_KEYFRAME_HEADER_SZ)

/*---------------------------------------------------------------------------
 * Boolean decoder (RFC 6386 Section 7)
 *-------------------------------------------------------------------------*/

static void bool_init(WebpBoolDec* br, const uint8_t* data, uint32_t size) {
    if (size >= 2) {
        br->value = ((uint32_t)data[0] << 8) | data[1];
        br->input = data + 2;
        br->input_len = size - 2;
    } else {
        br->value = 0;
        br->input = NULL;
        br->input_len = 0;
    }
    br->range = 255;       /* the initial range is the full one */
    br->bit_count = 0;
}

/* Reads one binary symbol. `probability` is the chance of reading a zero,
 * scaled to 0..255 exactly as the encoder scaled it. */
static int bool_get(WebpBoolDec* br, int probability) {
    const uint32_t split =
        1u + (((br->range - 1u) * (uint32_t)probability) >> 8);
    const uint32_t point = split << 8;
    int retval;

    if (br->value >= point) {        /* the encoder wrote a one */
        retval = 1;
        br->range -= split;
        br->value -= point;          /* drop the left endpoint */
    } else {                         /* the encoder wrote a zero */
        retval = 0;
        br->range = split;
    }

    while (br->range < 128u) {       /* renormalise */
        br->value <<= 1;
        br->range <<= 1;
        if (++br->bit_count == 8) {  /* refill eight bits at a time */
            br->bit_count = 0;
            if (br->input_len != 0) {
                br->value |= *br->input++;
                br->input_len--;
            }
            /* an exhausted partition keeps shifting in zeros, which is what
             * the guide prescribes for a truncated tail */
        }
    }
    return retval;
}

static int bool_get_bit(WebpBoolDec* br) {
    return bool_get(br, 128);
}

/* An unsigned magnitude, most significant bit first. */
static int bool_get_uint(WebpBoolDec* br, int bits) {
    int z = 0;
    int bit;

    for (bit = bits - 1; bit >= 0; --bit)
        z |= bool_get_bit(br) << bit;
    return z;
}

/* A magnitude followed by a sign bit. */
static int bool_get_int(WebpBoolDec* br, int bits) {
    const int magnitude = bool_get_uint(br, bits);

    return bool_get_bit(br) ? -magnitude : magnitude;
}

static int bool_maybe_get_int(WebpBoolDec* br, int bits) {
    return bool_get_bit(br) ? bool_get_int(br, bits) : 0;
}

/* Walks a decoding tree. A negative entry is the negated symbol and a
 * non-negative one the index of the subtree to descend into; probability i
 * belongs to the node at tree index 2*i. */
static int bool_read_tree(WebpBoolDec* br, const int8_t* tree,
                          const uint8_t* probs) {
    int i = 0;

    while ((i = tree[i + bool_get(br, probs[i >> 1])]) > 0)
        ;
    return -i;
}

/* Same walk, but starting at an arbitrary node: the coefficient tree is
 * entered below its end-of-block leaf right after a zero coefficient. */
static int bool_read_coeff_token(WebpBoolDec* br, const uint8_t* probs,
                                 int node) {
    int i = node;

    while ((i = webp_coeff_tree[i + bool_get(br, probs[i >> 1])]) > 0)
        ;
    return -i;
}

/*---------------------------------------------------------------------------
 * Residue tokens (RFC 6386 Section 13)
 *-------------------------------------------------------------------------*/

/* Decodes the residue of one macroblock into `coeffs`, which must already be
 * zeroed, and dequantizes it in place. `left` and `above` are the nine slot
 * entropy contexts and are updated as the blocks are read.
 *
 * The return value is the eob_mask of the bitstream guide: bits 0..24 mark
 * the blocks that ran past their second coefficient and bit 31 marks a
 * macroblock that carries residue at all, which is what the loop filter
 * consults to decide whether the sub block edges need filtering. */
static uint32_t vp8_decode_mb_tokens(
        WebpBoolDec* br, uint8_t* left, uint8_t* above, int16_t* coeffs,
        int y_mode,
        const uint8_t probs[VP8_BLOCK_TYPES][VP8_COEFF_BANDS]
                           [VP8_PREV_CONTEXTS][VP8_ENTROPY_NODES],
        const int16_t factor[VP8_TOKEN_BLOCK_TYPES][2]) {
    uint32_t eob_mask = 0;
    const uint8_t* type_probs;
    const int16_t* dqf;
    int16_t* block;
    int i, stop, type;

    /* A macroblock that is not split into sub blocks carries its luma DC
     * values in the second order block, so its sixteen 4x4 luma blocks start
     * at coefficient one. B_PRED has no second order block and starts at
     * zero, which selects a different set of default probabilities. */
    if (y_mode != VP8_B_PRED) {
        i = 24;  stop = 24;  type = 1;
        block = coeffs + 24 * 16;
        dqf = factor[VP8_TOKEN_Y2];
    } else {
        i = 0;   stop = 16;  type = 3;
        block = coeffs;
        dqf = factor[VP8_TOKEN_Y1];
    }
    type_probs = probs[type][0][0];

    for (;;) {
        const int first = (type == 0) ? 1 : 0;
        int c = first;
        int t = left[webp_left_ctx_idx[i]] + above[webp_above_ctx_idx[i]];
        int node = 0;

        for (;;) {
            const uint8_t* prob = type_probs + t * VP8_ENTROPY_NODES +
                                  VP8_BAND_OFF(webp_coeff_bands[c]);
            const int token = bool_read_coeff_token(br, prob, node);
            int val;

            if (token == VP8_TOK_EOB)
                break;

            if (token == VP8_TOK_ZERO) {
                if (c >= 15)
                    break;   /* malformed tail, the guide ends the block */
                ++c;
                t = 0;       /* the context resets after a zero coefficient */
                node = 2;    /* end-of-block cannot follow a zero */
                continue;
            }

            if (token <= VP8_TOK_FOUR) {
                val = token;                     /* ONE..FOUR are 1..4 */
                t = (val == 1) ? 1 : 2;
            } else {
                /* a category carries a fixed number of raw extra bits whose
                 * probabilities are given most significant bit first */
                const int cat = token - VP8_TOK_CAT1;
                const int bits = webp_cat_bits[cat];
                int j;

                val = webp_cat_base[cat];
                for (j = bits - 1; j >= 0; --j)
                    val += bool_get(br, webp_cat_probs[cat][bits - 1 - j]) << j;
                t = 2;
            }

            val = bool_get_bit(br) ? -val : val;
            /* coefficient zero uses the DC factor, the rest the AC one */
            block[webp_zigzag[c]] = (int16_t)(val * dqf[c != 0]);
            node = 0;
            if (c >= 15)
                break;
            ++c;
        }

        eob_mask |= (uint32_t)(c > 1) << i;
        t = (c != first);                  /* any data beyond the first? */
        eob_mask |= (uint32_t)t << 31;
        left[webp_left_ctx_idx[i]] = (uint8_t)t;
        above[webp_above_ctx_idx[i]] = (uint8_t)t;
        block += 16;

        if (++i < stop)
            continue;
        if (i == VP8_NUM_COEFF_BLOCKS) {   /* the luma 4x4 blocks follow Y2 */
            type = 0;
            i = 0;
            stop = 16;
            type_probs = probs[type][0][0];
            block = coeffs;
            dqf = factor[VP8_TOKEN_Y1];
            continue;
        }
        if (i == 16) {                     /* and the chroma blocks follow */
            type = 2;
            stop = 24;
            type_probs = probs[type][0][0];
            dqf = factor[VP8_TOKEN_UV];
            continue;
        }
        return eob_mask;
    }
}

/* Clears the contexts of a macroblock that decodes no tokens at all. The
 * second order slot survives when the mode would not have written it either,
 * that is for B_PRED, which has no second order block. */
static void vp8_reset_mb_context(uint8_t* left, uint8_t* above, int y_mode) {
    memset(left, 0, 8);
    memset(above, 0, 8);
    if (y_mode != VP8_B_PRED) {
        left[8] = 0;
        above[8] = 0;
    }
}

/*---------------------------------------------------------------------------
 * Macroblock modes (RFC 6386 Section 12)
 *-------------------------------------------------------------------------*/

/* The mode of the 4x4 sub block above sub block `b`. For the top row of the
 * macroblock it comes from the macroblock above, whose own 16x16 mode maps
 * onto the corresponding sub block mode. */
static int vp8_above_block_mode(const WebpMbInfo* mb,
                                const WebpMbInfo* above, int b) {
    if (b < 4) {
        switch (above->y_mode) {
        case VP8_DC_PRED: return VP8_B_DC_PRED;
        case VP8_V_PRED:  return VP8_B_VE_PRED;
        case VP8_H_PRED:  return VP8_B_HE_PRED;
        case VP8_TM_PRED: return VP8_B_TM_PRED;
        case VP8_B_PRED:  return above->b_modes[b + 12];
        default:          return VP8_B_DC_PRED;
        }
    }
    return mb->b_modes[b - 4];
}

/* The same for the sub block to the left, which is the rightmost column of
 * the macroblock on the left at the same height. */
static int vp8_left_block_mode(const WebpMbInfo* mb, const WebpMbInfo* left,
                               int b) {
    if ((b & 3) == 0) {
        switch (left->y_mode) {
        case VP8_DC_PRED: return VP8_B_DC_PRED;
        case VP8_V_PRED:  return VP8_B_VE_PRED;
        case VP8_H_PRED:  return VP8_B_HE_PRED;
        case VP8_TM_PRED: return VP8_B_TM_PRED;
        case VP8_B_PRED:  return left->b_modes[b + 3];
        default:          return VP8_B_DC_PRED;
        }
    }
    return mb->b_modes[b - 1];
}

/* The mode syntax of a key frame macroblock: the 16x16 luma mode first, then
 * - only for B_PRED - the sixteen sub block modes, each conditioned on the
 * modes above and to the left of it, and finally the chroma mode. */
static void vp8_decode_mb_modes(WebpBoolDec* br, WebpMbInfo* mb,
                                const WebpMbInfo* left,
                                const WebpMbInfo* above) {
    const int y_mode = bool_read_tree(br, webp_kf_y_mode_tree,
                                      webp_kf_y_mode_probs);
    int b;

    if (y_mode == VP8_B_PRED) {
        for (b = 0; b < 16; ++b) {
            const int a = vp8_above_block_mode(mb, above, b);
            const int l = vp8_left_block_mode(mb, left, b);

            mb->b_modes[b] = (uint8_t)bool_read_tree(
                    br, webp_b_mode_tree, webp_kf_bmode_probs[a][l]);
        }
    }
    mb->y_mode = (uint8_t)y_mode;
    mb->uv_mode = (uint8_t)bool_read_tree(br, webp_uv_mode_tree,
                                          webp_kf_uv_mode_probs);
}

/*---------------------------------------------------------------------------
 * Frame header (RFC 6386 Sections 9 and 10)
 *-------------------------------------------------------------------------*/

/* The segment of a macroblock, read from a three leaf tree whose branch
 * probabilities the segmentation header may have replaced. */
static int vp8_read_segment_id(WebpBoolDec* br, const WebpVp8Dec* dec) {
    return bool_get(br, dec->seg_tree_probs[0])
           ? 2 + bool_get(br, dec->seg_tree_probs[2])
           : bool_get(br, dec->seg_tree_probs[1]);
}

/* Section 9.3. The tree probabilities default to "always take the first
 * branch" and are only present when the map is updated. */
static void vp8_parse_segment_header(WebpVp8Dec* dec) {
    WebpBoolDec* br = &dec->part0;
    int i;

    dec->seg_enabled = bool_get_bit(br);
    if (!dec->seg_enabled)
        return;

    dec->seg_update_map = bool_get_bit(br);
    if (bool_get_bit(br)) {                    /* update the segment data */
        dec->seg_abs = bool_get_bit(br);
        for (i = 0; i < VP8_MAX_MB_SEGMENTS; ++i)
            dec->seg_quant_idx[i] = bool_maybe_get_int(br, 7);
        for (i = 0; i < VP8_MAX_MB_SEGMENTS; ++i)
            dec->seg_lf_level[i] = bool_maybe_get_int(br, 6);
    }
    if (dec->seg_update_map) {
        for (i = 0; i < 3; ++i)
            dec->seg_tree_probs[i] =
                    bool_get_bit(br) ? bool_get_uint(br, 8) : 255;
    }
}

static void vp8_parse_filter_header(WebpVp8Dec* dec) {
    WebpBoolDec* br = &dec->part0;
    int i;

    dec->lf_use_simple = bool_get_bit(br);
    dec->lf_level = bool_get_uint(br, 6);
    dec->lf_sharpness = bool_get_uint(br, 3);
    dec->lf_delta_enabled = bool_get_bit(br);

    /* the second flag says whether any delta is transmitted at all */
    if (dec->lf_delta_enabled && bool_get_bit(br)) {
        for (i = 0; i < VP8_BLOCK_CONTEXTS; ++i)
            dec->lf_ref_delta[i] = bool_maybe_get_int(br, 6);
        for (i = 0; i < VP8_BLOCK_CONTEXTS; ++i)
            dec->lf_mode_delta[i] = bool_maybe_get_int(br, 6);
    }
}

/* Section 9.5: the token partitions. All but the last carry an explicit
 * three byte length and the last one takes whatever is left. */
static int vp8_parse_partitions(WebpVp8Dec* dec, const uint8_t* data,
                                uint32_t size) {
    uint32_t part_size[VP8_MAX_PARTITIONS];
    const uint8_t* start;
    uint32_t left;
    int i;

    dec->num_partitions = 1 << bool_get_uint(&dec->part0, 2);
    if (size < 3u * (uint32_t)(dec->num_partitions - 1))
        return WEBP_ERR_CORRUPT;
    left = size - 3u * (uint32_t)(dec->num_partitions - 1);

    /* Every size but the last is spelled out in three bytes, and the last
     * partition gets whatever is left over. They all have to be collected
     * before any reader is started: the size fields sit together at the front
     * of the buffer, so the partition data only begins after the final one. */
    for (i = 0; i < dec->num_partitions; ++i) {
        if (i < dec->num_partitions - 1) {
            part_size[i] = webp_get_le24(data);
            data += 3;
        } else {
            part_size[i] = left;
        }
        if (part_size[i] > left)
            return WEBP_ERR_CORRUPT;
        left -= part_size[i];
    }

    start = data;
    for (i = 0; i < dec->num_partitions; ++i) {
        bool_init(&dec->tokens[i], start, part_size[i]);
        start += part_size[i];
    }
    return WEBP_OK;
}

static void vp8_parse_quant_header(WebpVp8Dec* dec) {
    WebpBoolDec* br = &dec->part0;

    dec->q_index = bool_get_uint(br, 7);
    dec->y1_dc_delta_q = bool_maybe_get_int(br, 4);
    dec->y2_dc_delta_q = bool_maybe_get_int(br, 4);
    dec->y2_ac_delta_q = bool_maybe_get_int(br, 4);
    dec->uv_dc_delta_q = bool_maybe_get_int(br, 4);
    dec->uv_ac_delta_q = bool_maybe_get_int(br, 4);
}

/* Section 9.9: a key frame always restarts from the default coefficient
 * probabilities and then applies the updates the header carries. The
 * interframe probability updates that follow in an interframe are not
 * reachable here. */
static void vp8_parse_entropy_header(WebpVp8Dec* dec) {
    WebpBoolDec* br = &dec->part0;
    int i, j, k, l;

    memcpy(dec->coeff_probs, webp_default_coeff_probs,
           sizeof(dec->coeff_probs));

    for (i = 0; i < VP8_BLOCK_TYPES; ++i)
        for (j = 0; j < VP8_COEFF_BANDS; ++j)
            for (k = 0; k < VP8_PREV_CONTEXTS; ++k)
                for (l = 0; l < VP8_ENTROPY_NODES; ++l)
                    if (bool_get(br, webp_coeff_update_probs[i][j][k][l]))
                        dec->coeff_probs[i][j][k][l] =
                                (uint8_t)bool_get_uint(br, 8);

    dec->coeff_skip_enabled = bool_get_bit(br);
    if (dec->coeff_skip_enabled)
        dec->coeff_skip_prob = (uint8_t)bool_get_uint(br, 8);
}

/* Section 10.4: the dequantization factors of every segment and block kind.
 * A segment either replaces the frame quantizer or is added to it. The index
 * has to be clamped first, because a relative segment can push it outside
 * the lookup tables. */
static int vp8_clamp_q(int q) {
    return q < 0 ? 0 : (q > 127 ? 127 : q);
}

static int vp8_dc_q(int q) {
    return webp_dc_q[vp8_clamp_q(q)];
}

static int vp8_ac_q(int q) {
    return webp_ac_q[vp8_clamp_q(q)];
}

static void vp8_init_dequant(WebpVp8Dec* dec) {
    int s;

    for (s = 0; s < VP8_MAX_MB_SEGMENTS; ++s) {
        int16_t (*f)[2] = dec->factor[s];
        int q = dec->q_index;
        int y2_ac;

        if (dec->seg_enabled)
            q = dec->seg_abs ? dec->seg_quant_idx[s]
                             : dec->q_index + dec->seg_quant_idx[s];

        f[VP8_TOKEN_Y1][0] = (int16_t)vp8_dc_q(q + dec->y1_dc_delta_q);
        f[VP8_TOKEN_Y1][1] = (int16_t)vp8_ac_q(q);
        f[VP8_TOKEN_UV][0] = (int16_t)vp8_dc_q(q + dec->uv_dc_delta_q);
        f[VP8_TOKEN_UV][1] = (int16_t)vp8_ac_q(q + dec->uv_ac_delta_q);
        f[VP8_TOKEN_Y2][0] = (int16_t)(vp8_dc_q(q + dec->y2_dc_delta_q) * 2);

        y2_ac = vp8_ac_q(q + dec->y2_ac_delta_q) * 155 / 100;
        if (y2_ac < 8)
            y2_ac = 8;
        f[VP8_TOKEN_Y2][1] = (int16_t)y2_ac;

        if (f[VP8_TOKEN_UV][0] > 132)
            f[VP8_TOKEN_UV][0] = 132;
    }
}

/*---------------------------------------------------------------------------
 * Planes
 *-------------------------------------------------------------------------*/

/* Allocates a plane of w x h pixels surrounded by the border that holds the
 * out-of-frame prediction pixels. The border is filled in by the predictor,
 * but zeroing it keeps an unwritten edge from feeding garbage into a
 * prediction. */
static int vp8_alloc_plane(WebpPlane* plane, int w, int h) {
    const size_t stride = (size_t)w + 2 * VP8_BORDER;
    const size_t rows = (size_t)h + 2 * VP8_BORDER;
    uint8_t* base = (uint8_t*)calloc(stride * rows, 1);

    if (base == NULL)
        return WEBP_ERR_OOM;

    plane->base = base;
    plane->stride = (int)stride;
    plane->pixels = base + VP8_BORDER * stride + VP8_BORDER;
    return WEBP_OK;
}

/*---------------------------------------------------------------------------
 * Entry point
 *-------------------------------------------------------------------------*/

int webp_vp8_decode(const uint8_t* data, uint32_t size,
                    uint32_t** out_pixels, int* out_width, int* out_height) {
    WebpVp8Dec dec;
    WebpMbInfo* mb_rows = NULL;
    int16_t* coeffs = NULL;
    uint8_t (*above_ctx)[VP8_CTX_SLOTS] = NULL;
    uint8_t left_ctx[VP8_CTX_SLOTS];
    uint32_t* pixels = NULL;
    const uint8_t* tokens;
    uint32_t tokens_size;
    uint32_t raw, part0_sz;
    int row, ret;

    *out_pixels = NULL;
    *out_width = 0;
    *out_height = 0;

    if (data == NULL || size < VP8_FIXED_HEADER)
        return WEBP_ERR_CORRUPT;

    memset(&dec, 0, sizeof(dec));

    /* Section 9.1: the frame tag is a three byte little endian value. */
    raw = webp_get_le24(data);
    if (raw & 1u)
        return WEBP_ERR_UNSUPPORTED;   /* an interframe has no reference */
    if ((raw >> 3) & 1u)
        return WEBP_ERR_UNSUPPORTED;   /* experimental bitstream */
    part0_sz = (raw >> 5) & 0x7ffffu;
    /* the version field selects an encoder complexity profile only; it does
     * not change decoding */

    if (data[3] != 0x9d || data[4] != 0x01 || data[5] != 0x2a)
        return WEBP_ERR_CORRUPT;       /* the key frame start code */

    raw = webp_get_le32(data + 6);
    dec.width = (int)(raw & 0x3fffu);
    dec.height = (int)((raw >> 16) & 0x3fffu);
    /* The two bit scaling factors next to each dimension are a hint to resize
     * the picture after decoding. They carry no information the decoder needs
     * and the WebP container states the display size itself, so they are read
     * and dropped rather than refused. */
    if (dec.width == 0 || dec.height == 0)
        return WEBP_ERR_CORRUPT;
    if (dec.width > WEBP_MAX_DIM || dec.height > WEBP_MAX_DIM ||
        (uint64_t)dec.width * dec.height > WEBP_MAX_PIXELS)
        return WEBP_ERR_TOO_LARGE;

    if (size - VP8_FIXED_HEADER < part0_sz)
        return WEBP_ERR_CORRUPT;

    dec.mb_cols = (dec.width + 15) / 16;
    dec.mb_rows = (dec.height + 15) / 16;
    dec.part0_size = (int)part0_sz;

    bool_init(&dec.part0, data + VP8_FIXED_HEADER, part0_sz);
    tokens = data + VP8_FIXED_HEADER + part0_sz;
    tokens_size = size - VP8_FIXED_HEADER - part0_sz;

    /* Section 9.3: the colour space and the clamping type. Only the BT.601
     * like space without clamping is defined, and a non-zero value here
     * makes the frame undecodable. */
    if (bool_get_uint(&dec.part0, 2) != 0)
        return WEBP_ERR_UNSUPPORTED;

    vp8_parse_segment_header(&dec);
    vp8_parse_filter_header(&dec);
    ret = vp8_parse_partitions(&dec, tokens, tokens_size);
    if (ret != WEBP_OK)
        return ret;
    vp8_parse_quant_header(&dec);
    /* Section 9.8: a key frame refreshes every reference buffer, so only the
     * entropy refresh flag is on the wire. It means nothing for a single
     * frame but the bit still has to be consumed to stay aligned. */
    (void)bool_get_bit(&dec.part0);
    vp8_parse_entropy_header(&dec);
    vp8_init_dequant(&dec);

    /* Two macroblock info rows are enough: prediction and filtering only
     * ever look one row up. Index zero of each row is the out-of-frame
     * macroblock to the left. */
    mb_rows = (WebpMbInfo*)calloc((size_t)(dec.mb_cols + 1) * 2,
                                  sizeof(*mb_rows));
    coeffs = (int16_t*)calloc((size_t)dec.mb_cols * VP8_MB_COEFFS,
                              sizeof(*coeffs));
    above_ctx = (uint8_t(*)[VP8_CTX_SLOTS])calloc((size_t)dec.mb_cols,
                                                  VP8_CTX_SLOTS);
    if (mb_rows == NULL || coeffs == NULL || above_ctx == NULL) {
        ret = WEBP_ERR_OOM;
        goto cleanup;
    }

    ret = vp8_alloc_plane(&dec.y, dec.mb_cols * 16, dec.mb_rows * 16);
    if (ret == WEBP_OK)
        ret = vp8_alloc_plane(&dec.u, dec.mb_cols * 8, dec.mb_rows * 8);
    if (ret == WEBP_OK)
        ret = vp8_alloc_plane(&dec.v, dec.mb_cols * 8, dec.mb_rows * 8);
    if (ret != WEBP_OK)
        goto cleanup;

    for (row = 0; row < dec.mb_rows; ++row) {
        WebpMbInfo* cur = mb_rows + (size_t)(row & 1) * (dec.mb_cols + 1);
        const WebpMbInfo* above =
                mb_rows + (size_t)((row + 1) & 1) * (dec.mb_cols + 1);
        WebpBoolDec* tok = &dec.tokens[row % dec.num_partitions];
        int col;

        /* the macroblock to the left of the leftmost column reads as an
         * all-zero, that is DC_PRED, macroblock */
        memset(cur, 0, sizeof(*cur));

        for (col = 0; col < dec.mb_cols; ++col) {
            WebpMbInfo* mb = cur + 1 + col;

            mb->segment_id = dec.seg_update_map
                    ? (uint8_t)vp8_read_segment_id(&dec.part0, &dec) : 0;
            mb->skip_coeff = dec.coeff_skip_enabled
                    ? (uint8_t)bool_get(&dec.part0, dec.coeff_skip_prob) : 0;
            mb->eob_mask = 0;
            vp8_decode_mb_modes(&dec.part0, mb, cur + col, above + 1 + col);
        }

        /* The left context restarts on every row while the above one only
         * restarts on the first. */
        memset(left_ctx, 0, sizeof(left_ctx));
        if (row == 0)
            memset(above_ctx, 0, (size_t)dec.mb_cols * VP8_CTX_SLOTS);

        for (col = 0; col < dec.mb_cols; ++col) {
            WebpMbInfo* mb = cur + 1 + col;
            int16_t* block = coeffs + (size_t)col * VP8_MB_COEFFS;
            uint8_t* above_slot = above_ctx[col];

            memset(block, 0, VP8_MB_COEFFS * sizeof(*block));
            if (mb->skip_coeff)
                vp8_reset_mb_context(left_ctx, above_slot, mb->y_mode);
            else
                mb->eob_mask = vp8_decode_mb_tokens(
                        tok, left_ctx, above_slot, block, mb->y_mode,
                        dec.coeff_probs, dec.factor[mb->segment_id]);
        }

        webp_vp8_predict_row(&dec, row, cur + 1, coeffs);

        /* The filter lags one row behind the prediction, because intra
         * prediction has to see the unfiltered pixels of the row above. */
        if (row > 0)
            webp_vp8_filter_row(&dec, row - 1, above + 1);
    }
    webp_vp8_filter_row(&dec, dec.mb_rows - 1,
                        mb_rows + (size_t)((dec.mb_rows - 1) & 1) *
                                  (dec.mb_cols + 1) + 1);

    pixels = (uint32_t*)malloc((size_t)dec.width * dec.height *
                               sizeof(*pixels));
    if (pixels == NULL) {
        ret = WEBP_ERR_OOM;
        goto cleanup;
    }
    webp_vp8_to_argb(&dec, pixels);

    *out_pixels = pixels;
    *out_width = dec.width;
    *out_height = dec.height;
    ret = WEBP_OK;

cleanup:
    free(coeffs);
    free(mb_rows);
    free(above_ctx);
    free(dec.y.base);
    free(dec.u.base);
    free(dec.v.base);
    return ret;
}
