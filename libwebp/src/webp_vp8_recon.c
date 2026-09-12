/*
 * webp_vp8_recon.c - the reconstruction half of the lossy VP8 decoder:
 * inverse transforms, intra prediction, the loop filter and the final
 * YUV -> ARGB conversion.
 *
 * Every arithmetic step follows the reference decoder of RFC 6386
 * Section 20 (files idct_add.c, predict.c and dixie_loopfilter.c) so that
 * the output is bit identical to what the specification mandates.
 */
#include "webp_internal.h"
#include "webp_vp8.h"

/* Saturating clamps of the bitstream guide: sat8 keeps a signed value in
 * -128..127, clamp255 keeps a filtered pixel in 0..255. */
#define VP8_SAT8(x)     ((x) < -128 ? -128 : ((x) > 127 ? 127 : (x)))
#define VP8_CLAMP255(x) ((x) < 0 ? 0 : ((x) > 255 ? 255 : (x)))

/* Fixed point constants of the 4x4 inverse transform (RFC 6386 14.4):
 * (cos(pi/8) * sqrt(2) - 1) and (sin(pi/8) * sqrt(2)), both scaled by 2^16. */
#define VP8_IDCT_COS8  20091
#define VP8_IDCT_SIN8  35468

/*---------------------------------------------------------------------------
 * Inverse transforms
 *-------------------------------------------------------------------------*/

/* Second order (Walsh-Hadamard) transform of the sixteen luma DC values.
 * The result feeds back as coefficient zero of each 4x4 luma block. */
static void vp8_walsh(const int16_t* in, int16_t* out) {
    int16_t tmp[16];
    int i;

    for (i = 0; i < 4; ++i) {
        const int a1 = in[0] + in[12];
        const int b1 = in[4] + in[8];
        const int c1 = in[4] - in[8];
        const int d1 = in[0] - in[12];

        tmp[i]      = (int16_t)(a1 + b1);
        tmp[i + 4]  = (int16_t)(c1 + d1);
        tmp[i + 8]  = (int16_t)(a1 - b1);
        tmp[i + 12] = (int16_t)(d1 - c1);
        ++in;
    }

    for (i = 0; i < 16; i += 4) {
        const int a1 = tmp[i] + tmp[i + 3];
        const int b1 = tmp[i + 1] + tmp[i + 2];
        const int c1 = tmp[i + 1] - tmp[i + 2];
        const int d1 = tmp[i] - tmp[i + 3];

        out[i]     = (int16_t)((a1 + b1 + 3) >> 3);
        out[i + 1] = (int16_t)((c1 + d1 + 3) >> 3);
        out[i + 2] = (int16_t)((a1 - b1 + 3) >> 3);
        out[i + 3] = (int16_t)((d1 - c1 + 3) >> 3);
    }
}

/* Columns of the 4x4 inverse transform, from dequantized coefficients into
 * a scratch block. The rows are handled by the caller, which can then add
 * the prediction and clamp in one pass. */
static void vp8_idct_cols(const int16_t* in, int16_t* out) {
    int i;

    for (i = 0; i < 4; ++i) {
        const int a1 = in[0] + in[8];
        const int b1 = in[0] - in[8];
        const int c1 = ((in[4] * VP8_IDCT_SIN8) >> 16) -
                       (in[12] + ((in[12] * VP8_IDCT_COS8) >> 16));
        const int d1 = (in[4] + ((in[4] * VP8_IDCT_COS8) >> 16)) +
                       ((in[12] * VP8_IDCT_SIN8) >> 16);

        out[0]  = (int16_t)(a1 + d1);
        out[4]  = (int16_t)(b1 + c1);
        out[8]  = (int16_t)(b1 - c1);
        out[12] = (int16_t)(a1 - d1);
        ++in;
        ++out;
    }
}

/* Full 4x4 inverse transform added onto the prediction, in place. */
static void vp8_idct_add(uint8_t* recon, const uint8_t* pred, int stride,
                         const int16_t* coeffs) {
    int16_t tmp[16];
    int i;

    vp8_idct_cols(coeffs, tmp);

    for (i = 0; i < 4; ++i) {
        const int16_t* t = tmp + i * 4;
        const int a1 = t[0] + t[2];
        const int b1 = t[0] - t[2];
        const int c1 = ((t[1] * VP8_IDCT_SIN8) >> 16) -
                       (t[3] + ((t[3] * VP8_IDCT_COS8) >> 16));
        const int d1 = (t[1] + ((t[1] * VP8_IDCT_COS8) >> 16)) +
                       ((t[3] * VP8_IDCT_SIN8) >> 16);

        recon[0] = (uint8_t)VP8_CLAMP255(pred[0] + ((a1 + d1 + 4) >> 3));
        recon[1] = (uint8_t)VP8_CLAMP255(pred[1] + ((b1 + c1 + 4) >> 3));
        recon[2] = (uint8_t)VP8_CLAMP255(pred[2] + ((b1 - c1 + 4) >> 3));
        recon[3] = (uint8_t)VP8_CLAMP255(pred[3] + ((a1 - d1 + 4) >> 3));
        recon += stride;
        pred += stride;
    }
}

/*---------------------------------------------------------------------------
 * Intra prediction
 *
 * The predictors are built directly into the reconstructed plane and the
 * residue is then added on top, which is why every routine below takes a
 * pointer to the top-left pixel of the block it fills.
 *-------------------------------------------------------------------------*/

static void vp8_pred_h(uint8_t* p, int stride, int n) {
    const uint8_t* left = p - 1;
    int i, j;

    for (i = 0; i < n; ++i) {
        for (j = 0; j < n; ++j)
            p[j] = left[0];
        p += stride;
        left += stride;
    }
}

static void vp8_pred_v(uint8_t* p, int stride, int n) {
    const uint8_t* above = p - stride;
    int i, j;

    for (i = 0; i < n; ++i) {
        for (j = 0; j < n; ++j)
            p[j] = above[j];
        p += stride;
    }
}

/* True motion prediction: p + above[i] - left[j] for every pixel. */
static void vp8_pred_tm(uint8_t* p, int stride, int n) {
    const uint8_t* left = p - 1;
    const uint8_t* above = p - stride;
    const int tl = above[-1];
    int i, j;

    for (i = 0; i < n; ++i) {
        for (j = 0; j < n; ++j)
            p[j] = (uint8_t)VP8_CLAMP255(left[0] + above[j] - tl);
        p += stride;
        left += stride;
    }
}

/* DC prediction averages the row above and the column to the left. The
 * out-of-frame fixups of vp8_fixup_left/vp8_fixup_above have already made
 * sure that both edges exist, so no availability test is needed here. */
static void vp8_pred_dc(uint8_t* p, int stride, int n) {
    const uint8_t* left = p - 1;
    const uint8_t* above = p - stride;
    int i, j, dc = 0;

    for (i = 0; i < n; ++i) {
        dc += left[0] + above[i];
        left += stride;
    }
    if (n == 16)      dc = (dc + 16) >> 5;
    else if (n == 8)  dc = (dc + 8) >> 4;
    else              dc = (dc + 4) >> 3;

    for (i = 0; i < n; ++i) {
        for (j = 0; j < n; ++j)
            p[j] = (uint8_t)dc;
        p += stride;
    }
}

static void vp8_pred_ve_4x4(uint8_t* p, int stride) {
    const uint8_t* a = p - stride;
    int i, j;

    p[0] = (uint8_t)((a[-1] + 2 * a[0] + a[1] + 2) >> 2);
    p[1] = (uint8_t)((a[ 0] + 2 * a[1] + a[2] + 2) >> 2);
    p[2] = (uint8_t)((a[ 1] + 2 * a[2] + a[3] + 2) >> 2);
    p[3] = (uint8_t)((a[ 2] + 2 * a[3] + a[4] + 2) >> 2);
    for (i = 1; i < 4; ++i) {
        p += stride;
        for (j = 0; j < 4; ++j)
            p[j] = p[j - stride];
    }
}

static void vp8_pred_he_4x4(uint8_t* p, int stride) {
    const uint8_t* l = p - 1;
    int i;

    for (i = 0; i < 4; ++i) {
        /* the bottom row has no pixel below it, so it repeats its own */
        const int below = (i == 3) ? l[0] : l[stride];
        const uint8_t val = (uint8_t)((l[-stride] + 2 * l[0] + below + 2) >> 2);
        p[0] = p[1] = p[2] = p[3] = val;
        p += stride;
        l += stride;
    }
}

static void vp8_pred_ld_4x4(uint8_t* p, int stride) {
    const uint8_t* a = p - stride;
    const int c0 = (a[0] + 2 * a[1] + a[2] + 2) >> 2;
    const int c1 = (a[1] + 2 * a[2] + a[3] + 2) >> 2;
    const int c2 = (a[2] + 2 * a[3] + a[4] + 2) >> 2;
    const int c3 = (a[3] + 2 * a[4] + a[5] + 2) >> 2;
    const int c4 = (a[4] + 2 * a[5] + a[6] + 2) >> 2;
    const int c5 = (a[5] + 2 * a[6] + a[7] + 2) >> 2;
    /* the top row is extended by repeating its last pixel, so above[7] is
     * used twice for the bottom right value */
    const int c6 = (a[6] + 2 * a[7] + a[7] + 2) >> 2;

    p[0] = (uint8_t)c0; p[1] = (uint8_t)c1; p[2] = (uint8_t)c2; p[3] = (uint8_t)c3;
    p += stride;
    p[0] = (uint8_t)c1; p[1] = (uint8_t)c2; p[2] = (uint8_t)c3; p[3] = (uint8_t)c4;
    p += stride;
    p[0] = (uint8_t)c2; p[1] = (uint8_t)c3; p[2] = (uint8_t)c4; p[3] = (uint8_t)c5;
    p += stride;
    p[0] = (uint8_t)c3; p[1] = (uint8_t)c4; p[2] = (uint8_t)c5; p[3] = (uint8_t)c6;
}

static void vp8_pred_rd_4x4(uint8_t* p, int stride) {
    const uint8_t* l = p - 1;
    const uint8_t* a = p - stride;
    const int c0 = (a[-1] + 2 * a[0] + a[1] + 2) >> 2;
    const int c1 = (a[ 0] + 2 * a[1] + a[2] + 2) >> 2;
    const int c2 = (a[ 1] + 2 * a[2] + a[3] + 2) >> 2;
    const int c3 = (l[ 0] + 2 * a[-1] + a[0] + 2) >> 2;
    const int c4 = (l[stride] + 2 * l[0] + a[-1] + 2) >> 2;
    const int c5 = (l[stride * 2] + 2 * l[stride] + l[0] + 2) >> 2;
    const int c6 = (l[stride * 3] + 2 * l[stride * 2] + l[stride] + 2) >> 2;

    p[0] = (uint8_t)c3; p[1] = (uint8_t)c0; p[2] = (uint8_t)c1; p[3] = (uint8_t)c2;
    p += stride;
    p[0] = (uint8_t)c4; p[1] = (uint8_t)c3; p[2] = (uint8_t)c0; p[3] = (uint8_t)c1;
    p += stride;
    p[0] = (uint8_t)c5; p[1] = (uint8_t)c4; p[2] = (uint8_t)c3; p[3] = (uint8_t)c0;
    p += stride;
    p[0] = (uint8_t)c6; p[1] = (uint8_t)c5; p[2] = (uint8_t)c4; p[3] = (uint8_t)c3;
}

static void vp8_pred_vr_4x4(uint8_t* p, int stride) {
    const uint8_t* l = p - 1;
    const uint8_t* a = p - stride;
    const int s0 = (a[-1] + a[0] + 1) >> 1;
    const int s1 = (a[ 0] + a[1] + 1) >> 1;
    const int s2 = (a[ 1] + a[2] + 1) >> 1;
    const int s3 = (a[ 2] + a[3] + 1) >> 1;
    const int c0 = (a[-1] + 2 * a[0] + a[1] + 2) >> 2;
    const int c1 = (a[ 0] + 2 * a[1] + a[2] + 2) >> 2;
    const int c2 = (a[ 1] + 2 * a[2] + a[3] + 2) >> 2;
    const int c3 = (l[0] + 2 * a[-1] + a[0] + 2) >> 2;
    const int c4 = (l[stride] + 2 * l[0] + a[-1] + 2) >> 2;
    const int c5 = (l[stride * 2] + 2 * l[stride] + l[0] + 2) >> 2;

    p[0] = (uint8_t)s0; p[1] = (uint8_t)s1; p[2] = (uint8_t)s2; p[3] = (uint8_t)s3;
    p += stride;
    p[0] = (uint8_t)c3; p[1] = (uint8_t)c0; p[2] = (uint8_t)c1; p[3] = (uint8_t)c2;
    p += stride;
    p[0] = (uint8_t)c4; p[1] = (uint8_t)s0; p[2] = (uint8_t)s1; p[3] = (uint8_t)s2;
    p += stride;
    p[0] = (uint8_t)c5; p[1] = (uint8_t)c3; p[2] = (uint8_t)c0; p[3] = (uint8_t)c1;
}

static void vp8_pred_vl_4x4(uint8_t* p, int stride) {
    const uint8_t* a = p - stride;
    const int s0 = (a[0] + a[1] + 1) >> 1;
    const int s1 = (a[1] + a[2] + 1) >> 1;
    const int s2 = (a[2] + a[3] + 1) >> 1;
    const int s3 = (a[3] + a[4] + 1) >> 1;
    const int c0 = (a[0] + 2 * a[1] + a[2] + 2) >> 2;
    const int c1 = (a[1] + 2 * a[2] + a[3] + 2) >> 2;
    const int c2 = (a[2] + 2 * a[3] + a[4] + 2) >> 2;
    const int c3 = (a[3] + 2 * a[4] + a[5] + 2) >> 2;
    const int c4 = (a[4] + 2 * a[5] + a[6] + 2) >> 2;
    const int c5 = (a[5] + 2 * a[6] + a[7] + 2) >> 2;

    p[0] = (uint8_t)s0; p[1] = (uint8_t)s1; p[2] = (uint8_t)s2; p[3] = (uint8_t)s3;
    p += stride;
    p[0] = (uint8_t)c0; p[1] = (uint8_t)c1; p[2] = (uint8_t)c2; p[3] = (uint8_t)c3;
    p += stride;
    p[0] = (uint8_t)s1; p[1] = (uint8_t)s2; p[2] = (uint8_t)s3; p[3] = (uint8_t)c4;
    p += stride;
    p[0] = (uint8_t)c1; p[1] = (uint8_t)c2; p[2] = (uint8_t)c3; p[3] = (uint8_t)c5;
}

static void vp8_pred_hd_4x4(uint8_t* p, int stride) {
    const uint8_t* l = p - 1;
    const uint8_t* a = p - stride;
    const int s0 = (l[0] + a[-1] + 1) >> 1;
    const int c0 = (l[0] + 2 * a[-1] + a[0] + 2) >> 2;
    const int c1 = (a[-1] + 2 * a[0] + a[1] + 2) >> 2;
    const int c2 = (a[0] + 2 * a[1] + a[2] + 2) >> 2;
    const int s1 = (l[stride] + l[0] + 1) >> 1;
    const int c3 = (l[stride] + 2 * l[0] + a[-1] + 2) >> 2;
    const int s2 = (l[stride * 2] + l[stride] + 1) >> 1;
    const int c4 = (l[stride * 2] + 2 * l[stride] + l[0] + 2) >> 2;
    const int s3 = (l[stride * 3] + l[stride * 2] + 1) >> 1;
    const int c5 = (l[stride * 3] + 2 * l[stride * 2] + l[stride] + 2) >> 2;

    p[0] = (uint8_t)s0; p[1] = (uint8_t)c0; p[2] = (uint8_t)c1; p[3] = (uint8_t)c2;
    p += stride;
    p[0] = (uint8_t)s1; p[1] = (uint8_t)c3; p[2] = (uint8_t)s0; p[3] = (uint8_t)c0;
    p += stride;
    p[0] = (uint8_t)s2; p[1] = (uint8_t)c4; p[2] = (uint8_t)s1; p[3] = (uint8_t)c3;
    p += stride;
    p[0] = (uint8_t)s3; p[1] = (uint8_t)c5; p[2] = (uint8_t)s2; p[3] = (uint8_t)c4;
}

static void vp8_pred_hu_4x4(uint8_t* p, int stride) {
    const uint8_t* l = p - 1;
    const int l0 = l[0], l1 = l[stride], l2 = l[stride * 2], l3 = l[stride * 3];
    const int s0 = (l0 + l1 + 1) >> 1;
    const int c0 = (l0 + 2 * l1 + l2 + 2) >> 2;
    const int s1 = (l1 + l2 + 1) >> 1;
    const int c1 = (l1 + 2 * l2 + l3 + 2) >> 2;
    const int s2 = (l2 + l3 + 1) >> 1;
    /* there is nothing below the last left pixel, so it is repeated */
    const int c2 = (l2 + 2 * l3 + l3 + 2) >> 2;

    p[0] = (uint8_t)s0; p[1] = (uint8_t)c0; p[2] = (uint8_t)s1; p[3] = (uint8_t)c1;
    p += stride;
    p[0] = (uint8_t)s1; p[1] = (uint8_t)c1; p[2] = (uint8_t)s2; p[3] = (uint8_t)c2;
    p += stride;
    p[0] = (uint8_t)s2; p[1] = (uint8_t)c2; p[2] = (uint8_t)l3; p[3] = (uint8_t)l3;
    p += stride;
    p[0] = p[1] = p[2] = p[3] = (uint8_t)l3;
}

/* The four pixels above and to the right of sub block 3 are also the ones
 * used by sub blocks 7, 11 and 15, whose own neighbours have not been
 * reconstructed yet. Copy them down before any sub block is predicted. */
static void vp8_copy_down(uint8_t* p, int stride) {
    uint8_t tmp[4];
    int i, row;

    for (i = 0; i < 4; ++i)
        tmp[i] = p[16 - stride + i];
    for (row = 1; row <= 3; ++row) {
        uint8_t* dst = p + 16 - stride + row * 4 * stride;
        for (i = 0; i < 4; ++i)
            dst[i] = tmp[i];
    }
}

static void vp8_predict_b(uint8_t* p, int stride, const WebpMbInfo* mb,
                          const int16_t* coeffs) {
    int i;

    vp8_copy_down(p, stride);

    for (i = 0; i < 16; ++i) {
        uint8_t* b = p + (i & 3) * 4;

        switch (mb->b_modes[i]) {
        case VP8_B_DC_PRED: vp8_pred_dc(b, stride, 4);     break;
        case VP8_B_TM_PRED: vp8_pred_tm(b, stride, 4);     break;
        case VP8_B_VE_PRED: vp8_pred_ve_4x4(b, stride);    break;
        case VP8_B_HE_PRED: vp8_pred_he_4x4(b, stride);    break;
        case VP8_B_LD_PRED: vp8_pred_ld_4x4(b, stride);    break;
        case VP8_B_RD_PRED: vp8_pred_rd_4x4(b, stride);    break;
        case VP8_B_VR_PRED: vp8_pred_vr_4x4(b, stride);    break;
        case VP8_B_VL_PRED: vp8_pred_vl_4x4(b, stride);    break;
        case VP8_B_HD_PRED: vp8_pred_hd_4x4(b, stride);    break;
        default:            vp8_pred_hu_4x4(b, stride);    break;
        }
        vp8_idct_add(b, b, stride, coeffs);
        coeffs += 16;

        if ((i & 3) == 3)
            p += stride * 4;
    }
}

static void vp8_predict_luma(uint8_t* p, int stride, const WebpMbInfo* mb,
                             int16_t* coeffs) {
    int i;

    if (mb->y_mode == VP8_B_PRED) {
        vp8_predict_b(p, stride, mb, coeffs);
        return;
    }

    switch (mb->y_mode) {
    case VP8_DC_PRED: vp8_pred_dc(p, stride, 16); break;
    case VP8_V_PRED:  vp8_pred_v(p, stride, 16);  break;
    case VP8_H_PRED:  vp8_pred_h(p, stride, 16);  break;
    default:          vp8_pred_tm(p, stride, 16); break;
    }

    /* The sixteen luma DC values arrive as one Walsh block; unpack it into
     * coefficient zero of each 4x4 block before the inverse transforms. */
    {
        int16_t y2[16];
        vp8_walsh(coeffs + 24 * 16, y2);
        for (i = 0; i < 16; ++i)
            coeffs[i * 16] = y2[i];
    }

    for (i = 0; i < 16; ++i) {
        vp8_idct_add(p, p, stride, coeffs);
        coeffs += 16;
        p += 4;
        if ((i & 3) == 3)
            p += stride * 4 - 16;
    }
}

static void vp8_predict_chroma(uint8_t* up, uint8_t* vp, int stride,
                               const WebpMbInfo* mb, const int16_t* coeffs) {
    int i;

    switch (mb->uv_mode) {
    case VP8_DC_PRED:
        vp8_pred_dc(up, stride, 8);
        vp8_pred_dc(vp, stride, 8);
        break;
    case VP8_V_PRED:
        vp8_pred_v(up, stride, 8);
        vp8_pred_v(vp, stride, 8);
        break;
    case VP8_H_PRED:
        vp8_pred_h(up, stride, 8);
        vp8_pred_h(vp, stride, 8);
        break;
    default:
        vp8_pred_tm(up, stride, 8);
        vp8_pred_tm(vp, stride, 8);
        break;
    }

    coeffs += 16 * 16;
    for (i = 0; i < 4; ++i) {
        vp8_idct_add(up, up, stride, coeffs);
        coeffs += 16;
        up += 4;
        if (i & 1)
            up += stride * 4 - 8;
    }
    for (i = 0; i < 4; ++i) {
        vp8_idct_add(vp, vp, stride, coeffs);
        coeffs += 16;
        vp += 4;
        if (i & 1)
            vp += stride * 4 - 8;
    }
}

/* The left column of out-of-frame pixels is 129, except that DC prediction
 * averages whatever edges do exist and therefore needs the missing one
 * filled in with a copy of the edge that is there. */
static void vp8_fixup_left(uint8_t* p, int width, int stride, int row,
                           int mode) {
    uint8_t* left = p - 1;
    int i;

    if (mode == VP8_DC_PRED && row != 0) {
        const uint8_t* above = p - stride;
        for (i = 0; i < width; ++i) {
            *left = above[i];
            left += stride;
        }
    } else {
        /* start one row higher: the pixel above-left belongs to the left
         * column too and may still hold a DC prediction copy */
        left -= stride;
        for (i = -1; i < width; ++i) {
            *left = 129;
            left += stride;
        }
    }
}

/* The row of out-of-frame pixels above the frame is 127, with the same DC
 * prediction exception as the left column. The four pixels above and to the
 * right of the macroblock are always 127 on the top row. */
static void vp8_fixup_above(uint8_t* p, int width, int stride, int col,
                            int mode) {
    uint8_t* above = p - stride;
    int i;

    if (mode == VP8_DC_PRED && col != 0) {
        const uint8_t* left = p - 1;
        for (i = 0; i < width; ++i) {
            above[i] = *left;
            left += stride;
        }
    } else {
        memset(above - 1, 127, (size_t)width + 1);
    }
    memset(above + width, 127, 4);
}

void webp_vp8_predict_row(const WebpVp8Dec* dec, int row,
                          const WebpMbInfo* mbs, int16_t* coeffs) {
    const int ystride = dec->y.stride;
    const int uvstride = dec->u.stride;
    uint8_t* yp = dec->y.pixels + (size_t)row * 16 * ystride;
    uint8_t* up = dec->u.pixels + (size_t)row * 8 * uvstride;
    uint8_t* vp = dec->v.pixels + (size_t)row * 8 * uvstride;
    int16_t* block = coeffs;   /* the Walsh block is written back into it */
    int col;

    vp8_fixup_left(yp, 16, ystride, row, mbs[0].y_mode);
    vp8_fixup_left(up, 8, uvstride, row, mbs[0].uv_mode);
    vp8_fixup_left(vp, 8, uvstride, row, mbs[0].uv_mode);
    if (row == 0)
        yp[-ystride - 1] = 127;

    for (col = 0; col < dec->mb_cols; ++col) {
        if (row == 0) {
            vp8_fixup_above(yp, 16, ystride, col, mbs[col].y_mode);
            vp8_fixup_above(up, 8, uvstride, col, mbs[col].uv_mode);
            vp8_fixup_above(vp, 8, uvstride, col, mbs[col].uv_mode);
        }
        vp8_predict_luma(yp, ystride, &mbs[col], block);
        vp8_predict_chroma(up, vp, uvstride, &mbs[col], block);

        yp += 16;
        up += 8;
        vp += 8;
        block += VP8_NUM_COEFF_BLOCKS * 16;
    }

    /* RFC 6386 12.2: the rightmost macroblock of a row has no neighbour to
     * the right, so its above-right pixels repeat the pixel immediately
     * above its right edge. The border is wide enough to hold them. */
    {
        const uint8_t val = yp[-1 + 15 * ystride];
        uint8_t* ext = yp + 15 * ystride;
        ext[0] = ext[1] = ext[2] = ext[3] = val;
    }
}

/*---------------------------------------------------------------------------
 * Loop filter
 *-------------------------------------------------------------------------*/

static int vp8_abs(int x) { return x < 0 ? -x : x; }

/* |p1 - p0| and |q1 - q0| above the high edge variance threshold means the
 * edge is a real detail edge and only the narrow filter may be used. */
static int vp8_hev(const uint8_t* p, int off, int threshold) {
    return vp8_abs((int)p[-2 * off] - (int)p[-off]) > threshold ||
           vp8_abs((int)p[ off] - (int)p[0]) > threshold;
}

/* The filter is disabled when the four pixels straddling the edge differ
 * too much. */
static int vp8_simple_ok(const uint8_t* p, int off, int limit) {
    return (vp8_abs((int)p[-off] - (int)p[0]) * 2 +
            (vp8_abs((int)p[-2 * off] - (int)p[off]) >> 1)) <= limit;
}

static int vp8_normal_ok(const uint8_t* p, int off, int edge, int interior) {
    return vp8_simple_ok(p, off, 2 * edge + interior) &&
           vp8_abs((int)p[-4 * off] - (int)p[-3 * off]) <= interior &&
           vp8_abs((int)p[-3 * off] - (int)p[-2 * off]) <= interior &&
           vp8_abs((int)p[-2 * off] - (int)p[ -off]) <= interior &&
           vp8_abs((int)p[ 3 * off] - (int)p[ 2 * off]) <= interior &&
           vp8_abs((int)p[ 2 * off] - (int)p[  off]) <= interior &&
           vp8_abs((int)p[  off] - (int)p[0]) <= interior;
}

/* Narrow filter: two or four taps depending on the edge variance. */
static void vp8_filter_narrow(uint8_t* p, int off, int outer_taps) {
    int a = 3 * ((int)p[0] - (int)p[-off]);
    int f1, f2;

    if (outer_taps)
        a += VP8_SAT8((int)p[-2 * off] - (int)p[off]);
    a = VP8_SAT8(a);

    f1 = (a + 4 > 127 ? 127 : a + 4) >> 3;
    f2 = (a + 3 > 127 ? 127 : a + 3) >> 3;

    p[-off] = (uint8_t)VP8_CLAMP255((int)p[-off] + f2);
    p[0]    = (uint8_t)VP8_CLAMP255((int)p[0] - f1);

    if (!outer_taps) {
        a = (f1 + 1) >> 1;
        p[-2 * off] = (uint8_t)VP8_CLAMP255((int)p[-2 * off] + a);
        p[ off]     = (uint8_t)VP8_CLAMP255((int)p[ off] - a);
    }
}

/* Wide filter, used for macroblock edges that are not high variance. */
static void vp8_filter_wide(uint8_t* p, int off) {
    const int w = VP8_SAT8(VP8_SAT8((int)p[-2 * off] - (int)p[off]) +
                           3 * ((int)p[0] - (int)p[-off]));
    int a;

    a = (27 * w + 63) >> 7;
    p[-off]     = (uint8_t)VP8_CLAMP255((int)p[-off] + a);
    p[0]        = (uint8_t)VP8_CLAMP255((int)p[0] - a);
    a = (18 * w + 63) >> 7;
    p[-2 * off] = (uint8_t)VP8_CLAMP255((int)p[-2 * off] + a);
    p[ off]     = (uint8_t)VP8_CLAMP255((int)p[ off] - a);
    a = (9 * w + 63) >> 7;
    p[-3 * off] = (uint8_t)VP8_CLAMP255((int)p[-3 * off] + a);
    p[2 * off]  = (uint8_t)VP8_CLAMP255((int)p[2 * off] - a);
}

/* Filters `count` parallel edges. `off` is the distance from p0 to q0 - one
 * pixel for a vertical edge, the plane stride for a horizontal one - while
 * `step` walks from one edge to the next. `wide` selects the macroblock
 * filter, which reaches three pixels into each side. */
static void vp8_filter_edges(uint8_t* p, int off, int step, int count,
                             int edge, int interior, int hev, int wide) {
    int i;

    for (i = 0; i < count; ++i) {
        if (vp8_normal_ok(p, off, edge, interior)) {
            if (vp8_hev(p, off, hev))
                vp8_filter_narrow(p, off, 1);
            else if (wide)
                vp8_filter_wide(p, off);
            else
                vp8_filter_narrow(p, off, 0);
        }
        p += step;
    }
}

/* The simple filter of RFC 6386 15.2 always uses the outer taps. */
static void vp8_filter_edges_simple(uint8_t* p, int off, int step, int count,
                                    int limit) {
    int i;

    for (i = 0; i < count; ++i) {
        if (vp8_simple_ok(p, off, limit))
            vp8_filter_narrow(p, off, 1);
        p += step;
    }
}

/* Derives the three thresholds of RFC 6386 15.4 for one macroblock. The
 * returned filter level is zero when the macroblock must not be filtered. */
static int vp8_filter_params(const WebpVp8Dec* dec, const WebpMbInfo* mb,
                             int* interior_out, int* hev_out) {
    int level = dec->lf_level;
    int interior, hev;

    if (dec->seg_enabled)
        level = dec->seg_abs ? dec->seg_lf_level[mb->segment_id]
                             : level + dec->seg_lf_level[mb->segment_id];
    if (level > 63) level = 63;
    else if (level < 0) level = 0;

    if (dec->lf_delta_enabled) {
        /* a key frame only ever references the current frame, so the
         * reference adjustment is the one for slot zero */
        level += dec->lf_ref_delta[0];
        if (mb->y_mode == VP8_B_PRED)
            level += dec->lf_mode_delta[0];
        if (level > 63) level = 63;
        else if (level < 0) level = 0;
    }

    interior = level;
    if (dec->lf_sharpness != 0) {
        interior >>= (dec->lf_sharpness > 4) ? 2 : 1;
        if (interior > 9 - dec->lf_sharpness)
            interior = 9 - dec->lf_sharpness;
    }
    if (interior < 1)
        interior = 1;

    /* key frames only reach a threshold of two */
    hev = (level >= 15) ? 1 : 0;
    if (level >= 40)
        hev = 2;

    *interior_out = interior;
    *hev_out = hev;
    return level;
}

void webp_vp8_filter_row(const WebpVp8Dec* dec, int row,
                         const WebpMbInfo* mbs) {
    const int ystride = dec->y.stride;
    const int uvstride = dec->u.stride;
    uint8_t* yp;
    uint8_t* up;
    uint8_t* vp;
    int col;

    /* A header level of zero switches the filter off for the whole frame,
     * even where a segmentation or delta adjustment would have raised an
     * individual macroblock above zero. */
    if (dec->lf_level == 0)
        return;

    yp = dec->y.pixels + (size_t)row * 16 * ystride;
    up = dec->u.pixels + (size_t)row * 8 * uvstride;
    vp = dec->v.pixels + (size_t)row * 8 * uvstride;

    for (col = 0; col < dec->mb_cols; ++col) {
        int interior, hev;
        const int level = vp8_filter_params(dec, &mbs[col], &interior, &hev);

        if (level != 0) {
            /* B_PRED always splits its sub blocks, so the narrow filter has
             * to run over them even when the residue is empty */
            const int subblocks = (mbs[col].eob_mask != 0 ||
                                   mbs[col].y_mode == VP8_B_PRED);

            if (dec->lf_use_simple) {
                const int mb_limit = (level + 2) * 2 + interior;
                const int b_limit = level * 2 + interior;

                /* the simple filter is defined on luma only */
                if (col != 0)
                    vp8_filter_edges_simple(yp, 1, ystride, 16, mb_limit);
                if (subblocks) {
                    vp8_filter_edges_simple(yp + 4, 1, ystride, 16, b_limit);
                    vp8_filter_edges_simple(yp + 8, 1, ystride, 16, b_limit);
                    vp8_filter_edges_simple(yp + 12, 1, ystride, 16, b_limit);
                }
                if (row != 0)
                    vp8_filter_edges_simple(yp, ystride, 1, 16, mb_limit);
                if (subblocks) {
                    vp8_filter_edges_simple(yp + 4 * ystride, ystride, 1, 16, b_limit);
                    vp8_filter_edges_simple(yp + 8 * ystride, ystride, 1, 16, b_limit);
                    vp8_filter_edges_simple(yp + 12 * ystride, ystride, 1, 16, b_limit);
                }
            } else {
                if (col != 0) {
                    vp8_filter_edges(yp, 1, ystride, 16, level + 2, interior, hev, 1);
                    vp8_filter_edges(up, 1, uvstride, 8, level + 2, interior, hev, 1);
                    vp8_filter_edges(vp, 1, uvstride, 8, level + 2, interior, hev, 1);
                }
                if (subblocks) {
                    vp8_filter_edges(yp + 4, 1, ystride, 16, level, interior, hev, 0);
                    vp8_filter_edges(yp + 8, 1, ystride, 16, level, interior, hev, 0);
                    vp8_filter_edges(yp + 12, 1, ystride, 16, level, interior, hev, 0);
                    vp8_filter_edges(up + 4, 1, uvstride, 8, level, interior, hev, 0);
                    vp8_filter_edges(vp + 4, 1, uvstride, 8, level, interior, hev, 0);
                }
                if (row != 0) {
                    vp8_filter_edges(yp, ystride, 1, 16, level + 2, interior, hev, 1);
                    vp8_filter_edges(up, uvstride, 1, 8, level + 2, interior, hev, 1);
                    vp8_filter_edges(vp, uvstride, 1, 8, level + 2, interior, hev, 1);
                }
                if (subblocks) {
                    vp8_filter_edges(yp + 4 * ystride, ystride, 1, 16, level, interior, hev, 0);
                    vp8_filter_edges(yp + 8 * ystride, ystride, 1, 16, level, interior, hev, 0);
                    vp8_filter_edges(yp + 12 * ystride, ystride, 1, 16, level, interior, hev, 0);
                    vp8_filter_edges(up + 4 * uvstride, uvstride, 1, 8, level, interior, hev, 0);
                    vp8_filter_edges(vp + 4 * uvstride, uvstride, 1, 8, level, interior, hev, 0);
                }
            }
        }
        yp += 16;
        up += 8;
        vp += 8;
    }
}

/*---------------------------------------------------------------------------
 * YUV -> ARGB
 *-------------------------------------------------------------------------*/

/* The fixed point conversion libwebp uses, which is the BT.601 matrix
 * scaled so that eight bits of fraction survive the intermediate products:
 *   R = (19077*Y + 26149*V - 14234) >> 6
 *   G = (19077*Y -  6419*U - 13320*V +  8708) >> 6
 *   B = (19077*Y + 33050*U - 17685) >> 6
 * with the products taken as (v * coeff) >> 8. */
#define VP8_YUV_FIX2   6
#define VP8_YUV_MASK2  ((256 << VP8_YUV_FIX2) - 1)

static int vp8_mulhi(int v, int coeff) { return (v * coeff) >> 8; }

static int vp8_clip8(int v) {
    return ((v & ~VP8_YUV_MASK2) == 0) ? (v >> VP8_YUV_FIX2)
                                       : ((v < 0) ? 0 : 255);
}

void webp_vp8_to_argb(const WebpVp8Dec* dec, uint32_t* dst) {
    const uint8_t* yp = dec->y.pixels;
    int row;

    /* Chroma is 4:2:0 and is repeated over each 2x2 pixel group. A
     * smoothing upsampler would hide the blockiness of saturated edges but
     * the format does not require one. */
    for (row = 0; row < dec->height; ++row) {
        const uint8_t* urow = dec->u.pixels + (size_t)(row >> 1) * dec->u.stride;
        const uint8_t* vrow = dec->v.pixels + (size_t)(row >> 1) * dec->v.stride;
        uint32_t* out = dst + (size_t)row * dec->width;
        int col;

        for (col = 0; col < dec->width; ++col) {
            const int y = yp[col];
            const int u = urow[col >> 1];
            const int v = vrow[col >> 1];
            const int r = vp8_clip8(vp8_mulhi(y, 19077) + vp8_mulhi(v, 26149) - 14234);
            const int g = vp8_clip8(vp8_mulhi(y, 19077) - vp8_mulhi(u, 6419) -
                                    vp8_mulhi(v, 13320) + 8708);
            const int b = vp8_clip8(vp8_mulhi(y, 19077) + vp8_mulhi(u, 33050) - 17685);
            out[col] = WEBP_ARGB(255, r, g, b);
        }
        yp += dec->y.stride;
    }
}
