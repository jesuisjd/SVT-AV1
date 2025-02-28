/*
* Copyright(c) 2019 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

/*
* Copyright (c) 2016, Alliance for Open Media. All rights reserved
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at www.aomedia.org/license/software. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at www.aomedia.org/license/patent.
*/

#include <stdlib.h>
#include <string.h>

#include "EbEntropyCoding.h"
#include "EbEntropyCodingUtil.h"
#include "EbUtility.h"
#include "EbCodingUnit.h"
#include "EbSvtAv1ErrorCodes.h"
#include "EbTransforms.h"
#include "EbEntropyCodingProcess.h"
#include "EbSegmentation.h"

#include "aom_dsp_rtcd.h"

#define S32 32*32
#define S16 16*16
#define S8  8*8
#define S4  4*4

int32_t av1_loop_restoration_corners_in_sb(Av1Common *cm, int32_t plane,
    int32_t mi_row, int32_t mi_col, BlockSize bsize,
    int32_t *rcol0, int32_t *rcol1, int32_t *rrow0,
    int32_t *rrow1, int32_t *tile_tl_idx);

static INLINE int has_second_ref(const MbModeInfo *mbmi) {
    return mbmi->ref_frame[1] > INTRA_FRAME;
}

static INLINE int has_uni_comp_refs(const MbModeInfo *mbmi) {
    return has_second_ref(mbmi) && (!((mbmi->ref_frame[0] >= BWDREF_FRAME) ^
        (mbmi->ref_frame[1] >= BWDREF_FRAME)));
}
int32_t is_inter_block(const MbModeInfo *mbmi);
#define CHAR_BIT      8         /* number of bits in a char */
#if ADD_DELTA_QP_SUPPORT
#define OD_CLZ0 (1)
#define OD_CLZ(x) (-get_msb(x))
#define OD_ILOG_NZ(x) (OD_CLZ0 - OD_CLZ(x))
#endif

static INLINE int32_t is_comp_ref_allowed(BlockSize bsize) {
    return AOMMIN(block_size_wide[bsize], block_size_high[bsize]) >= 8;
}
int32_t av1_loop_restoration_corners_in_sb(Av1Common *cm, int32_t plane,
    int32_t mi_row, int32_t mi_col, BlockSize bsize,
    int32_t *rcol0, int32_t *rcol1, int32_t *rrow0,
    int32_t *rrow1, int32_t *tile_tl_idx);

/*****************************
* Enums
*****************************/

enum COEFF_SCAN_TYPE
{
    SCAN_ZIGZAG = 0,      // zigzag scan
    SCAN_HOR,             // first scan is horizontal
    SCAN_VER,             // first scan is vertical
    SCAN_DIAG             // up-right diagonal scan
};

extern void av1_set_ref_frame(MvReferenceFrame *rf,
    int8_t ref_frame_type);

/************************************************
* CABAC Encoder Constructor
************************************************/
void CabacCtor(
    CabacEncodeContext *cabacEncContextPtr)
{
    EB_MEMSET(cabacEncContextPtr, 0, sizeof(CabacEncodeContext));

    return;
}

static INLINE int32_t does_level_match(int32_t width, int32_t height, double fps,
    int32_t lvl_width, int32_t lvl_height,
    double lvl_fps, int32_t lvl_dim_mult) {
    const int64_t lvl_luma_pels = lvl_width * lvl_height;
    const double lvl_display_sample_rate = lvl_luma_pels * lvl_fps;
    const int64_t luma_pels = width * height;
    const double display_sample_rate = luma_pels * fps;
    return luma_pels <= lvl_luma_pels &&
        display_sample_rate <= lvl_display_sample_rate &&
        width <= lvl_width * lvl_dim_mult &&
        height <= lvl_height * lvl_dim_mult;
}

static void SetBitstreamLevelTier(SequenceControlSet *scs_ptr) {
    // TODO(any): This is a placeholder function that only addresses dimensions
    // and max display sample rates.
    // Need to add checks for max bit rate, max decoded luma sample rate, header
    // rate, etc. that are not covered by this function.

    BitstreamLevel bl = { 9, 3 };
    if (does_level_match(scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height, (scs_ptr->frame_rate >> 16), 512,
        288, 30.0, 4)) {
        bl.major = 2;
        bl.minor = 0;
    }
    else if (does_level_match(scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height, (scs_ptr->frame_rate >> 16),
        704, 396, 30.0, 4)) {
        bl.major = 2;
        bl.minor = 1;
    }
    else if (does_level_match(scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height, (scs_ptr->frame_rate >> 16),
        1088, 612, 30.0, 4)) {
        bl.major = 3;
        bl.minor = 0;
    }
    else if (does_level_match(scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height, (scs_ptr->frame_rate >> 16),
        1376, 774, 30.0, 4)) {
        bl.major = 3;
        bl.minor = 1;
    }
    else if (does_level_match(scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height, (scs_ptr->frame_rate >> 16),
        2048, 1152, 30.0, 3)) {
        bl.major = 4;
        bl.minor = 0;
    }
    else if (does_level_match(scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height, (scs_ptr->frame_rate >> 16),
        2048, 1152, 60.0, 3)) {
        bl.major = 4;
        bl.minor = 1;
    }
    else if (does_level_match(scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height, (scs_ptr->frame_rate >> 16),
        4096, 2176, 30.0, 2)) {
        bl.major = 5;
        bl.minor = 0;
    }
    else if (does_level_match(scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height, (scs_ptr->frame_rate >> 16),
        4096, 2176, 60.0, 2)) {
        bl.major = 5;
        bl.minor = 1;
    }
    else if (does_level_match(scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height, (scs_ptr->frame_rate >> 16),
        4096, 2176, 120.0, 2)) {
        bl.major = 5;
        bl.minor = 2;
    }
    else if (does_level_match(scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height, (scs_ptr->frame_rate >> 16),
        8192, 4352, 30.0, 2)) {
        bl.major = 6;
        bl.minor = 0;
    }
    else if (does_level_match(scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height, (scs_ptr->frame_rate >> 16),
        8192, 4352, 60.0, 2)) {
        bl.major = 6;
        bl.minor = 1;
    }
    else if (does_level_match(scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height, (scs_ptr->frame_rate >> 16),
        8192, 4352, 120.0, 2)) {
        bl.major = 6;
        bl.minor = 2;
    }
    else if (does_level_match(scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height, (scs_ptr->frame_rate >> 16),
        16384, 8704, 30.0, 2)) {
        bl.major = 7;
        bl.minor = 0;
    }
    else if (does_level_match(scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height, (scs_ptr->frame_rate >> 16),
        16384, 8704, 60.0, 2)) {
        bl.major = 7;
        bl.minor = 1;
    }
    else if (does_level_match(scs_ptr->seq_header.max_frame_width, scs_ptr->seq_header.max_frame_height, (scs_ptr->frame_rate >> 16),
        16384, 8704, 120.0, 2)) {
        bl.major = 7;
        bl.minor = 2;
    }
    for (int32_t i = 0; i < MAX_NUM_OPERATING_POINTS; ++i) {
        scs_ptr->level[i] = bl;
        scs_ptr->seq_header.operating_point[i].seq_tier = 0;  // setting main tier by default
    }
}

const uint8_t KEobOffsetBits[12] = { 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };

static void WriteGolomb(AomWriter *w, int32_t level) {
    int32_t x = level + 1;
    int32_t i = x;
    int32_t length = 0;

    while (i) {
        i >>= 1;
        ++length;
    }
    assert(length > 0);

    for (i = 0; i < length - 1; ++i) aom_write_bit(w, 0);

    for (i = length - 1; i >= 0; --i) aom_write_bit(w, (x >> i) & 0x01);
}

static const uint8_t EobToPosSmall[33] = {
    0, 1, 2,                                        // 0-2
    3, 3,                                           // 3-4
    4, 4, 4, 4,                                     // 5-8
    5, 5, 5, 5, 5, 5, 5, 5,                         // 9-16
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6  // 17-32
};

static const uint8_t EobToPosLarge[17] = {
    6,                               // place holder
    7,                               // 33-64
    8, 8,                           // 65-128
    9, 9, 9, 9,                   // 129-256
    10, 10, 10, 10, 10, 10, 10, 10,  // 257-512
    11                               // 513-
};
const int16_t KEobGroupStart[12] = { 0,  1,  2,  3,   5,   9,
                                        17, 33, 65, 129, 257, 513 };

static INLINE int16_t GetEobPosToken(const int16_t eob, int16_t *const extra) {
    int16_t t;

    if (eob < 33)
        t = EobToPosSmall[eob];
    else {
        const int16_t e = MIN((eob - 1) >> 5, 16);
        t = EobToPosLarge[e];
    }

    *extra = eob - KEobGroupStart[t];

    return t;
}

static INLINE uint8_t *SetLevels(uint8_t *const levelsBuf, const int32_t width) {
    return levelsBuf + TX_PAD_TOP * (width + TX_PAD_HOR);
}

static INLINE void av1TxbInitLevels(
    int32_t         *coeffBufferPtr,
    const uint32_t    coeff_stride,
    const int16_t    width,
    const int16_t    height,
    uint8_t *const    levels) {
    const int16_t stride = width + TX_PAD_HOR;
    uint8_t *ls = levels;

    memset(levels - TX_PAD_TOP * stride, 0,
        sizeof(*levels) * TX_PAD_TOP * stride);
    memset(levels + stride * height, 0,
        sizeof(*levels) * (TX_PAD_BOTTOM * stride + TX_PAD_END));

    for (int16_t i = 0; i < height; i++) {
        for (int16_t j = 0; j < width; j++)
            *ls++ = (uint8_t)CLIP3(0, INT8_MAX, abs(coeffBufferPtr[i * coeff_stride + j]));
        for (int16_t j = 0; j < TX_PAD_HOR; j++)
            *ls++ = 0;
    }
}

/************************************************************************************************/
// blockd.h

static INLINE int32_t get_txb_wide(TxSize tx_size) {
    tx_size = av1_get_adjusted_tx_size(tx_size);
    assert(tx_size < TX_SIZES_ALL);
    return tx_size_wide[tx_size];
}
static INLINE int32_t get_txb_high(TxSize tx_size) {
    tx_size = av1_get_adjusted_tx_size(tx_size);
    assert(tx_size < TX_SIZES_ALL);
    return tx_size_high[tx_size];
}
static INLINE int32_t get_txb_bwl(TxSize tx_size) {
    tx_size = av1_get_adjusted_tx_size(tx_size);
    assert(tx_size < TX_SIZES_ALL);
    return tx_size_wide_log2[tx_size];
}

static INLINE int16_t GetBrCtx(
    const uint8_t *const levels,
    const int16_t c,  // raster order
    const int16_t bwl,
    const TxType tx_type
) {
    const int16_t row = c >> bwl;
    const int16_t col = c - (row << bwl);
    const int16_t stride = (1 << bwl) + TX_PAD_HOR;
    const TxClass tx_class = tx_type_to_class[tx_type];
    const int16_t pos = row * stride + col;
    int16_t mag = levels[pos + 1];
    mag += levels[pos + stride];
    switch (tx_class) {
    case TX_CLASS_2D:
        mag += levels[pos + stride + 1];
        mag = MIN((mag + 1) >> 1, 6);
        if (c == 0) return mag;
        if ((row < 2) && (col < 2)) return mag + 7;
        break;

    case TX_CLASS_HORIZ:
        mag += levels[pos + 2];
        mag = AOMMIN((mag + 1) >> 1, 6);
        if (c == 0) return mag;
        if (col == 0) return mag + 7;
        break;
    case TX_CLASS_VERT:
        mag += levels[pos + (stride << 1)];
        mag = AOMMIN((mag + 1) >> 1, 6);
        if (c == 0) return mag;
        if (row == 0) return mag + 7;
        break;

    default: break;
    }

    return mag + 14;
}

void get_txb_ctx(
#if INCOMPLETE_SB_FIX
    SequenceControlSet *sequence_control_set_ptr,
#endif
    const int32_t        plane,
    NeighborArrayUnit   *dc_sign_level_coeff_neighbor_array,
    uint32_t             cu_origin_x,
    uint32_t             cu_origin_y,
    const BlockSize      plane_bsize,
    const TxSize         tx_size,
    int16_t *const       txb_skip_ctx,
    int16_t *const       dc_sign_ctx) {
    uint32_t dcSignLevelCoeffLeftNeighborIndex = get_neighbor_array_unit_left_index(
        dc_sign_level_coeff_neighbor_array,
        cu_origin_y);
    uint32_t dcSignLevelCoeffTopNeighborIndex = get_neighbor_array_unit_top_index(
        dc_sign_level_coeff_neighbor_array,
        cu_origin_x);

#define MAX_TX_SIZE_UNIT 16
    static const int8_t signs[3] = { 0, -1, 1 };
#if INCOMPLETE_SB_FIX
    int32_t txb_w_unit;
    int32_t txb_h_unit;
    if (plane) {
        txb_w_unit = MIN(tx_size_wide_unit[tx_size], (int32_t)(sequence_control_set_ptr->seq_header.max_frame_width / 2 - cu_origin_x) >> 2);
        txb_h_unit = MIN(tx_size_high_unit[tx_size], (int32_t)(sequence_control_set_ptr->seq_header.max_frame_height / 2 - cu_origin_y) >> 2);
    }
    else {
        txb_w_unit = MIN(tx_size_wide_unit[tx_size], (int32_t)(sequence_control_set_ptr->seq_header.max_frame_width - cu_origin_x) >> 2);
        txb_h_unit = MIN(tx_size_high_unit[tx_size], (int32_t)(sequence_control_set_ptr->seq_header.max_frame_height - cu_origin_y) >> 2);
    }
#else
    const int32_t txb_w_unit = tx_size_wide_unit[tx_size];
    const int32_t txb_h_unit = tx_size_high_unit[tx_size];
#endif
    int16_t dc_sign = 0;
    uint16_t k = 0;

    uint8_t sign;

    if (dc_sign_level_coeff_neighbor_array->top_array[dcSignLevelCoeffTopNeighborIndex] != INVALID_NEIGHBOR_DATA){
        do {
            sign = ((uint8_t)dc_sign_level_coeff_neighbor_array->top_array[k + dcSignLevelCoeffTopNeighborIndex] >> COEFF_CONTEXT_BITS);
            assert(sign <= 2);
            dc_sign += signs[sign];
        } while (++k < txb_w_unit);
    }

    if (dc_sign_level_coeff_neighbor_array->left_array[dcSignLevelCoeffLeftNeighborIndex] != INVALID_NEIGHBOR_DATA){
        k = 0;
        do {
            sign = ((uint8_t)dc_sign_level_coeff_neighbor_array->left_array[k + dcSignLevelCoeffLeftNeighborIndex] >> COEFF_CONTEXT_BITS);
            assert(sign <= 2);
            dc_sign += signs[sign];
        } while (++k < txb_h_unit);
    }

    if (dc_sign > 0)
        *dc_sign_ctx = 2;
    else if (dc_sign < 0)
        *dc_sign_ctx = 1;
    else
        *dc_sign_ctx = 0;

    if (plane == 0) {
        if (plane_bsize == txsize_to_bsize[tx_size])
            *txb_skip_ctx = 0;
        else {
            static const uint8_t skip_contexts[5][5] = { { 1, 2, 2, 2, 3 },
            { 1, 4, 4, 4, 5 },
            { 1, 4, 4, 4, 5 },
            { 1, 4, 4, 4, 5 },
            { 1, 4, 4, 4, 6 } };
            int32_t top = 0;
            int32_t left = 0;

            k = 0;
            if (dc_sign_level_coeff_neighbor_array->top_array[dcSignLevelCoeffTopNeighborIndex] != INVALID_NEIGHBOR_DATA) {
                do {
                    top |= (int32_t)(dc_sign_level_coeff_neighbor_array->top_array[k + dcSignLevelCoeffTopNeighborIndex]);
                } while (++k < txb_w_unit);
            }
            top &= COEFF_CONTEXT_MASK;

            if (dc_sign_level_coeff_neighbor_array->left_array[dcSignLevelCoeffLeftNeighborIndex] != INVALID_NEIGHBOR_DATA) {
                k = 0;
                do {
                    left |= (int32_t)(dc_sign_level_coeff_neighbor_array->left_array[k + dcSignLevelCoeffLeftNeighborIndex]);
                } while (++k < txb_h_unit);
            }
            left &= COEFF_CONTEXT_MASK;
            //do {
            //    top |= a[k];
            //} while (++k < txb_w_unit);
            //top &= COEFF_CONTEXT_MASK;

            //k = 0;
            //do {
            //    left |= l[k];
            //} while (++k < txb_h_unit);
            //left &= COEFF_CONTEXT_MASK;
            const int32_t max = AOMMIN(top | left, 4);
            const int32_t min = AOMMIN(AOMMIN(top, left), 4);

            *txb_skip_ctx = skip_contexts[min][max];
        }
    }
    else {
        //const int32_t ctx_base = get_entropy_context(tx_size, a, l);
        int16_t ctx_base_left = 0;
        int16_t ctx_base_top = 0;

        if (dc_sign_level_coeff_neighbor_array->top_array[dcSignLevelCoeffTopNeighborIndex] != INVALID_NEIGHBOR_DATA) {
            k = 0;
            do {
                ctx_base_top += (dc_sign_level_coeff_neighbor_array->top_array[k + dcSignLevelCoeffTopNeighborIndex] != 0);
            } while (++k < txb_w_unit);
        }
        if (dc_sign_level_coeff_neighbor_array->left_array[dcSignLevelCoeffLeftNeighborIndex] != INVALID_NEIGHBOR_DATA) {
            k = 0;
            do {
                ctx_base_left += (dc_sign_level_coeff_neighbor_array->left_array[k + dcSignLevelCoeffLeftNeighborIndex] != 0);
            } while (++k < txb_h_unit);
        }
        const int32_t ctx_base = ((ctx_base_left != 0) + (ctx_base_top != 0));
        const int32_t ctx_offset = (num_pels_log2_lookup[plane_bsize] >
            num_pels_log2_lookup[txsize_to_bsize[tx_size]])
            ? 10
            : 7;
        *txb_skip_ctx = (int16_t)(ctx_base + ctx_offset);
    }
#undef MAX_TX_SIZE_UNIT
}

void Av1WriteTxType(
    PictureParentControlSet   *pcs_ptr,
    FRAME_CONTEXT               *frameContext,
    AomWriter                  *ec_writer,
    CodingUnit                *cu_ptr,
    uint32_t                      intraDir,
    TxType                     txType,
    TxSize                      txSize) {

    FrameHeader *frm_hdr = &pcs_ptr->frm_hdr;
    const int32_t isInter = cu_ptr->av1xd->use_intrabc || (cu_ptr->prediction_mode_flag == INTER_MODE);

    if (get_ext_tx_types(txSize, isInter, frm_hdr->reduced_tx_set) > 1 &&
        (frm_hdr->quantization_params.base_q_idx > 0)) {
        const TxSize squareTxSize = txsize_sqr_map[txSize];
        assert(squareTxSize <= EXT_TX_SIZES);

        const TxSetType txSetType =
            get_ext_tx_set_type(txSize, isInter, frm_hdr->reduced_tx_set);
        const int32_t eset = get_ext_tx_set(txSize, isInter, frm_hdr->reduced_tx_set);
        // eset == 0 should correspond to a set with only DCT_DCT and there
        // is no need to send the tx_type
        assert(eset > 0);
        assert(av1_ext_tx_used[txSetType][txType]);
        if (isInter) {
            aom_write_symbol(ec_writer, av1_ext_tx_ind[txSetType][txType],
                frameContext->inter_ext_tx_cdf[eset][squareTxSize],
                av1_num_ext_tx_set[txSetType]);
        }
        else {
            aom_write_symbol(
                ec_writer, av1_ext_tx_ind[txSetType][txType],
                frameContext->intra_ext_tx_cdf[eset][squareTxSize][intraDir],
                av1_num_ext_tx_set[txSetType]);
        }
    }
}

static INLINE void set_dc_sign(int32_t *cul_level, int32_t dc_val) {
    if (dc_val < 0)
        *cul_level |= 1 << COEFF_CONTEXT_BITS;
    else if (dc_val > 0)
        *cul_level += 2 << COEFF_CONTEXT_BITS;
}

int32_t  Av1WriteCoeffsTxb1D(
    PictureParentControlSet   *parent_pcs_ptr,
    FRAME_CONTEXT               *frameContext,
    AomWriter                  *ec_writer,
    CodingUnit                *cu_ptr,
    TxSize                     txSize,
    uint32_t                       pu_index,
    uint32_t                       tu_index,
    uint32_t                       intraLumaDir,
    int32_t                     *coeffBufferPtr,
    const uint16_t                coeff_stride,
    COMPONENT_TYPE              component_type,
    int16_t                      txb_skip_ctx,
    int16_t                      dcSignCtx,
    int16_t                      eob)
{
    (void)pu_index;
    (void)coeff_stride;
    const TxSize txs_ctx = (TxSize)((txsize_sqr_map[txSize] + txsize_sqr_up_map[txSize] + 1) >> 1);
    TxType txType = cu_ptr->transform_unit_array[tu_index].transform_type[component_type];
    const ScanOrder *const scan_order = &av1_scan_orders[txSize][txType];
    const int16_t *const scan = scan_order->scan;
    int32_t c;
    const int16_t bwl = (const uint16_t)get_txb_bwl(txSize);
    const uint16_t width = (const uint16_t)get_txb_wide(txSize);
    const uint16_t height = (const uint16_t)get_txb_high(txSize);

    uint8_t levelsBuf[TX_PAD_2D];
    uint8_t *const levels = SetLevels(levelsBuf, width);
    DECLARE_ALIGNED(16, int8_t, coeffContexts[MAX_TX_SQUARE]);

    assert(txs_ctx < TX_SIZES);

    aom_write_symbol(ec_writer, eob == 0,
        frameContext->txb_skip_cdf[txs_ctx][txb_skip_ctx], 2);

    if (component_type == 0 && eob == 0) {
        // INTER. Chroma follows Luma in transform type
        if (cu_ptr->prediction_mode_flag == INTER_MODE) {
            txType = cu_ptr->transform_unit_array[tu_index].transform_type[PLANE_TYPE_Y] = DCT_DCT;
            cu_ptr->transform_unit_array[0].transform_type[PLANE_TYPE_UV] = DCT_DCT;
        }
        else { // INTRA
            txType = cu_ptr->transform_unit_array[tu_index].transform_type[PLANE_TYPE_Y] = DCT_DCT;
        }
        assert(txType == DCT_DCT);
    }
    if (eob == 0) return 0;

    av1TxbInitLevels(
        coeffBufferPtr,
        width,
        width,
        height,
        levels);

    if (component_type == COMPONENT_LUMA) {
        Av1WriteTxType(
            parent_pcs_ptr,
            frameContext,
            ec_writer,
            cu_ptr,
            intraLumaDir,
            txType,
            txSize);
    }

    int16_t eobExtra;
    const int16_t eobPt = GetEobPosToken(eob, &eobExtra);
    const int16_t eobMultiSize = txsize_log2_minus4[txSize];
    const int16_t eobMultiCtx = (tx_type_to_class[txType] == TX_CLASS_2D) ? 0 : 1;
    switch (eobMultiSize) {
    case 0:
        aom_write_symbol(ec_writer, eobPt - 1,
            frameContext->eob_flag_cdf16[component_type][eobMultiCtx], 5);
        break;
    case 1:
        aom_write_symbol(ec_writer, eobPt - 1,
            frameContext->eob_flag_cdf32[component_type][eobMultiCtx], 6);
        break;
    case 2:
        aom_write_symbol(ec_writer, eobPt - 1,
            frameContext->eob_flag_cdf64[component_type][eobMultiCtx], 7);
        break;
    case 3:
        aom_write_symbol(ec_writer, eobPt - 1,
            frameContext->eob_flag_cdf128[component_type][eobMultiCtx], 8);
        break;
    case 4:
        aom_write_symbol(ec_writer, eobPt - 1,
            frameContext->eob_flag_cdf256[component_type][eobMultiCtx], 9);
        break;
    case 5:
        aom_write_symbol(ec_writer, eobPt - 1,
            frameContext->eob_flag_cdf512[component_type][eobMultiCtx], 10);
        break;
    default:
        aom_write_symbol(ec_writer, eobPt - 1,
            frameContext->eob_flag_cdf1024[component_type][eobMultiCtx], 11);
        break;
    }

    uint8_t eobOffsetBits = KEobOffsetBits[eobPt];
    if (eobOffsetBits > 0) {
        int32_t eobShift = eobOffsetBits - 1;
        int32_t bit = (eobExtra & (1 << eobShift)) ? 1 : 0;
        aom_write_symbol(ec_writer, bit, frameContext->eob_extra_cdf[txs_ctx][component_type][eobPt], 2);
        for (int32_t i = 1; i < eobOffsetBits; i++) {
            eobShift = eobOffsetBits - 1 - i;
            bit = (eobExtra & (1 << eobShift)) ? 1 : 0;
            aom_write_bit(ec_writer, bit);
        }
    }

    av1_get_nz_map_contexts(levels, scan, eob, txSize, tx_type_to_class[txType], coeffContexts);

    for (c = eob - 1; c >= 0; --c) {
        const int16_t pos = scan[c];
        const int32_t v = coeffBufferPtr[pos];
        const int16_t coeffCtx = coeffContexts[pos];
        int32_t level = ABS(v);

        if (c == eob - 1) {
            aom_write_symbol(
                ec_writer, AOMMIN(level, 3) - 1,
                frameContext->coeff_base_eob_cdf[txs_ctx][component_type][coeffCtx], 3);
        }
        else {
            aom_write_symbol(ec_writer, AOMMIN(level, 3),
                frameContext->coeff_base_cdf[txs_ctx][component_type][coeffCtx],
                4);
        }
        if (level > NUM_BASE_LEVELS) {
            // level is above 1.
            int32_t base_range = level - 1 - NUM_BASE_LEVELS;
            int16_t brCtx = GetBrCtx(levels, pos, bwl, txType);

            for (int32_t idx = 0; idx < COEFF_BASE_RANGE; idx += BR_CDF_SIZE - 1) {
                const int32_t k = AOMMIN(base_range - idx, BR_CDF_SIZE - 1);
                aom_write_symbol(
                    ec_writer, k,
                    frameContext->coeff_br_cdf[AOMMIN(txs_ctx, TX_32X32)][component_type][brCtx],
                    BR_CDF_SIZE);
                if (k < BR_CDF_SIZE - 1) break;
            }
        }
    }
    // Loop to code all signs in the transform block,
    // starting with the sign of DC (if applicable)

    int32_t cul_level = 0;
    for (c = 0; c < eob; ++c) {
        const int16_t pos = scan[c];
        const int32_t v = coeffBufferPtr[pos];
        int32_t level = ABS(v);
        cul_level += level;

        const int32_t sign = (v < 0) ? 1 : 0;
        if (level) {
            if (c == 0) {
                aom_write_symbol(
                    ec_writer, sign, frameContext->dc_sign_cdf[component_type][dcSignCtx], 2);
            }
            else
                aom_write_bit(ec_writer, sign);
            if (level > COEFF_BASE_RANGE + NUM_BASE_LEVELS) {
                WriteGolomb(ec_writer,
                    level - COEFF_BASE_RANGE - 1 - NUM_BASE_LEVELS);
            }
        }
    }

    cul_level = AOMMIN(COEFF_CONTEXT_MASK, cul_level);
    // DC value
    set_dc_sign(&cul_level, coeffBufferPtr[0]);
    return cul_level;
}

static EbErrorType av1_encode_tx_coef_y(
    PictureControlSet     *pcs_ptr,
    EntropyCodingContext  *context_ptr,
    FRAME_CONTEXT         *frameContext,
    AomWriter             *ec_writer,
    CodingUnit            *cu_ptr,
    uint32_t               cu_origin_x,
    uint32_t               cu_origin_y,
    uint32_t               intraLumaDir,
    BlockSize              plane_bsize,
    EbPictureBufferDesc   *coeff_ptr,
    NeighborArrayUnit     *luma_dc_sign_level_coeff_neighbor_array)
{
    EbErrorType return_error = EB_ErrorNone;

    const BlockGeom *blk_geom = get_blk_geom_mds(cu_ptr->mds_idx);
    int32_t cul_level_y = 0;

    uint8_t  tx_depth = cu_ptr->tx_depth;
    uint16_t txb_count = blk_geom->txb_count[cu_ptr->tx_depth];
    uint8_t  txb_itr = 0;

    uint8_t tx_index;
    for (tx_index = 0; tx_index < txb_count; tx_index++) {
        txb_itr = tx_index;

        const TxSize tx_size = blk_geom->txsize[tx_depth][txb_itr];

        int32_t *coeff_buffer;

        const uint32_t coeff1dOffset = context_ptr->coded_area_sb;

        coeff_buffer = (int32_t*)coeff_ptr->buffer_y + coeff1dOffset;

        {
            int16_t txb_skip_ctx = 0;
            int16_t dcSignCtx = 0;

            get_txb_ctx(
#if INCOMPLETE_SB_FIX
                pcs_ptr->parent_pcs_ptr->sequence_control_set_ptr,
#endif
                COMPONENT_LUMA,
                luma_dc_sign_level_coeff_neighbor_array,
                cu_origin_x + blk_geom->tx_org_x[tx_depth][txb_itr] - blk_geom->origin_x,
                cu_origin_y + blk_geom->tx_org_y[tx_depth][txb_itr] - blk_geom->origin_y,
                plane_bsize,
                tx_size,
                &txb_skip_ctx,
                &dcSignCtx);

            cul_level_y =
                Av1WriteCoeffsTxb1D(
                    pcs_ptr->parent_pcs_ptr,
                    frameContext,
                    ec_writer,
                    cu_ptr,
                    tx_size,
                    0,
                    txb_itr,
                    intraLumaDir,
                    coeff_buffer,
                    coeff_ptr->stride_y,
                    COMPONENT_LUMA,
                    txb_skip_ctx,
                    dcSignCtx,
                    cu_ptr->transform_unit_array[txb_itr].nz_coef_count[0]);
        }

        // Update the luma Dc Sign Level Coeff Neighbor Array
        {
            uint8_t dc_sign_level_coeff = (uint8_t)cul_level_y;

            neighbor_array_unit_mode_write(
                luma_dc_sign_level_coeff_neighbor_array,
                (uint8_t*)&dc_sign_level_coeff,
                cu_origin_x + blk_geom->tx_org_x[tx_depth][txb_itr] - blk_geom->origin_x,
                cu_origin_y + blk_geom->tx_org_y[tx_depth][txb_itr] - blk_geom->origin_y,
                blk_geom->tx_width[tx_depth][txb_itr],
                blk_geom->tx_height[tx_depth][txb_itr],
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
        }

        context_ptr->coded_area_sb += blk_geom->tx_width[tx_depth][txb_itr] * blk_geom->tx_height[tx_depth][txb_itr];
    }

    return return_error;
}
static EbErrorType av1_encode_tx_coef_uv(
    PictureControlSet     *pcs_ptr,
    EntropyCodingContext  *context_ptr,
    FRAME_CONTEXT         *frameContext,
    AomWriter             *ec_writer,
    CodingUnit            *cu_ptr,
    uint32_t               cu_origin_x,
    uint32_t               cu_origin_y,
    uint32_t               intraLumaDir,
    EbPictureBufferDesc   *coeff_ptr,
    NeighborArrayUnit     *cr_dc_sign_level_coeff_neighbor_array,
    NeighborArrayUnit     *cb_dc_sign_level_coeff_neighbor_array)
{
    EbErrorType return_error = EB_ErrorNone;

    const BlockGeom *blk_geom = get_blk_geom_mds(cu_ptr->mds_idx);

    if (!blk_geom->has_uv) return return_error;

    int32_t cul_level_cb = 0, cul_level_cr = 0;

    uint8_t tx_depth   = cu_ptr->tx_depth;
    uint16_t txb_count = 1;
    uint8_t txb_itr    = 0;

    uint8_t tx_index;

    for (tx_index = 0; tx_index < txb_count; tx_index++) {
        txb_itr = tx_index;

        const TxSize chroma_tx_size = blk_geom->txsize_uv[tx_depth][txb_itr];

        int32_t *coeff_buffer;

        const uint32_t coeff1dOffset = context_ptr->coded_area_sb;

        coeff_buffer = (int32_t*)coeff_ptr->buffer_y + coeff1dOffset;

        if (blk_geom->has_uv) {
            // cb
            coeff_buffer = (int32_t*)coeff_ptr->buffer_cb + context_ptr->coded_area_sb_uv;
            {
                int16_t txb_skip_ctx = 0;
                int16_t dcSignCtx = 0;

                get_txb_ctx(
#if INCOMPLETE_SB_FIX
                    pcs_ptr->parent_pcs_ptr->sequence_control_set_ptr,
#endif
                    COMPONENT_CHROMA,
                    cb_dc_sign_level_coeff_neighbor_array,
                    ROUND_UV(cu_origin_x + blk_geom->tx_org_x[tx_depth][txb_itr] - blk_geom->origin_x) >> 1,
                    ROUND_UV(cu_origin_y + blk_geom->tx_org_y[tx_depth][txb_itr] - blk_geom->origin_y) >> 1,
                    blk_geom->bsize_uv,
                    chroma_tx_size,
                    &txb_skip_ctx,
                    &dcSignCtx);

                cul_level_cb =
                    Av1WriteCoeffsTxb1D(
                        pcs_ptr->parent_pcs_ptr,
                        frameContext,
                        ec_writer,
                        cu_ptr,
                        chroma_tx_size,
                        0,
                        txb_itr,
                        intraLumaDir,
                        coeff_buffer,
                        coeff_ptr->stride_cb,
                        COMPONENT_CHROMA,
                        txb_skip_ctx,
                        dcSignCtx,
                        cu_ptr->transform_unit_array[txb_itr].nz_coef_count[1]);
            }

            // cr
            coeff_buffer = (int32_t*)coeff_ptr->buffer_cr + context_ptr->coded_area_sb_uv;
            {
                int16_t txb_skip_ctx = 0;
                int16_t dcSignCtx = 0;

                get_txb_ctx(
#if INCOMPLETE_SB_FIX
                    pcs_ptr->parent_pcs_ptr->sequence_control_set_ptr,
#endif
                    COMPONENT_CHROMA,
                    cr_dc_sign_level_coeff_neighbor_array,
                    ROUND_UV(cu_origin_x + blk_geom->tx_org_x[tx_depth][txb_itr] - blk_geom->origin_x) >> 1,
                    ROUND_UV(cu_origin_y + blk_geom->tx_org_y[tx_depth][txb_itr] - blk_geom->origin_y) >> 1,
                    blk_geom->bsize_uv,
                    chroma_tx_size,
                    &txb_skip_ctx,
                    &dcSignCtx);

                cul_level_cr =
                    Av1WriteCoeffsTxb1D(
                        pcs_ptr->parent_pcs_ptr,
                        frameContext,
                        ec_writer,
                        cu_ptr,
                        chroma_tx_size,
                        0,
                        txb_itr,
                        intraLumaDir,
                        coeff_buffer,
                        coeff_ptr->stride_cr,
                        COMPONENT_CHROMA,
                        txb_skip_ctx,
                        dcSignCtx,
                        cu_ptr->transform_unit_array[txb_itr].nz_coef_count[2]);
            }
        }

        // Update the cb Dc Sign Level Coeff Neighbor Array

        if (blk_geom->has_uv)
        {
            uint8_t dc_sign_level_coeff = (uint8_t)cul_level_cb;
            neighbor_array_unit_mode_write(
                cb_dc_sign_level_coeff_neighbor_array,
                (uint8_t*)&dc_sign_level_coeff,
                ROUND_UV(cu_origin_x + blk_geom->tx_org_x[tx_depth][txb_itr] - blk_geom->origin_x) >> 1,
                ROUND_UV(cu_origin_y + blk_geom->tx_org_y[tx_depth][txb_itr] - blk_geom->origin_y) >> 1,
                blk_geom->tx_width_uv[tx_depth][txb_itr],
                blk_geom->tx_height_uv[tx_depth][txb_itr],
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
        }

        if (blk_geom->has_uv)
            // Update the cr DC Sign Level Coeff Neighbor Array
        {
            uint8_t dc_sign_level_coeff = (uint8_t)cul_level_cr;
            neighbor_array_unit_mode_write(
                cr_dc_sign_level_coeff_neighbor_array,
                (uint8_t*)&dc_sign_level_coeff,
                ROUND_UV(cu_origin_x + blk_geom->tx_org_x[tx_depth][txb_itr] - blk_geom->origin_x) >> 1,
                ROUND_UV(cu_origin_y + blk_geom->tx_org_y[tx_depth][txb_itr] - blk_geom->origin_y) >> 1,
                blk_geom->tx_width_uv[tx_depth][txb_itr],
                blk_geom->tx_height_uv[tx_depth][txb_itr],
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
        }

        context_ptr->coded_area_sb_uv += blk_geom->tx_width_uv[tx_depth][txb_itr] * blk_geom->tx_height_uv[tx_depth][txb_itr];
    }

    return return_error;
}
/************************************
******* Av1EncodeTuCoeff
**************************************/
static EbErrorType Av1EncodeCoeff1D(
    PictureControlSet    *pcs_ptr,
    EntropyCodingContext *context_ptr,
    FRAME_CONTEXT        *frameContext,
    AomWriter            *ec_writer,
    CodingUnit           *cu_ptr,
    uint32_t              cu_origin_x,
    uint32_t              cu_origin_y,
    uint32_t              intraLumaDir,
    BlockSize             plane_bsize,
    EbPictureBufferDesc  *coeff_ptr,
    NeighborArrayUnit     *luma_dc_sign_level_coeff_neighbor_array,
    NeighborArrayUnit     *cr_dc_sign_level_coeff_neighbor_array,
    NeighborArrayUnit     *cb_dc_sign_level_coeff_neighbor_array)
{
    EbErrorType return_error = EB_ErrorNone;

    if (cu_ptr->prediction_mode_flag == INTRA_MODE && cu_ptr->tx_depth) {
        av1_encode_tx_coef_y(
            pcs_ptr,
            context_ptr,
            frameContext,
            ec_writer,
            cu_ptr,
            cu_origin_x,
            cu_origin_y,
            intraLumaDir,
            plane_bsize,
            coeff_ptr,
            luma_dc_sign_level_coeff_neighbor_array);

        av1_encode_tx_coef_uv(
            pcs_ptr,
            context_ptr,
            frameContext,
            ec_writer,
            cu_ptr,
            cu_origin_x,
            cu_origin_y,
            intraLumaDir,
            coeff_ptr,
            cr_dc_sign_level_coeff_neighbor_array,
            cb_dc_sign_level_coeff_neighbor_array);
    } else {
        // Transform partitioning free patch (except the 128x128 case)
        const BlockGeom *blk_geom = get_blk_geom_mds(cu_ptr->mds_idx);
        int32_t cul_level_y, cul_level_cb = 0, cul_level_cr = 0;

        uint16_t txb_count = blk_geom->txb_count[cu_ptr->tx_depth];
        uint8_t txb_itr = 0;

        for (txb_itr = 0; txb_itr < txb_count; txb_itr++) {
            const TxSize tx_size = blk_geom->txsize[cu_ptr->tx_depth][txb_itr];
            const TxSize chroma_tx_size = blk_geom->txsize_uv[cu_ptr->tx_depth][txb_itr];
            int32_t *coeff_buffer;

            const uint32_t coeff1dOffset = context_ptr->coded_area_sb;

            coeff_buffer = (int32_t*)coeff_ptr->buffer_y + coeff1dOffset;

            {
                int16_t txb_skip_ctx = 0;
                int16_t dcSignCtx = 0;

                get_txb_ctx(
#if INCOMPLETE_SB_FIX
                    pcs_ptr->parent_pcs_ptr->sequence_control_set_ptr,
#endif
                    COMPONENT_LUMA,
                    luma_dc_sign_level_coeff_neighbor_array,
                    cu_origin_x + blk_geom->tx_org_x[cu_ptr->tx_depth][txb_itr] - blk_geom->origin_x,
                    cu_origin_y + blk_geom->tx_org_y[cu_ptr->tx_depth][txb_itr] - blk_geom->origin_y,
                    plane_bsize,
                    tx_size,
                    &txb_skip_ctx,
                    &dcSignCtx);

                cul_level_y =
                    Av1WriteCoeffsTxb1D(
                        pcs_ptr->parent_pcs_ptr,
                        frameContext,
                        ec_writer,
                        cu_ptr,
                        tx_size,
                        0,
                        txb_itr,
                        intraLumaDir,
                        coeff_buffer,
                        coeff_ptr->stride_y,
                        COMPONENT_LUMA,
                        txb_skip_ctx,
                        dcSignCtx,
                        cu_ptr->transform_unit_array[txb_itr].nz_coef_count[0]);
            }

            if (blk_geom->has_uv) {
                // cb
                coeff_buffer = (int32_t*)coeff_ptr->buffer_cb + context_ptr->coded_area_sb_uv;
                {
                    int16_t txb_skip_ctx = 0;
                    int16_t dcSignCtx = 0;

                    get_txb_ctx(
#if INCOMPLETE_SB_FIX
                        pcs_ptr->parent_pcs_ptr->sequence_control_set_ptr,
#endif
                        COMPONENT_CHROMA,
                        cb_dc_sign_level_coeff_neighbor_array,
                        ROUND_UV(cu_origin_x + blk_geom->tx_org_x[cu_ptr->tx_depth][txb_itr] - blk_geom->origin_x) >> 1,
                        ROUND_UV(cu_origin_y + blk_geom->tx_org_y[cu_ptr->tx_depth][txb_itr] - blk_geom->origin_y) >> 1,
                        blk_geom->bsize_uv,
                        chroma_tx_size,
                        &txb_skip_ctx,
                        &dcSignCtx);

                    cul_level_cb =
                        Av1WriteCoeffsTxb1D(
                            pcs_ptr->parent_pcs_ptr,
                            frameContext,
                            ec_writer,
                            cu_ptr,
                            chroma_tx_size,
                            0,
                            txb_itr,
                            intraLumaDir,
                            coeff_buffer,
                            coeff_ptr->stride_cb,
                            COMPONENT_CHROMA,
                            txb_skip_ctx,
                            dcSignCtx,
                            cu_ptr->transform_unit_array[txb_itr].nz_coef_count[1]);
                }

                // cr
                coeff_buffer = (int32_t*)coeff_ptr->buffer_cr + context_ptr->coded_area_sb_uv;
                {
                    int16_t txb_skip_ctx = 0;
                    int16_t dcSignCtx = 0;

                    get_txb_ctx(
#if INCOMPLETE_SB_FIX
                        pcs_ptr->parent_pcs_ptr->sequence_control_set_ptr,
#endif
                        COMPONENT_CHROMA,
                        cr_dc_sign_level_coeff_neighbor_array,
                        ROUND_UV(cu_origin_x + blk_geom->tx_org_x[cu_ptr->tx_depth][txb_itr] - blk_geom->origin_x) >> 1,
                        ROUND_UV(cu_origin_y + blk_geom->tx_org_y[cu_ptr->tx_depth][txb_itr] - blk_geom->origin_y) >> 1,
                        blk_geom->bsize_uv,
                        chroma_tx_size,
                        &txb_skip_ctx,
                        &dcSignCtx);

                    cul_level_cr =
                        Av1WriteCoeffsTxb1D(
                            pcs_ptr->parent_pcs_ptr,
                            frameContext,
                            ec_writer,
                            cu_ptr,
                            chroma_tx_size,
                            0,
                            txb_itr,
                            intraLumaDir,
                            coeff_buffer,
                            coeff_ptr->stride_cr,
                            COMPONENT_CHROMA,
                            txb_skip_ctx,
                            dcSignCtx,
                            cu_ptr->transform_unit_array[txb_itr].nz_coef_count[2]);
                }
            }

            // Update the luma Dc Sign Level Coeff Neighbor Array
            {
                uint8_t dc_sign_level_coeff = (uint8_t)cul_level_y;
                //if (!txb_ptr->lumaCbf)
                //    dc_sign_level_coeff = 0;
                neighbor_array_unit_mode_write(
                    luma_dc_sign_level_coeff_neighbor_array,
                    (uint8_t*)&dc_sign_level_coeff,
                    cu_origin_x + blk_geom->tx_org_x[cu_ptr->tx_depth][txb_itr] - blk_geom->origin_x,
                    cu_origin_y + blk_geom->tx_org_y[cu_ptr->tx_depth][txb_itr] - blk_geom->origin_y,
                    blk_geom->tx_width[cu_ptr->tx_depth][txb_itr],
                    blk_geom->tx_height[cu_ptr->tx_depth][txb_itr],
                    NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
            }

            // Update the cb Dc Sign Level Coeff Neighbor Array

            if (blk_geom->has_uv)
            {
                uint8_t dc_sign_level_coeff = (uint8_t)cul_level_cb;
                neighbor_array_unit_mode_write(
                    cb_dc_sign_level_coeff_neighbor_array,
                    (uint8_t*)&dc_sign_level_coeff,
                    ROUND_UV(cu_origin_x + blk_geom->tx_org_x[cu_ptr->tx_depth][txb_itr] - blk_geom->origin_x) >> 1,
                    ROUND_UV(cu_origin_y + blk_geom->tx_org_y[cu_ptr->tx_depth][txb_itr] - blk_geom->origin_y) >> 1,
                    blk_geom->tx_width_uv[cu_ptr->tx_depth][txb_itr],
                    blk_geom->tx_height_uv[cu_ptr->tx_depth][txb_itr],
                    NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
            }

            if (blk_geom->has_uv)
                // Update the cr DC Sign Level Coeff Neighbor Array
            {
                uint8_t dc_sign_level_coeff = (uint8_t)cul_level_cr;
                neighbor_array_unit_mode_write(
                    cr_dc_sign_level_coeff_neighbor_array,
                    (uint8_t*)&dc_sign_level_coeff,
                    ROUND_UV(cu_origin_x + blk_geom->tx_org_x[cu_ptr->tx_depth][txb_itr] - blk_geom->origin_x) >> 1,
                    ROUND_UV(cu_origin_y + blk_geom->tx_org_y[cu_ptr->tx_depth][txb_itr] - blk_geom->origin_y) >> 1,
                    blk_geom->tx_width_uv[cu_ptr->tx_depth][txb_itr],
                    blk_geom->tx_height_uv[cu_ptr->tx_depth][txb_itr],
                    NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
            }

            context_ptr->coded_area_sb += blk_geom->tx_width[cu_ptr->tx_depth][txb_itr] * blk_geom->tx_height[cu_ptr->tx_depth][txb_itr];

            if (blk_geom->has_uv)
                context_ptr->coded_area_sb_uv += blk_geom->tx_width_uv[cu_ptr->tx_depth][txb_itr] * blk_geom->tx_height_uv[cu_ptr->tx_depth][txb_itr];
        }
    }
    return return_error;
}

/*********************************************************************
* EncodePartitionAv1
*   Encodes the partition
*********************************************************************/
// Return the number of elements in the partition CDF when
// partitioning the (square) block with luma block size of bsize.
static INLINE int32_t partition_cdf_length(BlockSize bsize) {
    if (bsize <= BLOCK_8X8)
        return PARTITION_TYPES;
    else if (bsize == BLOCK_128X128)
        return EXT_PARTITION_TYPES - 2;
    else
        return EXT_PARTITION_TYPES;
}
static int32_t cdf_element_prob(const AomCdfProb *const cdf,
    size_t element) {
    assert(cdf != NULL);
    return (element > 0 ? cdf[element - 1] : CDF_PROB_TOP) - cdf[element];
}
static void partition_gather_horz_alike(AomCdfProb *out,
    const AomCdfProb *const in,
    BlockSize bsize) {
    out[0] = CDF_PROB_TOP;
    out[0] -= cdf_element_prob(in, PARTITION_HORZ);
    out[0] -= cdf_element_prob(in, PARTITION_SPLIT);
    out[0] -= cdf_element_prob(in, PARTITION_HORZ_A);
    out[0] -= cdf_element_prob(in, PARTITION_HORZ_B);
    out[0] -= cdf_element_prob(in, PARTITION_VERT_A);
    if (bsize != BLOCK_128X128) out[0] -= cdf_element_prob(in, PARTITION_HORZ_4);
    out[0] = AOM_ICDF(out[0]);
    out[1] = AOM_ICDF(CDF_PROB_TOP);
    out[2] = 0;
}
static void partition_gather_vert_alike(AomCdfProb *out,
    const AomCdfProb *const in,
    BlockSize bsize) {
    out[0] = CDF_PROB_TOP;
    out[0] -= cdf_element_prob(in, PARTITION_VERT);
    out[0] -= cdf_element_prob(in, PARTITION_SPLIT);
    out[0] -= cdf_element_prob(in, PARTITION_HORZ_A);
    out[0] -= cdf_element_prob(in, PARTITION_VERT_A);
    out[0] -= cdf_element_prob(in, PARTITION_VERT_B);
    if (bsize != BLOCK_128X128) out[0] -= cdf_element_prob(in, PARTITION_VERT_4);
    out[0] = AOM_ICDF(out[0]);
    out[1] = AOM_ICDF(CDF_PROB_TOP);
    out[2] = 0;
}
static void EncodePartitionAv1(
    SequenceControlSet    *sequence_control_set_ptr,
    FRAME_CONTEXT           *frameContext,
    AomWriter              *ec_writer,
    BlockSize              bsize,
    PartitionType          p,
    uint32_t                  cu_origin_x,
    uint32_t                  cu_origin_y,
    NeighborArrayUnit    *partition_context_neighbor_array)
{
    const int32_t is_partition_point = bsize >= BLOCK_8X8;

    if (!is_partition_point) return;

    const int32_t hbs = (mi_size_wide[bsize] << 2) >> 1;
    const int32_t hasRows = (cu_origin_y + hbs) < sequence_control_set_ptr->seq_header.max_frame_height;
    const int32_t hasCols = (cu_origin_x + hbs) < sequence_control_set_ptr->seq_header.max_frame_width;

    uint32_t partitionContextLeftNeighborIndex = get_neighbor_array_unit_left_index(
        partition_context_neighbor_array,
        cu_origin_y);
    uint32_t partitionContextTopNeighborIndex = get_neighbor_array_unit_top_index(
        partition_context_neighbor_array,
        cu_origin_x);

    uint32_t contextIndex = 0;

    const PartitionContextType above_ctx = (((PartitionContext*)partition_context_neighbor_array->top_array)[partitionContextTopNeighborIndex].above == (int8_t)INVALID_NEIGHBOR_DATA) ?
        0 : ((PartitionContext*)partition_context_neighbor_array->top_array)[partitionContextTopNeighborIndex].above;
    const PartitionContextType left_ctx = (((PartitionContext*)partition_context_neighbor_array->left_array)[partitionContextLeftNeighborIndex].left == (int8_t)INVALID_NEIGHBOR_DATA) ?
        0 : ((PartitionContext*)partition_context_neighbor_array->left_array)[partitionContextLeftNeighborIndex].left;

    const int32_t bsl = mi_size_wide_log2[bsize] - mi_size_wide_log2[BLOCK_8X8];
    int32_t above = (above_ctx >> bsl) & 1, left = (left_ctx >> bsl) & 1;

    assert(mi_size_wide_log2[bsize] == mi_size_high_log2[bsize]);
    assert(bsl >= 0);
    assert(p < CDF_SIZE(EXT_PARTITION_TYPES));

    contextIndex = (left * 2 + above) + bsl * PARTITION_PLOFFSET;

    if (!hasRows && !hasCols) {
        assert(p == PARTITION_SPLIT);
        return;
    }

    if (hasRows && hasCols) {
        aom_write_symbol(
            ec_writer,
            p,
            frameContext->partition_cdf[contextIndex],
            partition_cdf_length(bsize));
    }
    else if (!hasRows && hasCols) {
        AomCdfProb cdf[CDF_SIZE(2)];
        partition_gather_vert_alike(cdf, frameContext->partition_cdf[contextIndex], bsize);
        aom_write_symbol(
            ec_writer,
            p == PARTITION_SPLIT,
            cdf,
            2);
    }
    else {
        AomCdfProb cdf[CDF_SIZE(2)];
        partition_gather_horz_alike(cdf, frameContext->partition_cdf[contextIndex], bsize);
        aom_write_symbol(
            ec_writer,
            p == PARTITION_SPLIT,
            cdf,
            2);
    }

    return;
}

/*********************************************************************
* EncodeSkipCoeffAv1
*   Encodes the skip coefficient flag
*********************************************************************/
static void EncodeSkipCoeffAv1(
    FRAME_CONTEXT           *frameContext,
    AomWriter              *ec_writer,
    EbBool                 skipCoeffFlag,
    uint32_t                  cu_origin_x,
    uint32_t                  cu_origin_y,
    NeighborArrayUnit    *skip_coeff_neighbor_array)
{
    //TODO: need to code in syntax for segmentation map + skip
    uint32_t skipCoeffLeftNeighborIndex = get_neighbor_array_unit_left_index(
        skip_coeff_neighbor_array,
        cu_origin_y);
    uint32_t skipCoeffTopNeighborIndex = get_neighbor_array_unit_top_index(
        skip_coeff_neighbor_array,
        cu_origin_x);

    uint32_t contextIndex = 0;

    contextIndex =
        (skip_coeff_neighbor_array->left_array[skipCoeffLeftNeighborIndex] == (uint8_t)INVALID_NEIGHBOR_DATA) ? 0 :
        (skip_coeff_neighbor_array->left_array[skipCoeffLeftNeighborIndex]) ? 1 : 0;

    contextIndex +=
        (skip_coeff_neighbor_array->top_array[skipCoeffTopNeighborIndex] == (uint8_t)INVALID_NEIGHBOR_DATA) ? 0 :
        (skip_coeff_neighbor_array->top_array[skipCoeffTopNeighborIndex]) ? 1 : 0;

    aom_write_symbol(
        ec_writer,
        skipCoeffFlag ? 1 : 0,
        frameContext->skip_cdfs[contextIndex],
        2);

    return;
}
/*********************************************************************
* EncodeIntraLumaModeAv1
*   Encodes the Intra Luma Mode
*********************************************************************/
static void EncodeIntraLumaModeAv1(
    FRAME_CONTEXT           *frameContext,
    AomWriter              *ec_writer,
    CodingUnit            *cu_ptr,
    uint32_t                  cu_origin_x,
    uint32_t                  cu_origin_y,
    BlockSize               bsize,
    uint32_t                  luma_mode,
    NeighborArrayUnit    *mode_type_neighbor_array,
    NeighborArrayUnit    *intra_luma_mode_neighbor_array)
{
    uint32_t modeTypeLeftNeighborIndex = get_neighbor_array_unit_left_index(
        mode_type_neighbor_array,
        cu_origin_y);
    uint32_t modeTypeTopNeighborIndex = get_neighbor_array_unit_top_index(
        mode_type_neighbor_array,
        cu_origin_x);
    uint32_t intraLumaModeLeftNeighborIndex = get_neighbor_array_unit_left_index(
        intra_luma_mode_neighbor_array,
        cu_origin_y);
    uint32_t intraLumaModeTopNeighborIndex = get_neighbor_array_unit_top_index(
        intra_luma_mode_neighbor_array,
        cu_origin_x);

    uint32_t topContext = 0, leftContext = 0;

    uint32_t left_neighbor_mode = (uint32_t)(
        (mode_type_neighbor_array->left_array[modeTypeLeftNeighborIndex] != INTRA_MODE) ? (uint32_t)DC_PRED :
        intra_luma_mode_neighbor_array->left_array[intraLumaModeLeftNeighborIndex]);

    uint32_t top_neighbor_mode = (uint32_t)(
        (mode_type_neighbor_array->top_array[modeTypeTopNeighborIndex] != INTRA_MODE) ? (uint32_t)DC_PRED :
        intra_luma_mode_neighbor_array->top_array[intraLumaModeTopNeighborIndex]);

    topContext = intra_mode_context[top_neighbor_mode];
    leftContext = intra_mode_context[left_neighbor_mode];

    aom_write_symbol(
        ec_writer,
        luma_mode,
        frameContext->kf_y_cdf[topContext][leftContext],
        INTRA_MODES);

    if (cu_ptr->pred_mode != INTRA_MODE_4x4)
        if (bsize >= BLOCK_8X8 && cu_ptr->prediction_unit_array[0].is_directional_mode_flag) {
            aom_write_symbol(ec_writer,
                cu_ptr->prediction_unit_array[0].angle_delta[PLANE_TYPE_Y] + MAX_ANGLE_DELTA,
                frameContext->angle_delta_cdf[luma_mode - V_PRED],
                2 * MAX_ANGLE_DELTA + 1);
        }

    return;
}
/*********************************************************************
* EncodeIntraLumaModeNonKeyAv1
*   Encodes the Intra Luma Mode for non Key frames
*********************************************************************/
static void EncodeIntraLumaModeNonKeyAv1(
    FRAME_CONTEXT           *frameContext,
    AomWriter              *ec_writer,
    CodingUnit            *cu_ptr,
    BlockSize                bsize,
    uint32_t                  luma_mode)
{
    aom_write_symbol(
        ec_writer,
        luma_mode,
        frameContext->y_mode_cdf[size_group_lookup[bsize]],
        INTRA_MODES);

    if (cu_ptr->pred_mode != INTRA_MODE_4x4)
        if (bsize >= BLOCK_8X8 && cu_ptr->prediction_unit_array[0].is_directional_mode_flag) {
            aom_write_symbol(ec_writer,
                cu_ptr->prediction_unit_array[0].angle_delta[PLANE_TYPE_Y] + MAX_ANGLE_DELTA,
                frameContext->angle_delta_cdf[luma_mode - V_PRED],
                2 * MAX_ANGLE_DELTA + 1);
        }

    return;
}

static void write_cfl_alphas(FRAME_CONTEXT *const ec_ctx, int32_t idx,
    int32_t joint_sign, AomWriter *w) {
    aom_write_symbol(w, joint_sign, ec_ctx->cfl_sign_cdf, CFL_JOINT_SIGNS);
    // Magnitudes are only signaled for nonzero codes.
    if (CFL_SIGN_U(joint_sign) != CFL_SIGN_ZERO) {
        AomCdfProb *cdf_u = ec_ctx->cfl_alpha_cdf[CFL_CONTEXT_U(joint_sign)];
        aom_write_symbol(w, CFL_IDX_U(idx), cdf_u, CFL_ALPHABET_SIZE);
    }
    if (CFL_SIGN_V(joint_sign) != CFL_SIGN_ZERO) {
        AomCdfProb *cdf_v = ec_ctx->cfl_alpha_cdf[CFL_CONTEXT_V(joint_sign)];
        aom_write_symbol(w, CFL_IDX_V(idx), cdf_v, CFL_ALPHABET_SIZE);
    }
}

/*********************************************************************
* EncodeIntraChromaModeAv1
*   Encodes the Intra Chroma Mode
*********************************************************************/
static void EncodeIntraChromaModeAv1(
    FRAME_CONTEXT           *frameContext,
    AomWriter              *ec_writer,
    CodingUnit            *cu_ptr,
    BlockSize     bsize,
    uint32_t                  luma_mode,
    uint32_t                  chroma_mode,
    uint8_t                   cflAllowed)
{
    aom_write_symbol(
        ec_writer,
        chroma_mode,
        frameContext->uv_mode_cdf[cflAllowed][luma_mode],
        UV_INTRA_MODES - !cflAllowed);

    if (chroma_mode == UV_CFL_PRED)
        write_cfl_alphas(frameContext, cu_ptr->prediction_unit_array->cfl_alpha_idx, cu_ptr->prediction_unit_array->cfl_alpha_signs, ec_writer);

    if (cu_ptr->pred_mode != INTRA_MODE_4x4)
        if (bsize >= BLOCK_8X8 && cu_ptr->prediction_unit_array[0].is_directional_chroma_mode_flag) {
            aom_write_symbol(ec_writer,
                cu_ptr->prediction_unit_array[0].angle_delta[PLANE_TYPE_UV] + MAX_ANGLE_DELTA,
                frameContext->angle_delta_cdf[chroma_mode - V_PRED],
                2 * MAX_ANGLE_DELTA + 1);
        }

    return;
}

/*********************************************************************
* EncodeSkipModeAv1
*   Encodes the skip Mode flag
*********************************************************************/
static void EncodeSkipModeAv1(
    FRAME_CONTEXT           *frameContext,
    AomWriter              *ec_writer,
    EbBool                 skipModeFlag,
    uint32_t                  cu_origin_x,
    uint32_t                  cu_origin_y,
    NeighborArrayUnit    *skip_flag_neighbor_array)
{

    //TODO: not coded in syntax for skip mode/ref-frame/global-mv in segmentation map
    uint32_t skipFlagLeftNeighborIndex = get_neighbor_array_unit_left_index(
        skip_flag_neighbor_array,
        cu_origin_y);
    uint32_t skipFlagTopNeighborIndex = get_neighbor_array_unit_top_index(
        skip_flag_neighbor_array,
        cu_origin_x);

    uint32_t contextIndex = 0;

    contextIndex =
        (skip_flag_neighbor_array->left_array[skipFlagLeftNeighborIndex] == (uint8_t)INVALID_NEIGHBOR_DATA) ? 0 :
        (skip_flag_neighbor_array->left_array[skipFlagLeftNeighborIndex]) ? 1 : 0;

    contextIndex +=
        (skip_flag_neighbor_array->top_array[skipFlagTopNeighborIndex] == (uint8_t)INVALID_NEIGHBOR_DATA) ? 0 :
        (skip_flag_neighbor_array->top_array[skipFlagTopNeighborIndex]) ? 1 : 0;

    aom_write_symbol(
        ec_writer,
        skipModeFlag ? 1 : 0,
        frameContext->skip_mode_cdfs[contextIndex],
        2);

    return;
}
/*********************************************************************
* EncodePredModeAv1
*   Encodes the Prediction Mode
*********************************************************************/
static void EncodePredModeAv1(
    FRAME_CONTEXT           *frameContext,
    AomWriter              *ec_writer,
    EbBool                 predModeFlag,
    uint32_t                  cu_origin_x,
    uint32_t                  cu_origin_y,
    NeighborArrayUnit    *mode_type_neighbor_array)
{
    uint32_t modeTypeLeftNeighborIndex = get_neighbor_array_unit_left_index(
        mode_type_neighbor_array,
        cu_origin_y);
    uint32_t modeTypeTopNeighborIndex = get_neighbor_array_unit_top_index(
        mode_type_neighbor_array,
        cu_origin_x);

    uint32_t contextIndex = 0;

    if (mode_type_neighbor_array->left_array[modeTypeLeftNeighborIndex] != (uint8_t)INVALID_MODE && mode_type_neighbor_array->top_array[modeTypeTopNeighborIndex] != (uint8_t)INVALID_MODE) {
        contextIndex = (mode_type_neighbor_array->left_array[modeTypeLeftNeighborIndex] == (uint8_t)INTRA_MODE && mode_type_neighbor_array->top_array[modeTypeTopNeighborIndex] == (uint8_t)INTRA_MODE) ? 3 :
            (mode_type_neighbor_array->left_array[modeTypeLeftNeighborIndex] == (uint8_t)INTRA_MODE || mode_type_neighbor_array->top_array[modeTypeTopNeighborIndex] == (uint8_t)INTRA_MODE) ? 1 : 0;
    }
    else  if (mode_type_neighbor_array->left_array[modeTypeLeftNeighborIndex] != (uint8_t)INVALID_MODE)
        contextIndex = (mode_type_neighbor_array->left_array[modeTypeLeftNeighborIndex] == (uint8_t)INTRA_MODE) ? 2 : 0;
    else if (mode_type_neighbor_array->top_array[modeTypeTopNeighborIndex] != (uint8_t)INVALID_MODE)
        contextIndex = (mode_type_neighbor_array->top_array[modeTypeTopNeighborIndex] == (uint8_t)INTRA_MODE) ? 2 : 0;
    else
        contextIndex = 0;
    aom_write_symbol(
        ec_writer,
        predModeFlag == INTER_MODE ? 1 : 0,
        frameContext->intra_inter_cdf[contextIndex],
        2);

    return;
}

//****************************************************************************************************//

/*********************************************************************
* motion_mode_allowed
*   checks the motion modes that are allowed for the current block
*********************************************************************/

static INLINE int is_inter_mode(PredictionMode mode)
{
    return mode >= SINGLE_INTER_MODE_START && mode < SINGLE_INTER_MODE_END;
}

static INLINE int is_global_mv_block(
    const PredictionMode          mode,
    const BlockSize               bsize,
    TransformationType            type)
{
    return (mode == GLOBALMV || mode == GLOBAL_GLOBALMV)
            && type > TRANSLATION
            && is_motion_variation_allowed_bsize(bsize);
}

MotionMode motion_mode_allowed(
    const PictureControlSet       *picture_control_set_ptr,
    const CodingUnit              *cu_ptr,
    const BlockSize                 bsize,
    MvReferenceFrame                rf0,
    MvReferenceFrame                rf1,
    PredictionMode                  mode)
{
    FrameHeader *frm_hdr = &picture_control_set_ptr->parent_pcs_ptr->frm_hdr;
    if(!frm_hdr->is_motion_mode_switchable)
        return SIMPLE_TRANSLATION;

    if (frm_hdr->force_integer_mv == 0) {
        const TransformationType gm_type =
            picture_control_set_ptr->parent_pcs_ptr->global_motion[rf0].wmtype;
        if (is_global_mv_block(mode, bsize, gm_type))
            return SIMPLE_TRANSLATION;
    }

    if (is_motion_variation_allowed_bsize(bsize) &&
        is_inter_mode(mode) &&
        rf1 != INTRA_FRAME &&
        !(rf1 > INTRA_FRAME) ) // is_motion_variation_allowed_compound
    {
        if (!has_overlappable_candidates(cu_ptr)) // check_num_overlappable_neighbors
            return SIMPLE_TRANSLATION;

        if (cu_ptr->prediction_unit_array[0].num_proj_ref >= 1 &&
           (frm_hdr->allow_warped_motion)) // TODO(JS): when scale is added, put: && !av1_is_scaled(&(xd->block_refs[0]->sf))
        {
            if (frm_hdr->force_integer_mv)
                return OBMC_CAUSAL;
            return WARPED_CAUSAL;
        }
        return OBMC_CAUSAL;
    } else
        return SIMPLE_TRANSLATION;
}

/*********************************************************************
* write_motion_mode
*   Encodes the Motion Mode (obmc or warped)
*********************************************************************/
static void write_motion_mode(
    FRAME_CONTEXT            *frame_context,
    AomWriter               *ec_writer,
    BlockSize                 bsize,
    MotionMode               motion_mode,
    MvReferenceFrame          rf0,
    MvReferenceFrame          rf1,
    CodingUnit             *cu_ptr,
    PictureControlSet      *picture_control_set_ptr)
{
    const PredictionMode mode = cu_ptr->prediction_unit_array[0].inter_mode;
    MotionMode last_motion_mode_allowed =
        motion_mode_allowed(picture_control_set_ptr, cu_ptr, bsize, rf0, rf1, mode);

    switch (last_motion_mode_allowed) {
    case SIMPLE_TRANSLATION: break;
    case OBMC_CAUSAL:
        aom_write_symbol(
            ec_writer,
            SIMPLE_TRANSLATION, // motion_mode == OBMC_CAUSAL, TODO: support OBMC
            frame_context->obmc_cdf[bsize],
            2);
        break;
    default:
        aom_write_symbol(
            ec_writer,
            motion_mode,
            frame_context->motion_mode_cdf[bsize],
            MOTION_MODES);
    }

    return;
}

//****************************************************************************************************//
extern  int8_t av1_ref_frame_type(const MvReferenceFrame *const rf);
uint16_t compound_mode_ctx_map[3][COMP_NEWMV_CTXS] = {
   { 0, 1, 1, 1, 1 },
   { 1, 2, 3, 4, 4 },
   { 4, 4, 5, 6, 7 },
};

static int16_t Av1ModeContextAnalyzer(
    const int16_t *const mode_context, const MvReferenceFrame *const rf) {
    const int8_t ref_frame = av1_ref_frame_type(rf);

    if (rf[1] <= INTRA_FRAME) return mode_context[ref_frame];

    const int16_t newmv_ctx = mode_context[ref_frame] & NEWMV_CTX_MASK;
    const int16_t refmv_ctx =
        (mode_context[ref_frame] >> REFMV_OFFSET) & REFMV_CTX_MASK;
    assert((refmv_ctx >> 1) < 3);
    const int16_t comp_ctx = compound_mode_ctx_map[refmv_ctx >> 1][AOMMIN(
        newmv_ctx, COMP_NEWMV_CTXS - 1)];
    return comp_ctx;
}

EbErrorType encode_slice_finish(
    EntropyCoder        *entropy_coder_ptr)
{
    EbErrorType return_error = EB_ErrorNone;

    aom_stop_encode(&entropy_coder_ptr->ec_writer);

    return return_error;
}

EbErrorType reset_bitstream(
    EbPtr bitstream_ptr)
{
    EbErrorType return_error = EB_ErrorNone;
    OutputBitstreamUnit *output_bitstream_ptr = (OutputBitstreamUnit*)bitstream_ptr;

    output_bitstream_reset(
        output_bitstream_ptr);

    return return_error;
}

EbErrorType reset_entropy_coder(
    EncodeContext            *encode_context_ptr,
    EntropyCoder             *entropy_coder_ptr,
    uint32_t                      qp,
    EB_SLICE                    slice_type)
{
    EbErrorType return_error = EB_ErrorNone;

    (void)encode_context_ptr;
    (void)slice_type;
    av1_default_coef_probs(entropy_coder_ptr->fc, qp);
    init_mode_probs(entropy_coder_ptr->fc);

    return return_error;
}

EbErrorType copy_rbsp_bitstream_to_payload(
    Bitstream *bitstream_ptr,
    EbByte      output_buffer,
    uint32_t      *output_buffer_index,
    uint32_t      *output_buffer_size,
    EncodeContext         *encode_context_ptr)
{
    EbErrorType return_error = EB_ErrorNone;
    OutputBitstreamUnit *output_bitstream_ptr = (OutputBitstreamUnit*)bitstream_ptr->output_bitstream_ptr;

    CHECK_REPORT_ERROR(
        ((output_bitstream_ptr->written_bits_count >> 3) + (*output_buffer_index) < (*output_buffer_size)),
        encode_context_ptr->app_callback_ptr,
        EB_ENC_EC_ERROR2);

    output_bitstream_rbsp_to_payload(
        output_bitstream_ptr,
        output_buffer,
        output_buffer_index,
        output_buffer_size,
        0);

    return return_error;
}

static void bitstream_dctor(EbPtr p)
{
    Bitstream *obj = (Bitstream*)p;
    OutputBitstreamUnit *output_bitstream_ptr = (OutputBitstreamUnit *)obj->output_bitstream_ptr;
    EB_DELETE(output_bitstream_ptr);
}

EbErrorType bitstream_ctor(
    Bitstream *bitstream_ptr,
    uint32_t buffer_size)
{
    EbErrorType return_error = EB_ErrorNone;
    OutputBitstreamUnit* output_bitstream_ptr;

    bitstream_ptr->dctor = bitstream_dctor;

    EB_NEW(
        output_bitstream_ptr,
        output_bitstream_unit_ctor,
        buffer_size);
    bitstream_ptr->output_bitstream_ptr = output_bitstream_ptr;

    return return_error;
}

static void entropy_coder_dctor(EbPtr p)
{
    EntropyCoder *obj = (EntropyCoder *)p;
    CabacEncodeContext *cabacEncCtxPtr = (CabacEncodeContext*)obj->cabac_encode_context_ptr;
    OutputBitstreamUnit *output_bitstream_ptr = (OutputBitstreamUnit *)cabacEncCtxPtr->bac_enc_context.m_pc_t_com_bit_if;

    EB_DELETE(output_bitstream_ptr);

    output_bitstream_ptr = (OutputBitstreamUnit*)obj->ec_output_bitstream_ptr;
    EB_DELETE(output_bitstream_ptr);

    EB_FREE(obj->fc);
    EB_FREE(obj->cabac_encode_context_ptr);
}
EbErrorType entropy_coder_ctor(
    EntropyCoder *entropy_coder_ptr,
    uint32_t buffer_size)
{
    EbErrorType return_error = EB_ErrorNone;
    OutputBitstreamUnit *output_bitstream_ptr;

    entropy_coder_ptr->dctor = entropy_coder_dctor;

    EB_CALLOC(entropy_coder_ptr->cabac_encode_context_ptr, 1, sizeof(CabacEncodeContext));

    EB_MALLOC(entropy_coder_ptr->fc, sizeof(FRAME_CONTEXT));


    EB_NEW(
        output_bitstream_ptr,
        output_bitstream_unit_ctor,
        buffer_size);

    entropy_coder_ptr->ec_output_bitstream_ptr = output_bitstream_ptr;

    EB_NEW(
        output_bitstream_ptr,
        output_bitstream_unit_ctor,
        buffer_size);
    ((CabacEncodeContext*)entropy_coder_ptr->cabac_encode_context_ptr)->bac_enc_context.m_pc_t_com_bit_if = output_bitstream_ptr;
    return return_error;
}

EbPtr entropy_coder_get_bitstream_ptr(
    EntropyCoder *entropy_coder_ptr)
{
    CabacEncodeContext *cabacEncCtxPtr = (CabacEncodeContext*)entropy_coder_ptr->cabac_encode_context_ptr;
    EbPtr bitstream_ptr = cabacEncCtxPtr->bac_enc_context.m_pc_t_com_bit_if;

    return bitstream_ptr;
}

//*******************************************************************************************//
//*******************************************************************************************//
//*******************************************************************************************//
//*******************************************************************************************//
// aom_integer.c
static const size_t kMaximumLeb128Size = 8;
static const uint64_t kMaximumLeb128Value = 0xFFFFFFFFFFFFFF;  // 2 ^ 56 - 1

size_t aom_uleb_size_in_bytes(uint64_t value) {
    size_t size = 0;
    do {
        ++size;
    } while ((value >>= 7) != 0);
    return size;
}

int32_t aom_uleb_encode(uint64_t value, size_t available, uint8_t *coded_value,
    size_t *coded_size) {
    const size_t leb_size = aom_uleb_size_in_bytes(value);
    if (value > kMaximumLeb128Value || leb_size > kMaximumLeb128Size ||
        leb_size > available || !coded_value || !coded_size) {
        return -1;
    }

    for (size_t i = 0; i < leb_size; ++i) {
        uint8_t byte = value & 0x7f;
        value >>= 7;

        if (value != 0) byte |= 0x80;  // Signal that more bytes follow.

        *(coded_value + i) = byte;
    }

    *coded_size = leb_size;
    return 0;
}

int32_t aom_wb_is_byte_aligned(const struct AomWriteBitBuffer *wb) {
    return (wb->bit_offset % CHAR_BIT == 0);
}

uint32_t aom_wb_bytes_written(const struct AomWriteBitBuffer *wb) {
    return wb->bit_offset / CHAR_BIT + (wb->bit_offset % CHAR_BIT > 0);
}

void aom_wb_write_bit(struct AomWriteBitBuffer *wb, int32_t bit) {
    const int32_t off = (int32_t)wb->bit_offset;
    const int32_t p = off / CHAR_BIT;
    const int32_t q = CHAR_BIT - 1 - off % CHAR_BIT;
    if (q == CHAR_BIT - 1) {
        // Zero next char and write bit
        wb->bit_buffer[p] = (uint8_t)(bit << q);
    }
    else {
        wb->bit_buffer[p] &= ~(1 << q);
        wb->bit_buffer[p] |= bit << q;
    }
    wb->bit_offset = off + 1;
}

void aom_wb_overwrite_bit(struct AomWriteBitBuffer *wb, int32_t bit) {
    // Do not zero bytes but overwrite exisiting values
    const int32_t off = (int32_t)wb->bit_offset;
    const int32_t p = off / CHAR_BIT;
    const int32_t q = CHAR_BIT - 1 - off % CHAR_BIT;
    wb->bit_buffer[p] &= ~(1 << q);
    wb->bit_buffer[p] |= bit << q;
    wb->bit_offset = off + 1;
}

void aom_wb_write_literal(struct AomWriteBitBuffer *wb, int32_t data, int32_t bits) {
    int32_t bit;
    for (bit = bits - 1; bit >= 0; bit--) aom_wb_write_bit(wb, (data >> bit) & 1);
}

void aom_wb_write_inv_signed_literal(struct AomWriteBitBuffer *wb, int32_t data,
    int32_t bits) {
    aom_wb_write_literal(wb, data, bits + 1);
}

//*******************************************************************************************//

static void WriteInterMode(
    FRAME_CONTEXT       *frameContext,
    AomWriter          *ec_writer,
    PredictionMode     mode,
    const int16_t       mode_ctx,
    uint32_t                  cu_origin_x,
    uint32_t                  cu_origin_y)
{
    (void)cu_origin_x;
    (void)cu_origin_y;
    int16_t newmv_ctx = mode_ctx & NEWMV_CTX_MASK;
    assert(newmv_ctx<NEWMV_MODE_CONTEXTS);
    aom_write_symbol(ec_writer, mode != NEWMV, frameContext->newmv_cdf[newmv_ctx], 2);

    if (mode != NEWMV) {
        const int16_t zeromv_ctx =
            (mode_ctx >> GLOBALMV_OFFSET) & GLOBALMV_CTX_MASK;
        aom_write_symbol(ec_writer, mode != GLOBALMV, frameContext->zeromv_cdf[zeromv_ctx], 2);

        if (mode != GLOBALMV) {
            int16_t refmv_ctx = (mode_ctx >> REFMV_OFFSET) & REFMV_CTX_MASK;
            assert(refmv_ctx<REFMV_MODE_CONTEXTS);
            aom_write_symbol(ec_writer, mode != NEARESTMV, frameContext->refmv_cdf[refmv_ctx], 2);
        }
    }
}

//extern INLINE int8_t av1_ref_frame_type(const MvReferenceFrame *const rf);
extern uint8_t av1_drl_ctx(const CandidateMv *ref_mv_stack, int32_t ref_idx);

void WriteDrlIdx(
    FRAME_CONTEXT       *frameContext,
    AomWriter          *ec_writer,
    CodingUnit        *cu_ptr) {
    //uint8_t ref_frame_type = av1_ref_frame_type(mbmi->ref_frame);
    uint8_t ref_frame_type = cu_ptr->prediction_unit_array[0].ref_frame_type;
    MacroBlockD*  xd = cu_ptr->av1xd;
    //assert(mbmi->ref_mv_idx < 3);

    //xd->ref_mv_stack[ref_frame][ref_mv_idx].comp_mv;

    const int32_t new_mv = cu_ptr->pred_mode == NEWMV || cu_ptr->pred_mode == NEW_NEWMV;
    if (new_mv) {
        int32_t idx;
        for (idx = 0; idx < 2; ++idx) {
            if (xd->ref_mv_count[ref_frame_type] > idx + 1) {
                uint8_t drl_ctx =
                    av1_drl_ctx(xd->final_ref_mv_stack, idx);

                aom_write_symbol(ec_writer, cu_ptr->drl_index != idx, frameContext->drl_cdf[drl_ctx],
                    2);

                if (cu_ptr->drl_index == idx) return;
            }
        }
        return;
    }

    if (have_nearmv_in_inter_mode(cu_ptr->pred_mode)) {
        int32_t idx;
        // TODO(jingning): Temporary solution to compensate the NEARESTMV offset.
        for (idx = 1; idx < 3; ++idx) {
            if (xd->ref_mv_count[ref_frame_type] > idx + 1) {
                uint8_t drl_ctx =
                    av1_drl_ctx(xd->final_ref_mv_stack, idx);

                aom_write_symbol(ec_writer, cu_ptr->drl_index != (idx - 1),
                    frameContext->drl_cdf[drl_ctx], 2);

                if (cu_ptr->drl_index == (idx - 1)) return;
            }
        }
        return;
    }
}

extern MvJointType av1_get_mv_joint(int32_t diff[2]);
static void encode_mv_component(AomWriter *w, int32_t comp, NmvComponent *mvcomp,
    MvSubpelPrecision precision) {
    int32_t offset;
    const int32_t sign = comp < 0;
    const int32_t mag = sign ? -comp : comp;
    const int32_t mv_class = av1_get_mv_class(mag - 1, &offset);
    const int32_t d = offset >> 3;         // int32_t mv data
    const int32_t fr = (offset >> 1) & 3;  // fractional mv data
    const int32_t hp = offset & 1;         // high precision mv data

    assert(comp != 0);

    // Sign
    aom_write_symbol(w, sign, mvcomp->sign_cdf, 2);

    // Class
    aom_write_symbol(w, mv_class, mvcomp->classes_cdf, MV_CLASSES);

    // Integer bits
    if (mv_class == MV_CLASS_0)
        aom_write_symbol(w, d, mvcomp->class0_cdf, CLASS0_SIZE);
    else {
        int32_t i;
        const int32_t n = mv_class + CLASS0_BITS - 1;  // number of bits
        for (i = 0; i < n; ++i)
            aom_write_symbol(w, (d >> i) & 1, mvcomp->bits_cdf[i], 2);
    }
    // Fractional bits
    if (precision > MV_SUBPEL_NONE) {
        aom_write_symbol(
            w, fr,
            mv_class == MV_CLASS_0 ? mvcomp->class0_fp_cdf[d] : mvcomp->fp_cdf,
            MV_FP_SIZE);
    }

    // High precision bit
    if (precision > MV_SUBPEL_LOW_PRECISION)
        aom_write_symbol(
            w, hp, mv_class == MV_CLASS_0 ? mvcomp->class0_hp_cdf : mvcomp->hp_cdf,
            2);
}

MvJointType av1_get_mv_joint_diff(int32_t diff[2]) {
    if (diff[0] == 0)
        return diff[1] == 0 ? MV_JOINT_ZERO : MV_JOINT_HNZVZ;
    else
        return diff[1] == 0 ? MV_JOINT_HZVNZ : MV_JOINT_HNZVNZ;
}

static INLINE int32_t is_mv_valid(const MV *mv) {
    return mv->row > MV_LOW && mv->row < MV_UPP && mv->col > MV_LOW &&
        mv->col < MV_UPP;
}

void av1_encode_mv(
    PictureParentControlSet   *pcs_ptr,
    AomWriter                  *ec_writer,
    const MV *mv,
    const MV *ref,
    NmvContext *mvctx,
    int32_t usehp) {
    if (is_mv_valid(mv) == 0)
        printf("Corrupted MV\n");

    int32_t diff[2] = { mv->row - ref->row, mv->col - ref->col };
    const MvJointType j = av1_get_mv_joint_diff(diff);

    if (pcs_ptr->frm_hdr.force_integer_mv)
        usehp = MV_SUBPEL_NONE;
    aom_write_symbol(ec_writer, j, mvctx->joints_cdf, MV_JOINTS);
    if (mv_joint_vertical(j))
        encode_mv_component(ec_writer, diff[0], &mvctx->comps[0], (MvSubpelPrecision)usehp);

    if (mv_joint_horizontal(j))
        encode_mv_component(ec_writer, diff[1], &mvctx->comps[1], (MvSubpelPrecision)usehp);

    // If auto_mv_step_size is enabled then keep track of the largest
    // motion vector component used.
    //if (cpi->sf.mv.auto_mv_step_size) {
    //    uint32_t maxv = AOMMAX(abs(mv->row), abs(mv->col)) >> 3;
    //    cpi->max_mv_magnitude = AOMMAX(maxv, cpi->max_mv_magnitude);
    //}
}

///InterpFilter av1_extract_interp_filter(uint32_t filters,
//    int32_t x_filter) {
//    return (InterpFilter)((filters >> (x_filter ? 16 : 0)) & 0xffff);
//}
#define INTER_FILTER_COMP_OFFSET (SWITCHABLE_FILTERS + 1)
#define INTER_FILTER_DIR_OFFSET ((SWITCHABLE_FILTERS + 1) * 2)

int32_t av1_get_pred_context_switchable_interp(
    NeighborArrayUnit     *ref_frame_type_neighbor_array,
    MvReferenceFrame rf0,
    MvReferenceFrame rf1,
    NeighborArrayUnit32     *interpolation_type_neighbor_array,
    uint32_t cu_origin_x,
    uint32_t cu_origin_y,
    //const MacroBlockD *xd,
    int32_t dir
) {
    uint32_t interpolationTypeLeftNeighborIndex = get_neighbor_array_unit_left_index32(
        interpolation_type_neighbor_array,
        cu_origin_y);
    uint32_t interpolationTypeTopNeighborIndex = get_neighbor_array_unit_top_index32(
        interpolation_type_neighbor_array,
        cu_origin_x);

    uint32_t rfLeftNeighborIndex = get_neighbor_array_unit_left_index(
        ref_frame_type_neighbor_array,
        cu_origin_y);
    uint32_t rfTopNeighborIndex = get_neighbor_array_unit_top_index(
        ref_frame_type_neighbor_array,
        cu_origin_x);

    //const MbModeInfo *const mbmi = xd->mi[0];
    const int32_t ctx_offset =
        (rf1 > INTRA_FRAME) * INTER_FILTER_COMP_OFFSET;
    MvReferenceFrame ref_frame =
        (dir < 2) ? rf0 : rf1;
    // Note:
    // The mode info data structure has a one element border above and to the
    // left of the entries corresponding to real macroblocks.
    // The prediction flags in these dummy entries are initialized to 0.
    int32_t filter_type_ctx = ctx_offset + (dir & 0x01) * INTER_FILTER_DIR_OFFSET;
    int32_t left_type = SWITCHABLE_FILTERS;
    int32_t above_type = SWITCHABLE_FILTERS;

    if (cu_origin_x != 0 /*&& interpolation_type_neighbor_array->left_array[interpolationTypeLeftNeighborIndex] != SWITCHABLE_FILTERS*/) {
        MvReferenceFrame rf_left[2];
        av1_set_ref_frame(rf_left, ref_frame_type_neighbor_array->left_array[rfLeftNeighborIndex]);
        uint32_t leftNeigh = (uint32_t)interpolation_type_neighbor_array->left_array[interpolationTypeLeftNeighborIndex];
        left_type = (rf_left[0] == ref_frame || rf_left[1] == ref_frame) ? av1_extract_interp_filter(leftNeigh, dir & 0x01) : SWITCHABLE_FILTERS;
    }
    //get_ref_filter_type(xd->mi[-1], xd, dir, ref_frame);

    if (cu_origin_y != 0 /*&& interpolation_type_neighbor_array->top_array[interpolationTypeTopNeighborIndex] != SWITCHABLE_FILTERS*/) {
        MvReferenceFrame rf_above[2];
        av1_set_ref_frame(rf_above, ref_frame_type_neighbor_array->top_array[rfTopNeighborIndex]);
        uint32_t aboveNeigh = (uint32_t)interpolation_type_neighbor_array->top_array[interpolationTypeTopNeighborIndex];

        above_type = (rf_above[0] == ref_frame || rf_above[1] == ref_frame) ? av1_extract_interp_filter(aboveNeigh, dir & 0x01) : SWITCHABLE_FILTERS;
        //get_ref_filter_type(xd->mi[-xd->mi_stride], xd, dir, ref_frame);
    }

    if (left_type == above_type)
        filter_type_ctx += left_type;
    else if (left_type == SWITCHABLE_FILTERS) {
        assert(above_type != SWITCHABLE_FILTERS);
        filter_type_ctx += above_type;
    }
    else if (above_type == SWITCHABLE_FILTERS) {
        assert(left_type != SWITCHABLE_FILTERS);
        filter_type_ctx += left_type;
    }
    else
        filter_type_ctx += SWITCHABLE_FILTERS;
    //  printf("\t %d\t %d\t %d",filter_type_ctx,left_type ,above_type);

    return filter_type_ctx;
}

/*INLINE*/ int32_t is_nontrans_global_motion_EC(
    MvReferenceFrame             rf0,
    MvReferenceFrame             rf1,
    CodingUnit                  *cu_ptr,
    BlockSize sb_type,
    PictureParentControlSet   *pcs_ptr) {
    int32_t ref;

    // First check if all modes are GLOBALMV
    if (cu_ptr->pred_mode != GLOBALMV && cu_ptr->pred_mode != GLOBAL_GLOBALMV)
        return 0;

    if (MIN(mi_size_wide[sb_type], mi_size_high[sb_type]) < 2)
        return 0;

    // Now check if all global motion is non translational
    for (ref = 0; ref < 1 + cu_ptr->prediction_unit_array->is_compound; ++ref) {
        if (pcs_ptr->global_motion[ref ? rf1 : rf0].wmtype == TRANSLATION)
            return 0;
    }
    return 1;
}
static int32_t av1_is_interp_needed(
    MvReferenceFrame            rf0,
    MvReferenceFrame            rf1,
    CodingUnit               *cu_ptr,
    BlockSize                   bsize,
    PictureParentControlSet  *pcs_ptr)
{
    if (cu_ptr->skip_flag)
        return 0;

    if (cu_ptr->prediction_unit_array[0].motion_mode == WARPED_CAUSAL)
        return 0;

    if (is_nontrans_global_motion_EC(rf0, rf1, cu_ptr, bsize, pcs_ptr))
        return 0;

    return 1;
}

void write_mb_interp_filter(
    NeighborArrayUnit     *ref_frame_type_neighbor_array,
    BlockSize bsize,
    MvReferenceFrame rf0,
    MvReferenceFrame rf1,
    PictureParentControlSet   *pcs_ptr,
    AomWriter                  *ec_writer,
    CodingUnit             *cu_ptr,
    EntropyCoder *entropy_coder_ptr,
    NeighborArrayUnit32     *interpolation_type_neighbor_array,
    uint32_t blkOriginX,
    uint32_t blkOriginY)
{
    Av1Common *const cm = pcs_ptr->av1_cm; //&cpi->common;
    //const MbModeInfo *const mbmi = xd->mi[0];
    //FRAME_CONTEXT *ec_ctx = xd->tile_ctx;

    if (!av1_is_interp_needed(rf0, rf1, cu_ptr, bsize, pcs_ptr)) {
        /* assert(mbmi->interp_filters ==
                av1_broadcast_interp_filter(
                    av1_unswitchable_filter(cm->interp_filter)));*/
        return;
    }
    if (cm->interp_filter == SWITCHABLE) {
        int32_t dir;
        for (dir = 0; dir < 2; ++dir) {
            // printf("\nP:%d\tX: %d\tY:%d\t %d",(pcs_ptr)->picture_number,blkOriginX,blkOriginY ,((ec_writer)->ec).rng);
            const int32_t ctx = av1_get_pred_context_switchable_interp(
                ref_frame_type_neighbor_array,
                rf0,
                rf1,
                interpolation_type_neighbor_array,
                blkOriginX,
                blkOriginY,
                //xd,
                dir
            );
            InterpFilter filter = av1_extract_interp_filter(cu_ptr->interp_filters, dir);
            assert(ctx < SWITCHABLE_FILTER_CONTEXTS);
            assert(filter < CDF_SIZE(SWITCHABLE_FILTERS));
            aom_write_symbol(ec_writer, filter, /*ec_ctx*/entropy_coder_ptr->fc->switchable_interp_cdf[ctx],
                SWITCHABLE_FILTERS);

            // ++pcs_ptr->interp_filter_selected[0][filter];
            if (pcs_ptr->sequence_control_set_ptr->seq_header.enable_dual_filter == 0) return;
        }
    }
}

static void WriteInterCompoundMode(
    FRAME_CONTEXT       *frameContext,
    AomWriter          *ec_writer,
    PredictionMode     mode,
    const int16_t       mode_ctx) {
    assert(is_inter_compound_mode(mode));
    aom_write_symbol(ec_writer, INTER_COMPOUND_OFFSET(mode),
        frameContext->inter_compound_mode_cdf[mode_ctx],
        INTER_COMPOUND_MODES);
}

int32_t av1_get_reference_mode_context(
    uint32_t                  cu_origin_x,
    uint32_t                  cu_origin_y,
    NeighborArrayUnit    *mode_type_neighbor_array,
    NeighborArrayUnit    *inter_pred_dir_neighbor_array)
{
    uint32_t modeTypeLeftNeighborIndex = get_neighbor_array_unit_left_index(
        mode_type_neighbor_array,
        cu_origin_y);
    uint32_t modeTypeTopNeighborIndex = get_neighbor_array_unit_top_index(
        mode_type_neighbor_array,
        cu_origin_x);

    int32_t ctx = 0;

    // Note:
    // The mode info data structure has a one element border above and to the
    // left of the entries corresponding to real macroblocks.
    // The prediction flags in these dummy entries are initialized to 0.
    if (mode_type_neighbor_array->left_array[modeTypeLeftNeighborIndex] != (uint8_t)INVALID_MODE && mode_type_neighbor_array->top_array[modeTypeTopNeighborIndex] != (uint8_t)INVALID_MODE) {  // both edges available
        const int32_t topIntra = (mode_type_neighbor_array->top_array[modeTypeTopNeighborIndex] == (uint8_t)INTRA_MODE);
        const int32_t leftIntra = (mode_type_neighbor_array->left_array[modeTypeLeftNeighborIndex] == (uint8_t)INTRA_MODE);
        //if (has_above && has_left) {  // both edges available
        if (!(inter_pred_dir_neighbor_array->top_array[modeTypeTopNeighborIndex] == BI_PRED && !topIntra) && !(inter_pred_dir_neighbor_array->left_array[modeTypeLeftNeighborIndex] == BI_PRED && !leftIntra)) {
            // neither edge uses comp pred (0/1)
            ctx = (inter_pred_dir_neighbor_array->top_array[modeTypeTopNeighborIndex] == UNI_PRED_LIST_1) ^
                (inter_pred_dir_neighbor_array->left_array[modeTypeLeftNeighborIndex] == UNI_PRED_LIST_1);
        }
        else if (!(inter_pred_dir_neighbor_array->top_array[modeTypeTopNeighborIndex] == BI_PRED && !topIntra)/*has_second_ref(above_mbmi)*/) {
            // one of two edges uses comp pred (2/3)
            ctx = 2 + ((inter_pred_dir_neighbor_array->top_array[modeTypeTopNeighborIndex] == UNI_PRED_LIST_1) ||
                topIntra);
        }
        else if (!(inter_pred_dir_neighbor_array->left_array[modeTypeLeftNeighborIndex] == BI_PRED && !leftIntra)/*has_second_ref(left_mbmi)*/) {
            // one of two edges uses comp pred (2/3)
            ctx = 2 + ((inter_pred_dir_neighbor_array->left_array[modeTypeLeftNeighborIndex] == UNI_PRED_LIST_1) ||
                leftIntra);
        }
        else {  // both edges use comp pred (4){
            ctx = 4;
        }
    }
    else if (mode_type_neighbor_array->left_array[modeTypeLeftNeighborIndex] != (uint8_t)INVALID_MODE) {  // one edge available

        if (!(inter_pred_dir_neighbor_array->left_array[modeTypeLeftNeighborIndex] == BI_PRED && mode_type_neighbor_array->left_array[modeTypeLeftNeighborIndex] != (uint8_t)INTRA_MODE)/*has_second_ref(edge_mbmi)*/) {
            // edge does not use comp pred (0/1)
            ctx = (inter_pred_dir_neighbor_array->left_array[modeTypeLeftNeighborIndex] == UNI_PRED_LIST_1);
        }
        else {
            // edge uses comp pred (3)
            ctx = 3;
        }
    }
    else if (mode_type_neighbor_array->top_array[modeTypeTopNeighborIndex] != (uint8_t)INVALID_MODE) {  // one edge available

        if (!(inter_pred_dir_neighbor_array->top_array[modeTypeTopNeighborIndex] == BI_PRED && mode_type_neighbor_array->top_array[modeTypeTopNeighborIndex] != (uint8_t)INTRA_MODE)/*has_second_ref(edge_mbmi)*/) {
            // edge does not use comp pred (0/1)
            ctx = (inter_pred_dir_neighbor_array->top_array[modeTypeTopNeighborIndex] == UNI_PRED_LIST_1);
        }
        else {
            // edge uses comp pred (3)
            ctx = 3;
        }
    }
    else {  // no edges available (1)
        ctx = 1;
    }

    assert(ctx >= 0 && ctx < COMP_INTER_CONTEXTS);
    return ctx;
}
int av1_get_intra_inter_context(const MacroBlockD *xd);

int av1_get_reference_mode_context_new(const MacroBlockD *xd);

static INLINE AomCdfProb *av1_get_reference_mode_cdf(const MacroBlockD *xd) {
    return xd->tile_ctx->comp_inter_cdf[av1_get_reference_mode_context_new(xd)];
}

int av1_get_comp_reference_type_context_new(const MacroBlockD *xd);

// == Uni-directional contexts ==

int av1_get_pred_context_uni_comp_ref_p(const MacroBlockD *xd);

int av1_get_pred_context_uni_comp_ref_p1(const MacroBlockD *xd);

int av1_get_pred_context_uni_comp_ref_p2(const MacroBlockD *xd);

static INLINE AomCdfProb *av1_get_comp_reference_type_cdf(
    const MacroBlockD *xd) {
    const int pred_context = av1_get_comp_reference_type_context_new(xd);
    return xd->tile_ctx->comp_ref_type_cdf[pred_context];
}

static INLINE AomCdfProb *av1_get_pred_cdf_uni_comp_ref_p(
    const MacroBlockD *xd) {
    const int pred_context = av1_get_pred_context_uni_comp_ref_p(xd);
    return xd->tile_ctx->uni_comp_ref_cdf[pred_context][0];
}

static INLINE AomCdfProb *av1_get_pred_cdf_uni_comp_ref_p1(
    const MacroBlockD *xd) {
    const int pred_context = av1_get_pred_context_uni_comp_ref_p1(xd);
    return xd->tile_ctx->uni_comp_ref_cdf[pred_context][1];
}

static INLINE AomCdfProb *av1_get_pred_cdf_uni_comp_ref_p2(
    const MacroBlockD *xd) {
    const int pred_context = av1_get_pred_context_uni_comp_ref_p2(xd);
    return xd->tile_ctx->uni_comp_ref_cdf[pred_context][2];
}

static INLINE AomCdfProb *av1_get_pred_cdf_comp_ref_p(const MacroBlockD *xd) {
    const int pred_context = av1_get_pred_context_comp_ref_p(xd);
    return xd->tile_ctx->comp_ref_cdf[pred_context][0];
}

static INLINE AomCdfProb *av1_get_pred_cdf_comp_ref_p1(
    const MacroBlockD *xd) {
    const int pred_context = av1_get_pred_context_comp_ref_p1(xd);
    return xd->tile_ctx->comp_ref_cdf[pred_context][1];
}

static INLINE AomCdfProb *av1_get_pred_cdf_comp_ref_p2(
    const MacroBlockD *xd) {
    const int pred_context = av1_get_pred_context_comp_ref_p2(xd);
    return xd->tile_ctx->comp_ref_cdf[pred_context][2];
}

static INLINE AomCdfProb *av1_get_pred_cdf_comp_bwdref_p(
    const MacroBlockD *xd) {
    const int pred_context = av1_get_pred_context_comp_bwdref_p(xd);
    return xd->tile_ctx->comp_bwdref_cdf[pred_context][0];
}

static INLINE AomCdfProb *av1_get_pred_cdf_comp_bwdref_p1(
    const MacroBlockD *xd) {
    const int pred_context = av1_get_pred_context_comp_bwdref_p1(xd);
    return xd->tile_ctx->comp_bwdref_cdf[pred_context][1];
}

#define CHECK_BACKWARD_REFS(ref_frame) \
  (((ref_frame) >= BWDREF_FRAME) && ((ref_frame) <= ALTREF_FRAME))
#define IS_BACKWARD_REF_FRAME(ref_frame) CHECK_BACKWARD_REFS(ref_frame)

int av1_get_comp_reference_type_context_new(const MacroBlockD *xd) {
    int pred_context;
    const MbModeInfo *const above_mbmi = xd->above_mbmi;
    const MbModeInfo *const left_mbmi = xd->left_mbmi;
    const int above_in_image = xd->up_available;
    const int left_in_image = xd->left_available;

    if (above_in_image && left_in_image) {  // both edges available
        const int above_intra = !is_inter_block(above_mbmi);
        const int left_intra = !is_inter_block(left_mbmi);

        if (above_intra && left_intra) {  // intra/intra
            pred_context = 2;
        }
        else if (above_intra || left_intra) {  // intra/inter
            const MbModeInfo *inter_mbmi = above_intra ? left_mbmi : above_mbmi;

            if (!has_second_ref(inter_mbmi))  // single pred
                pred_context = 2;
            else  // comp pred
                pred_context = 1 + 2 * has_uni_comp_refs(inter_mbmi);
        }
        else {  // inter/inter
            const int a_sg = !has_second_ref(above_mbmi);
            const int l_sg = !has_second_ref(left_mbmi);
            const MvReferenceFrame frfa = above_mbmi->ref_frame[0];
            const MvReferenceFrame frfl = left_mbmi->ref_frame[0];

            if (a_sg && l_sg) {  // single/single
                pred_context = 1 + 2 * (!(IS_BACKWARD_REF_FRAME(frfa) ^
                    IS_BACKWARD_REF_FRAME(frfl)));
            }
            else if (l_sg || a_sg) {  // single/comp
                const int uni_rfc =
                    a_sg ? has_uni_comp_refs(left_mbmi) : has_uni_comp_refs(above_mbmi);

                if (!uni_rfc)  // comp bidir
                    pred_context = 1;
                else  // comp unidir
                    pred_context = 3 + (!(IS_BACKWARD_REF_FRAME(frfa) ^
                        IS_BACKWARD_REF_FRAME(frfl)));
            }
            else {  // comp/comp
                const int a_uni_rfc = has_uni_comp_refs(above_mbmi);
                const int l_uni_rfc = has_uni_comp_refs(left_mbmi);

                if (!a_uni_rfc && !l_uni_rfc)  // bidir/bidir
                    pred_context = 0;
                else if (!a_uni_rfc || !l_uni_rfc)  // unidir/bidir
                    pred_context = 2;
                else  // unidir/unidir
                    pred_context =
                    3 + (!((frfa == BWDREF_FRAME) ^ (frfl == BWDREF_FRAME)));
            }
        }
    }
    else if (above_in_image || left_in_image) {  // one edge available
        const MbModeInfo *edge_mbmi = above_in_image ? above_mbmi : left_mbmi;

        if (!is_inter_block(edge_mbmi)) {  // intra
            pred_context = 2;
        }
        else {                           // inter
            if (!has_second_ref(edge_mbmi))  // single pred
                pred_context = 2;
            else  // comp pred
                pred_context = 4 * has_uni_comp_refs(edge_mbmi);
        }
    }
    else {  // no edges available
        pred_context = 2;
    }

    assert(pred_context >= 0 && pred_context < COMP_REF_TYPE_CONTEXTS);
    return pred_context;
}

// Returns a context number for the given MB prediction signal
//
// Signal the uni-directional compound reference frame pair as either
// (BWDREF, ALTREF), or (LAST, LAST2) / (LAST, LAST3) / (LAST, GOLDEN),
// conditioning on the pair is known as uni-directional.
//
// 3 contexts: Voting is used to compare the count of forward references with
//             that of backward references from the spatial neighbors.
int av1_get_pred_context_uni_comp_ref_p(const MacroBlockD *xd) {
    const uint8_t *const ref_counts = &xd->neighbors_ref_counts[0];

    // Count of forward references (L, L2, L3, or G)
    const int frf_count = ref_counts[LAST_FRAME] + ref_counts[LAST2_FRAME] +
        ref_counts[LAST3_FRAME] + ref_counts[GOLDEN_FRAME];
    // Count of backward references (B or A)
    const int brf_count = ref_counts[BWDREF_FRAME] + ref_counts[ALTREF2_FRAME] +
        ref_counts[ALTREF_FRAME];

    const int pred_context =
        (frf_count == brf_count) ? 1 : ((frf_count < brf_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < UNI_COMP_REF_CONTEXTS);
    return pred_context;
}

// Returns a context number for the given MB prediction signal
//
// Signal the uni-directional compound reference frame pair as
// either (LAST, LAST2), or (LAST, LAST3) / (LAST, GOLDEN),
// conditioning on the pair is known as one of the above three.
//
// 3 contexts: Voting is used to compare the count of LAST2_FRAME with the
//             total count of LAST3/GOLDEN from the spatial neighbors.
int av1_get_pred_context_uni_comp_ref_p1(const MacroBlockD *xd) {
    const uint8_t *const ref_counts = &xd->neighbors_ref_counts[0];

    // Count of LAST2
    const int last2_count = ref_counts[LAST2_FRAME];
    // Count of LAST3 or GOLDEN
    const int last3_or_gld_count =
        ref_counts[LAST3_FRAME] + ref_counts[GOLDEN_FRAME];

    const int pred_context = (last2_count == last3_or_gld_count)
        ? 1
        : ((last2_count < last3_or_gld_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < UNI_COMP_REF_CONTEXTS);
    return pred_context;
}

// Returns a context number for the given MB prediction signal
//
// Signal the uni-directional compound reference frame pair as
// either (LAST, LAST3) or (LAST, GOLDEN),
// conditioning on the pair is known as one of the above two.
//
// 3 contexts: Voting is used to compare the count of LAST3_FRAME with the
//             total count of GOLDEN_FRAME from the spatial neighbors.
int av1_get_pred_context_uni_comp_ref_p2(const MacroBlockD *xd) {
    const uint8_t *const ref_counts = &xd->neighbors_ref_counts[0];

    // Count of LAST3
    const int last3_count = ref_counts[LAST3_FRAME];
    // Count of GOLDEN
    const int gld_count = ref_counts[GOLDEN_FRAME];

    const int pred_context =
        (last3_count == gld_count) ? 1 : ((last3_count < gld_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < UNI_COMP_REF_CONTEXTS);
    return pred_context;
}

int av1_get_reference_mode_context_new(const MacroBlockD *xd) {
    int ctx;
    const MbModeInfo *const above_mbmi = xd->above_mbmi;
    const MbModeInfo *const left_mbmi = xd->left_mbmi;
    const int has_above = xd->up_available;
    const int has_left = xd->left_available;

    // Note:
    // The mode info data structure has a one element border above and to the
    // left of the entries corresponding to real macroblocks.
    // The prediction flags in these dummy entries are initialized to 0.
    if (has_above && has_left) {  // both edges available
        if (!has_second_ref(above_mbmi) && !has_second_ref(left_mbmi))
            // neither edge uses comp pred (0/1)
            ctx = IS_BACKWARD_REF_FRAME(above_mbmi->ref_frame[0]) ^
            IS_BACKWARD_REF_FRAME(left_mbmi->ref_frame[0]);
        else if (!has_second_ref(above_mbmi))
            // one of two edges uses comp pred (2/3)
            ctx = 2 + (IS_BACKWARD_REF_FRAME(above_mbmi->ref_frame[0]) ||
                !is_inter_block(above_mbmi));
        else if (!has_second_ref(left_mbmi))
            // one of two edges uses comp pred (2/3)
            ctx = 2 + (IS_BACKWARD_REF_FRAME(left_mbmi->ref_frame[0]) ||
                !is_inter_block(left_mbmi));
        else  // both edges use comp pred (4)
            ctx = 4;
    }
    else if (has_above || has_left) {  // one edge available
        const MbModeInfo *edge_mbmi = has_above ? above_mbmi : left_mbmi;

        if (!has_second_ref(edge_mbmi))
            // edge does not use comp pred (0/1)
            ctx = IS_BACKWARD_REF_FRAME(edge_mbmi->ref_frame[0]);
        else
            // edge uses comp pred (3)
            ctx = 3;
    }
    else {  // no edges available (1)
        ctx = 1;
    }
    assert(ctx >= 0 && ctx < COMP_INTER_CONTEXTS);
    return ctx;
}
INLINE void av1_collect_neighbors_ref_counts_new(MacroBlockD *const xd) {
    av1_zero(xd->neighbors_ref_counts);

    uint8_t *const ref_counts = xd->neighbors_ref_counts;

    const MbModeInfo *const above_mbmi = xd->above_mbmi;
    const MbModeInfo *const left_mbmi = xd->left_mbmi;
    const int above_in_image = xd->up_available;
    const int left_in_image = xd->left_available;

    // Above neighbor
    if (above_in_image && is_inter_block(above_mbmi)) {
        ref_counts[above_mbmi->ref_frame[0]]++;
        if (has_second_ref(above_mbmi))
            ref_counts[above_mbmi->ref_frame[1]]++;
    }

    // Left neighbor
    if (left_in_image && is_inter_block(left_mbmi)) {
        ref_counts[left_mbmi->ref_frame[0]]++;
        if (has_second_ref(left_mbmi))
            ref_counts[left_mbmi->ref_frame[1]]++;
    }
}

int32_t av1_get_comp_reference_type_context(
    uint32_t                  cu_origin_x,
    uint32_t                  cu_origin_y,
    NeighborArrayUnit    *mode_type_neighbor_array,
    NeighborArrayUnit     *inter_pred_dir_neighbor_array) {
    int32_t pred_context = 0;

    uint32_t modeTypeLeftNeighborIndex = get_neighbor_array_unit_left_index(
        mode_type_neighbor_array,
        cu_origin_y);
    uint32_t modeTypeTopNeighborIndex = get_neighbor_array_unit_top_index(
        mode_type_neighbor_array,
        cu_origin_x);

    if (mode_type_neighbor_array->left_array[modeTypeLeftNeighborIndex] != (uint8_t)INVALID_MODE && mode_type_neighbor_array->top_array[modeTypeTopNeighborIndex] != (uint8_t)INVALID_MODE) {  // both edges available
        const int32_t above_intra = (mode_type_neighbor_array->top_array[modeTypeTopNeighborIndex] == (uint8_t)INTRA_MODE);
        const int32_t left_intra = (mode_type_neighbor_array->left_array[modeTypeLeftNeighborIndex] == (uint8_t)INTRA_MODE);

        if (above_intra && left_intra) {  // intra/intra
            pred_context = 2;
        }
        else if (left_intra) {  // Intra & Inter. Left is Intra, check Top

            if (inter_pred_dir_neighbor_array->top_array[modeTypeTopNeighborIndex] != BI_PRED)  // single pred
                pred_context = 2;
            else  // comp pred
                pred_context = 1 + 2 * 0/* has_uni_comp_refs(inter_mbmi)*/;
        }
        else if (above_intra) {  // Intra & Inter. Top is Intra, check Left

            if (inter_pred_dir_neighbor_array->left_array[modeTypeLeftNeighborIndex] != BI_PRED)  // single pred
                pred_context = 2;
            else  // comp pred
                pred_context = 1 + 2 * 0/* has_uni_comp_refs(inter_mbmi)*/;
        }
        else {  // inter/inter
            const int32_t a_sg = (inter_pred_dir_neighbor_array->top_array[modeTypeTopNeighborIndex] != BI_PRED);
            const int32_t l_sg = (inter_pred_dir_neighbor_array->left_array[modeTypeLeftNeighborIndex] != BI_PRED);
            //const MvReferenceFrame frfa = above_mbmi->ref_frame[0];
            //const MvReferenceFrame frfl = left_mbmi->ref_frame[0];

            if (a_sg && l_sg) {  // single/single
                pred_context = 1 + 2 * (!((inter_pred_dir_neighbor_array->top_array[modeTypeTopNeighborIndex] == UNI_PRED_LIST_1) ^
                    (inter_pred_dir_neighbor_array->left_array[modeTypeLeftNeighborIndex] == UNI_PRED_LIST_1)));
            }
            else if (l_sg || a_sg) {  // single/comp
                const int32_t uni_rfc =
                    a_sg ? 0 /*has_uni_comp_refs(left_mbmi) */ : 0 /*has_uni_comp_refs(above_mbmi)*/;

                if (!uni_rfc)  // comp bidir
                    pred_context = 1;
                else  // comp unidir
                    pred_context = 3 + (!((inter_pred_dir_neighbor_array->top_array[modeTypeTopNeighborIndex] == UNI_PRED_LIST_1) ^
                    (inter_pred_dir_neighbor_array->left_array[modeTypeLeftNeighborIndex] == UNI_PRED_LIST_1)));
            }
            else {  // comp/comp
                const int32_t a_uni_rfc = 0;// has_uni_comp_refs(above_mbmi);
                const int32_t l_uni_rfc = 0;// has_uni_comp_refs(left_mbmi);
                pred_context = 0;

                if (!a_uni_rfc && !l_uni_rfc)  // bidir/bidir
                    pred_context = 0;
                else if (!a_uni_rfc || !l_uni_rfc)  // unidir/bidir
                    pred_context = 2;
                else  // unidir/unidir
                    pred_context =
                    3 + (!(0 ^ 0));
            }
        }
    }
    else if (mode_type_neighbor_array->left_array[modeTypeLeftNeighborIndex] != (uint8_t)INVALID_MODE) {  // one edge available

        if (mode_type_neighbor_array->left_array[modeTypeLeftNeighborIndex] == (uint8_t)INTRA_MODE) {  // intra
            pred_context = 2;
        }
        else {                           // inter
            if (inter_pred_dir_neighbor_array->left_array[modeTypeLeftNeighborIndex] != BI_PRED)  // single pred
                pred_context = 2;
            else  // comp pred
                pred_context = 4 * 0/*has_uni_comp_refs(edge_mbmi)*/;
        }
    }
    else if (mode_type_neighbor_array->top_array[modeTypeTopNeighborIndex] != (uint8_t)INVALID_MODE) {  // one edge available

        if (mode_type_neighbor_array->top_array[modeTypeTopNeighborIndex] == (uint8_t)INTRA_MODE) {  // intra
            pred_context = 2;
        }
        else {                           // inter
            if (inter_pred_dir_neighbor_array->top_array[modeTypeTopNeighborIndex] != BI_PRED)  // single pred
                pred_context = 2;
            else  // comp pred
                pred_context = 4 * 0/*has_uni_comp_refs(edge_mbmi)*/;
        }
    }
    else {  // no edges available
        pred_context = 2;
    }

    //assert(pred_context >= 0 && pred_context < COMP_REF_TYPE_CONTEXTS);
    return pred_context;
}

void av1_collect_neighbors_ref_counts(
    CodingUnit            *cu_ptr,
    uint32_t                   cu_origin_x,
    uint32_t                   cu_origin_y,
    NeighborArrayUnit     *mode_type_neighbor_array,
    NeighborArrayUnit     *inter_pred_dir_neighbor_array,
    NeighborArrayUnit     *ref_frame_type_neighbor_array) {
    av1_zero(cu_ptr->av1xd->neighbors_ref_counts);

    uint8_t *const ref_counts = cu_ptr->av1xd->neighbors_ref_counts;

    uint32_t modeTypeLeftNeighborIndex = get_neighbor_array_unit_left_index(
        mode_type_neighbor_array,
        cu_origin_y);
    uint32_t modeTypeTopNeighborIndex = get_neighbor_array_unit_top_index(
        mode_type_neighbor_array,
        cu_origin_x);

    const int32_t topInter = (mode_type_neighbor_array->top_array[modeTypeTopNeighborIndex] == (uint8_t)INTER_MODE);
    const int32_t leftInter = (mode_type_neighbor_array->left_array[modeTypeLeftNeighborIndex] == (uint8_t)INTER_MODE);

    const int32_t topInImage = (mode_type_neighbor_array->top_array[modeTypeTopNeighborIndex] != (uint8_t)INVALID_MODE);
    const int32_t leftInImage = (mode_type_neighbor_array->left_array[modeTypeLeftNeighborIndex] != (uint8_t)INVALID_MODE);

    // Above neighbor
    if (topInImage && topInter) {
        MvReferenceFrame topRfType[2];
        av1_set_ref_frame(topRfType, ref_frame_type_neighbor_array->top_array[modeTypeTopNeighborIndex]);
        switch (inter_pred_dir_neighbor_array->top_array[modeTypeTopNeighborIndex]) {
        case UNI_PRED_LIST_0:
            ref_counts[topRfType[0]]++;

            break;
        case UNI_PRED_LIST_1:
            ref_counts[topRfType[0]]++;
            break;

        case BI_PRED:
            ref_counts[topRfType[0]]++;
            ref_counts[topRfType[1]]++;
            break;
        default:
            printf("ERROR[AN]: Invalid Pred Direction\n");
            break;
        }
    }

    // Left neighbor
    if (leftInImage && leftInter) {
        MvReferenceFrame leftRfType[2];
        av1_set_ref_frame(leftRfType, ref_frame_type_neighbor_array->left_array[modeTypeLeftNeighborIndex]);
        switch (inter_pred_dir_neighbor_array->left_array[modeTypeLeftNeighborIndex]) {
        case UNI_PRED_LIST_0:
            ref_counts[leftRfType[0]]++;

            break;
        case UNI_PRED_LIST_1:
            ref_counts[leftRfType[0]]++;
            break;

        case BI_PRED:
            ref_counts[leftRfType[0]]++;
            ref_counts[leftRfType[1]]++;
            break;
        default:
            printf("ERROR[AN]: Invalid Pred Direction\n");
            break;
        }
    }
}

#define WRITE_REF_BIT(bname, pname) \
  aom_write_symbol(w, bname, av1_get_pred_cdf_##pname(xd), 2)
/***************************************************************************************/

// == Common context functions for both comp and single ref ==
//
// Obtain contexts to signal a reference frame to be either LAST/LAST2 or
// LAST3/GOLDEN.
static int32_t get_pred_context_ll2_or_l3gld(const MacroBlockD *xd) {
    const uint8_t *const ref_counts = &xd->neighbors_ref_counts[0];

    // Count of LAST + LAST2
    const int32_t last_last2_count = ref_counts[LAST_FRAME] + ref_counts[LAST2_FRAME];
    // Count of LAST3 + GOLDEN
    const int32_t last3_gld_count =
        ref_counts[LAST3_FRAME] + ref_counts[GOLDEN_FRAME];

    const int32_t pred_context = (last_last2_count == last3_gld_count)
        ? 1
        : ((last_last2_count < last3_gld_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < REF_CONTEXTS);
    return pred_context;
}

// Obtain contexts to signal a reference frame to be either LAST or LAST2.
static int32_t get_pred_context_last_or_last2(const MacroBlockD *xd) {
    const uint8_t *const ref_counts = &xd->neighbors_ref_counts[0];

    // Count of LAST
    const int32_t last_count = ref_counts[LAST_FRAME];
    // Count of LAST2
    const int32_t last2_count = ref_counts[LAST2_FRAME];

    const int32_t pred_context =
        (last_count == last2_count) ? 1 : ((last_count < last2_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < REF_CONTEXTS);
    return pred_context;
}

// Obtain contexts to signal a reference frame to be either LAST3 or GOLDEN.
static int32_t get_pred_context_last3_or_gld(const MacroBlockD *xd) {
    const uint8_t *const ref_counts = &xd->neighbors_ref_counts[0];

    // Count of LAST3
    const int32_t last3_count = ref_counts[LAST3_FRAME];
    // Count of GOLDEN
    const int32_t gld_count = ref_counts[GOLDEN_FRAME];

    const int32_t pred_context =
        (last3_count == gld_count) ? 1 : ((last3_count < gld_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < REF_CONTEXTS);
    return pred_context;
}

// Obtain contexts to signal a reference frame be either BWDREF/ALTREF2, or
// ALTREF.
static int32_t get_pred_context_brfarf2_or_arf(const MacroBlockD *xd) {
    const uint8_t *const ref_counts = &xd->neighbors_ref_counts[0];

    // Counts of BWDREF, ALTREF2, or ALTREF frames (B, A2, or A)
    const int32_t brfarf2_count =
        ref_counts[BWDREF_FRAME] + ref_counts[ALTREF2_FRAME];
    const int32_t arf_count = ref_counts[ALTREF_FRAME];

    const int32_t pred_context =
        (brfarf2_count == arf_count) ? 1 : ((brfarf2_count < arf_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < REF_CONTEXTS);
    return pred_context;
}

// Obtain contexts to signal a reference frame be either BWDREF or ALTREF2.
static int32_t get_pred_context_brf_or_arf2(const MacroBlockD *xd) {
    const uint8_t *const ref_counts = &xd->neighbors_ref_counts[0];

    // Count of BWDREF frames (B)
    const int32_t brf_count = ref_counts[BWDREF_FRAME];
    // Count of ALTREF2 frames (A2)
    const int32_t arf2_count = ref_counts[ALTREF2_FRAME];

    const int32_t pred_context =
        (brf_count == arf2_count) ? 1 : ((brf_count < arf2_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < REF_CONTEXTS);
    return pred_context;
}

// == Context functions for comp ref ==
//
// Returns a context number for the given MB prediction signal
// Signal the first reference frame for a compound mode be either
// GOLDEN/LAST3, or LAST/LAST2.
int32_t av1_get_pred_context_comp_ref_p(const MacroBlockD *xd) {
    return get_pred_context_ll2_or_l3gld(xd);
}

// Returns a context number for the given MB prediction signal
// Signal the first reference frame for a compound mode be LAST,
// conditioning on that it is known either LAST/LAST2.
int32_t av1_get_pred_context_comp_ref_p1(const MacroBlockD *xd) {
    return get_pred_context_last_or_last2(xd);
}

// Returns a context number for the given MB prediction signal
// Signal the first reference frame for a compound mode be GOLDEN,
// conditioning on that it is known either GOLDEN or LAST3.
int32_t av1_get_pred_context_comp_ref_p2(const MacroBlockD *xd) {
    return get_pred_context_last3_or_gld(xd);
}

// Signal the 2nd reference frame for a compound mode be either
// ALTREF, or ALTREF2/BWDREF.
int32_t av1_get_pred_context_comp_bwdref_p(const MacroBlockD *xd) {
    return get_pred_context_brfarf2_or_arf(xd);
}

// Signal the 2nd reference frame for a compound mode be either
// ALTREF2 or BWDREF.
int32_t av1_get_pred_context_comp_bwdref_p1(const MacroBlockD *xd) {
    return get_pred_context_brf_or_arf2(xd);
}

// == Context functions for single ref ==
//
// For the bit to signal whether the single reference is a forward reference
// frame or a backward reference frame.
int32_t av1_get_pred_context_single_ref_p1(const MacroBlockD *xd) {
    const uint8_t *const ref_counts = &xd->neighbors_ref_counts[0];

    // Count of forward reference frames
    const int32_t fwd_count = ref_counts[LAST_FRAME] + ref_counts[LAST2_FRAME] +
        ref_counts[LAST3_FRAME] + ref_counts[GOLDEN_FRAME];
    // Count of backward reference frames
    const int32_t bwd_count = ref_counts[BWDREF_FRAME] + ref_counts[ALTREF2_FRAME] +
        ref_counts[ALTREF_FRAME];

    const int32_t pred_context =
        (fwd_count == bwd_count) ? 1 : ((fwd_count < bwd_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < REF_CONTEXTS);
    return pred_context;
}
static INLINE AomCdfProb *av1_get_pred_cdf_single_ref_p1(
    const MacroBlockD *xd) {
    return xd->tile_ctx
        ->single_ref_cdf[av1_get_pred_context_single_ref_p1(xd)][0];
}
static INLINE AomCdfProb *av1_get_pred_cdf_single_ref_p2(
    const MacroBlockD *xd) {
    return xd->tile_ctx
        ->single_ref_cdf[av1_get_pred_context_single_ref_p2(xd)][1];
}
static INLINE AomCdfProb *av1_get_pred_cdf_single_ref_p3(
    const MacroBlockD *xd) {
    return xd->tile_ctx
        ->single_ref_cdf[av1_get_pred_context_single_ref_p3(xd)][2];
}
static INLINE AomCdfProb *av1_get_pred_cdf_single_ref_p4(
    const MacroBlockD *xd) {
    return xd->tile_ctx
        ->single_ref_cdf[av1_get_pred_context_single_ref_p4(xd)][3];
}
static INLINE AomCdfProb *av1_get_pred_cdf_single_ref_p5(
    const MacroBlockD *xd) {
    return xd->tile_ctx
        ->single_ref_cdf[av1_get_pred_context_single_ref_p5(xd)][4];
}
static INLINE AomCdfProb *av1_get_pred_cdf_single_ref_p6(
    const MacroBlockD *xd) {
    return xd->tile_ctx
        ->single_ref_cdf[av1_get_pred_context_single_ref_p6(xd)][5];
}

// For the bit to signal whether the single reference is ALTREF_FRAME or
// non-ALTREF backward reference frame, knowing that it shall be either of
// these 2 choices.
int32_t av1_get_pred_context_single_ref_p2(const MacroBlockD *xd) {
    return get_pred_context_brfarf2_or_arf(xd);
}

// For the bit to signal whether the single reference is LAST3/GOLDEN or
// LAST2/LAST, knowing that it shall be either of these 2 choices.
int32_t av1_get_pred_context_single_ref_p3(const MacroBlockD *xd) {
    return get_pred_context_ll2_or_l3gld(xd);
}

// For the bit to signal whether the single reference is LAST2_FRAME or
// LAST_FRAME, knowing that it shall be either of these 2 choices.
int32_t av1_get_pred_context_single_ref_p4(const MacroBlockD *xd) {
    return get_pred_context_last_or_last2(xd);
}

// For the bit to signal whether the single reference is GOLDEN_FRAME or
// LAST3_FRAME, knowing that it shall be either of these 2 choices.
int32_t av1_get_pred_context_single_ref_p5(const MacroBlockD *xd) {
    return get_pred_context_last3_or_gld(xd);
}

// For the bit to signal whether the single reference is ALTREF2_FRAME or
// BWDREF_FRAME, knowing that it shall be either of these 2 choices.
int32_t av1_get_pred_context_single_ref_p6(const MacroBlockD *xd) {
    return get_pred_context_brf_or_arf2(xd);
}
/***************************************************************************************/

static void write_ref_frames(
    FRAME_CONTEXT               *frame_context,
    PictureParentControlSet   *pcs_ptr,
    const MacroBlockD           *xd,
    AomWriter                  *w) {
    FrameHeader *frm_hdr = &pcs_ptr->frm_hdr;
    const MbModeInfo *const mbmi = &xd->mi[0]->mbmi;
    const int is_compound = has_second_ref(mbmi);
    UNUSED(frame_context);
    {
        // does the feature use compound prediction or not
        // (if not specified at the frame/segment level)
        if (frm_hdr->reference_mode == REFERENCE_MODE_SELECT) {
            if (is_comp_ref_allowed(mbmi->sb_type))
                aom_write_symbol(w, is_compound, av1_get_reference_mode_cdf(xd), 2);
        }
        else {
            assert((!is_compound) ==
                (frm_hdr->reference_mode == SINGLE_REFERENCE));
        }

        if (is_compound) {
            const CompReferenceType comp_ref_type = has_uni_comp_refs(mbmi)
                ? UNIDIR_COMP_REFERENCE
                : BIDIR_COMP_REFERENCE;
            aom_write_symbol(w, comp_ref_type, av1_get_comp_reference_type_cdf(xd),
                2);

            if (comp_ref_type == UNIDIR_COMP_REFERENCE) {
                const int bit = mbmi->ref_frame[0] == BWDREF_FRAME;
                WRITE_REF_BIT(bit, uni_comp_ref_p);

                if (!bit) {
                    assert(mbmi->ref_frame[0] == LAST_FRAME);
                    const int bit1 = mbmi->ref_frame[1] == LAST3_FRAME ||
                        mbmi->ref_frame[1] == GOLDEN_FRAME;
                    WRITE_REF_BIT(bit1, uni_comp_ref_p1);
                    if (bit1) {
                        const int bit2 = mbmi->ref_frame[1] == GOLDEN_FRAME;
                        WRITE_REF_BIT(bit2, uni_comp_ref_p2);
                    }
                }
                else
                    assert(mbmi->ref_frame[1] == ALTREF_FRAME);
                return;
            }

            assert(comp_ref_type == BIDIR_COMP_REFERENCE);

            const int bit = (mbmi->ref_frame[0] == GOLDEN_FRAME ||
                mbmi->ref_frame[0] == LAST3_FRAME);
            WRITE_REF_BIT(bit, comp_ref_p);

            if (!bit) {
                const int bit1 = mbmi->ref_frame[0] == LAST2_FRAME;
                WRITE_REF_BIT(bit1, comp_ref_p1);
            }
            else {
                const int bit2 = mbmi->ref_frame[0] == GOLDEN_FRAME;
                WRITE_REF_BIT(bit2, comp_ref_p2);
            }

            const int bit_bwd = mbmi->ref_frame[1] == ALTREF_FRAME;
            WRITE_REF_BIT(bit_bwd, comp_bwdref_p);

            if (!bit_bwd)
                WRITE_REF_BIT(mbmi->ref_frame[1] == ALTREF2_FRAME, comp_bwdref_p1);
        }
        else {
            const int bit0 = (mbmi->ref_frame[0] <= ALTREF_FRAME &&
                mbmi->ref_frame[0] >= BWDREF_FRAME);
            WRITE_REF_BIT(bit0, single_ref_p1);

            if (bit0) {
                const int bit1 = mbmi->ref_frame[0] == ALTREF_FRAME;
                WRITE_REF_BIT(bit1, single_ref_p2);
                if (!bit1)
                    WRITE_REF_BIT(mbmi->ref_frame[0] == ALTREF2_FRAME, single_ref_p6);
            }
            else {
                const int bit2 = (mbmi->ref_frame[0] == LAST3_FRAME ||
                    mbmi->ref_frame[0] == GOLDEN_FRAME);
                WRITE_REF_BIT(bit2, single_ref_p3);
                if (!bit2) {
                    const int bit3 = mbmi->ref_frame[0] != LAST_FRAME;
                    WRITE_REF_BIT(bit3, single_ref_p4);
                }
                else {
                    const int bit4 = mbmi->ref_frame[0] != LAST3_FRAME;
                    WRITE_REF_BIT(bit4, single_ref_p5);
                }
            }
        }
    }
}
static void encode_restoration_mode(PictureParentControlSet *pcs_ptr,
    struct AomWriteBitBuffer *wb) {
    Av1Common* cm = pcs_ptr->av1_cm;
    FrameHeader *frm_hdr = &pcs_ptr->frm_hdr;
    //printf("ERROR[AN]: encode_restoration_mode might not work. Double check the reference code\n");
    assert(!frm_hdr->all_lossless);
    // move out side of the function
    //if (!cm->seq_params.enable_restoration) return;

    if (frm_hdr->allow_intrabc) return;

    const int32_t num_planes = 3;// av1_num_planes(cm);
    int32_t all_none = 1, chroma_none = 1;
    for (int32_t p = 0; p < num_planes; ++p) {
        //   aom_wb_write_bit(wb, 0);
        //   aom_wb_write_bit(wb, 0);

        RestorationInfo *rsi = &pcs_ptr->av1_cm->rst_info[p];

        //if (p==0)
        //   printf("POC:%i Luma restType:%i\n", pcs_ptr->picture_number, rsi->frame_restoration_type);
        //if (p == 1)
        //    printf("POC:%i Cb restType:%i\n", pcs_ptr->picture_number, rsi->frame_restoration_type);
        //if (p == 2)
        //    printf("POC:%i Cr restType:%i\n", pcs_ptr->picture_number, rsi->frame_restoration_type);

        if (rsi->frame_restoration_type != RESTORE_NONE) {
            all_none = 0;
            chroma_none &= (int32_t)(p == 0);
        }
        switch (rsi->frame_restoration_type) {
        case RESTORE_NONE:
            aom_wb_write_bit(wb, 0);
            aom_wb_write_bit(wb, 0);
            break;
        case RESTORE_WIENER:
            aom_wb_write_bit(wb, 1);
            aom_wb_write_bit(wb, 0);
            break;
        case RESTORE_SGRPROJ:
            aom_wb_write_bit(wb, 1);
            aom_wb_write_bit(wb, 1);
            break;
        case RESTORE_SWITCHABLE:
            aom_wb_write_bit(wb, 0);
            aom_wb_write_bit(wb, 1);
            break;
        default: assert(0);
        }
    }
    if (!all_none) {
        //  assert(cm->seq_params.sb_size == BLOCK_64X64 ||
        //      cm->seq_params.sb_size == BLOCK_128X128);
        const int32_t sb_size = pcs_ptr->sequence_control_set_ptr->seq_header.sb_size == BLOCK_128X128 ? 128 : 64;;
        RestorationInfo *rsi = &pcs_ptr->av1_cm->rst_info[0];

        assert(rsi->restoration_unit_size >= sb_size);
        assert(RESTORATION_UNITSIZE_MAX == 256);

        if (sb_size == 64)
            aom_wb_write_bit(wb, rsi->restoration_unit_size > 64);
        if (rsi->restoration_unit_size > 64)
            aom_wb_write_bit(wb, rsi->restoration_unit_size > 128);
    }

    if (num_planes > 1) {
        int32_t s = 1;// AOMMIN(cm->subsampling_x, cm->subsampling_y);
        if (s && !chroma_none) {
            aom_wb_write_bit(wb, cm->rst_info[1].restoration_unit_size !=
                cm->rst_info[0].restoration_unit_size);
            assert(cm->rst_info[1].restoration_unit_size ==
                cm->rst_info[0].restoration_unit_size ||
                cm->rst_info[1].restoration_unit_size ==
                (cm->rst_info[0].restoration_unit_size >> s));
            assert(cm->rst_info[2].restoration_unit_size ==
                cm->rst_info[1].restoration_unit_size);
        }
        else if (!s) {
            assert(cm->rst_info[1].restoration_unit_size ==
                cm->rst_info[0].restoration_unit_size);
            assert(cm->rst_info[2].restoration_unit_size ==
                cm->rst_info[1].restoration_unit_size);
        }
    }
}


static void encode_segmentation(PictureParentControlSet *pcsPtr, struct AomWriteBitBuffer *wb) {
    SegmentationParams *segmentation_params = &pcsPtr->frm_hdr.segmentation_params;
    aom_wb_write_bit(wb, segmentation_params->segmentation_enabled);
    if (segmentation_params->segmentation_enabled) {
        if (!(pcsPtr->frm_hdr.primary_ref_frame==PRIMARY_REF_NONE)) {
            aom_wb_write_bit(wb, segmentation_params->segmentation_update_map);
            if (segmentation_params->segmentation_update_map) {
                aom_wb_write_bit(wb, segmentation_params->segmentation_temporal_update);
            }
            aom_wb_write_bit(wb, segmentation_params->segmentation_update_map);
        }
        if (segmentation_params->segmentation_update_map) {
            for (int i = 0; i < MAX_SEGMENTS; i++) {
                for (int j = 0; j < SEG_LVL_MAX; j++) {
                    aom_wb_write_bit(wb, segmentation_params->feature_enabled[i][j]);
                    if (segmentation_params->feature_enabled[i][j]) {
                        //TODO: add clamping
                        if (segmentation_feature_signed[j]) {
                            aom_wb_write_inv_signed_literal(wb, segmentation_params->feature_data[i][j],
                                                            segmentation_feature_bits[j]);
                        } else {
                            aom_wb_write_literal(wb, segmentation_params->feature_data[i][j],
                                                 segmentation_feature_bits[j]);
                        }
                    }
                }
            }
        }

    }

}


static void encode_loopfilter(PictureParentControlSet *pcs_ptr, struct AomWriteBitBuffer *wb) {
    FrameHeader *frm_hdr = &pcs_ptr->frm_hdr;
    assert(!frm_hdr->coded_lossless);
    if (frm_hdr->allow_intrabc) return;
    const int32_t num_planes = 3;// av1_num_planes(pcs_ptr);

    struct LoopFilter *lf = &frm_hdr->loop_filter_params;

    // Encode the loop filter level and type
    aom_wb_write_literal(wb, lf->filter_level[0], 6);
    aom_wb_write_literal(wb, lf->filter_level[1], 6);
    if (num_planes > 1) {
        if (lf->filter_level[0] || lf->filter_level[1]) {
            aom_wb_write_literal(wb, lf->filter_level_u, 6);
            aom_wb_write_literal(wb, lf->filter_level_v, 6);
        }
    }
    aom_wb_write_literal(wb, lf->sharpness_level, 3);

    // Write out loop filter deltas applied at the MB level based on mode or
    // ref frame (if they are enabled).
    aom_wb_write_bit(wb, lf->mode_ref_delta_enabled);
    if (lf->mode_ref_delta_enabled) {
        printf("ERROR[AN]: Loop Filter is not supported yet \n");
        /* aom_wb_write_bit(wb, lf->mode_ref_delta_update);
        if (lf->mode_ref_delta_update) {
        const int32_t prime_idx = pcs_ptr->primary_ref_frame;
        const int32_t buf_idx =
        prime_idx == PRIMARY_REF_NONE ? -1 : cm->frame_refs[prime_idx].idx;
        int8_t last_ref_deltas[TOTAL_REFS_PER_FRAME];
        if (prime_idx == PRIMARY_REF_NONE || buf_idx < 0) {
        av1_set_default_ref_deltas(last_ref_deltas);
        } else {
        memcpy(last_ref_deltas, cm->buffer_pool->frame_bufs[buf_idx].ref_deltas,
        TOTAL_REFS_PER_FRAME);
        }
        for (i = 0; i < TOTAL_REFS_PER_FRAME; i++) {
        const int32_t delta = lf->ref_deltas[i];
        const int32_t changed = delta != last_ref_deltas[i];
        aom_wb_write_bit(wb, changed);
        if (changed) aom_wb_write_inv_signed_literal(wb, delta, 6);
        }
        int8_t last_mode_deltas[MAX_MODE_LF_DELTAS];
        if (prime_idx == PRIMARY_REF_NONE || buf_idx < 0) {
        av1_set_default_mode_deltas(last_mode_deltas);
        } else {
        memcpy(last_mode_deltas,
        cm->buffer_pool->frame_bufs[buf_idx].mode_deltas,
        MAX_MODE_LF_DELTAS);
        }

        for (i = 0; i < MAX_MODE_LF_DELTAS; i++) {
        const int32_t delta = lf->mode_deltas[i];
        const int32_t changed = delta != last_mode_deltas[i];
        aom_wb_write_bit(wb, changed);
        if (changed) aom_wb_write_inv_signed_literal(wb, delta, 6);
        }
        }*/
    }
}

static void encode_cdef(const PictureParentControlSet *pcs_ptr, struct AomWriteBitBuffer *wb) {
    //assert(!cm->coded_lossless);
    // moved out side
    //if (!cm->seq_params.enable_cdef) return;

    const FrameHeader *frm_hdr = &pcs_ptr->frm_hdr;

    if (frm_hdr->allow_intrabc) return;

    const int32_t num_planes = 3;// av1_num_planes(pcs_ptr);
    int32_t i;
    aom_wb_write_literal(wb, frm_hdr->CDEF_params.cdef_damping - 3, 2);
    //cdef_pri_damping & cdef_sec_damping consolidated to cdef_damping
    //assert(pcs_ptr->cdef_pri_damping == pcs_ptr->cdef_sec_damping);
    aom_wb_write_literal(wb, frm_hdr->CDEF_params.cdef_bits, 2);
    for (i = 0; i < pcs_ptr->nb_cdef_strengths; i++) {
        aom_wb_write_literal(wb, frm_hdr->CDEF_params.cdef_y_strength[i], CDEF_STRENGTH_BITS);
        if (num_planes > 1)
            aom_wb_write_literal(wb, frm_hdr->CDEF_params.cdef_uv_strength[i], CDEF_STRENGTH_BITS);
    }
}

static void write_delta_q(struct AomWriteBitBuffer *wb, int32_t delta_q) {
    if (delta_q != 0) {
        aom_wb_write_bit(wb, 1);
        aom_wb_write_inv_signed_literal(wb, delta_q, 6);
    }
    else
        aom_wb_write_bit(wb, 0);
}

static void encode_quantization(const PictureParentControlSet *const pcs_ptr,
    struct AomWriteBitBuffer *wb) {
    const int32_t num_planes = 3;//av1_num_planes(cm);

    const FrameHeader *frm_hdr = &pcs_ptr->frm_hdr;
    aom_wb_write_literal(wb, frm_hdr->quantization_params.base_q_idx, QINDEX_BITS);
    write_delta_q(wb, frm_hdr->quantization_params.delta_q_y_dc);
    if (num_planes > 1) {
        int32_t diff_uv_delta = (frm_hdr->quantization_params.delta_q_u_dc != frm_hdr->quantization_params.delta_q_v_dc) ||
            (frm_hdr->quantization_params.delta_q_u_ac != frm_hdr->quantization_params.delta_q_v_ac);
        if (pcs_ptr->separate_uv_delta_q) aom_wb_write_bit(wb, diff_uv_delta);
        write_delta_q(wb, frm_hdr->quantization_params.delta_q_u_dc);
        write_delta_q(wb, frm_hdr->quantization_params.delta_q_u_ac);
        if (diff_uv_delta) {
            write_delta_q(wb, frm_hdr->quantization_params.delta_q_v_dc);
            write_delta_q(wb, frm_hdr->quantization_params.delta_q_v_ac);
        }
    }
    aom_wb_write_bit(wb, frm_hdr->quantization_params.using_qmatrix);
    if (frm_hdr->quantization_params.using_qmatrix) {
        aom_wb_write_literal(wb, frm_hdr->quantization_params.qm_y, QM_LEVEL_BITS);
        aom_wb_write_literal(wb, frm_hdr->quantization_params.qm_u, QM_LEVEL_BITS);
        if (!pcs_ptr->separate_uv_delta_q)
            assert(frm_hdr->quantization_params.qm_u == frm_hdr->quantization_params.qm_v);
        else
            aom_wb_write_literal(wb, frm_hdr->quantization_params.qm_v, QM_LEVEL_BITS);
    }
}

static void write_tile_info_max_tile(const PictureParentControlSet *const pcs_ptr,
    struct AomWriteBitBuffer *wb) {
    Av1Common * cm = pcs_ptr->av1_cm;
    aom_wb_write_bit(wb, cm->tiles_info.uniform_tile_spacing_flag);

    if (cm->tiles_info.uniform_tile_spacing_flag) {
        // Uniform spaced tiles with power-of-two number of rows and columns
        // tile columns
        int32_t ones = cm->log2_tile_cols - cm->tiles_info.min_log2_tile_cols;
        while (ones--)
            aom_wb_write_bit(wb, 1);
        if (cm->log2_tile_cols < cm->tiles_info.max_log2_tile_cols)
            aom_wb_write_bit(wb, 0);
        // rows
        ones = cm->log2_tile_rows - cm->tiles_info.min_log2_tile_rows;
        while (ones--)
            aom_wb_write_bit(wb, 1);
        if (cm->log2_tile_rows < cm->tiles_info.max_log2_tile_rows)
            aom_wb_write_bit(wb, 0);
    }
    else {
        // Explicit tiles with configurable tile widths and heights
        printf("ERROR[AN]:  NON uniform_tile_spacing_flag not supported yet\n");
        //// columns
        //for (i = 0; i < cm->tile_cols; i++) {
        //    size_sb = cm->tile_col_start_sb[i + 1] - cm->tile_col_start_sb[i];
        //    wb_write_uniform(wb, AOMMIN(width_sb, cm->max_tile_width_sb),
        //        size_sb - 1);
        //    width_sb -= size_sb;
        //}
        //assert(width_sb == 0);

        //// rows
        //for (i = 0; i < cm->tile_rows; i++) {
        //    size_sb = cm->tile_row_start_sb[i + 1] - cm->tile_row_start_sb[i];
        //    wb_write_uniform(wb, AOMMIN(height_sb, cm->max_tile_height_sb),
        //        size_sb - 1);
        //    height_sb -= size_sb;
        //}
        //assert(height_sb == 0);
    }
}

static int32_t tile_log2(int32_t blk_size, int32_t target) {
    int32_t k;
    for (k = 0; (blk_size << k) < target; k++) {
    }
    return k;
}

void av1_get_tile_limits(PictureParentControlSet * pcs_ptr) {
    Av1Common * cm = pcs_ptr->av1_cm;

    int32_t mi_cols = ALIGN_POWER_OF_TWO(cm->mi_cols, pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2);
    int32_t mi_rows = ALIGN_POWER_OF_TWO(cm->mi_rows, pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2);
    int32_t sb_cols = mi_cols >> pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2;
    int32_t sb_rows = mi_rows >> pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2;

    int32_t sb_size_log2 = pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2 + MI_SIZE_LOG2;
    cm->tiles_info.max_tile_width_sb = MAX_TILE_WIDTH >> sb_size_log2;
    int32_t max_tile_area_sb = MAX_TILE_AREA >> (2 * sb_size_log2);

    cm->tiles_info.min_log2_tile_cols = tile_log2(cm->tiles_info.max_tile_width_sb, sb_cols);
    cm->tiles_info.max_log2_tile_cols = tile_log2(1, AOMMIN(sb_cols, MAX_TILE_COLS));
    cm->tiles_info.max_log2_tile_rows = tile_log2(1, AOMMIN(sb_rows, MAX_TILE_ROWS));
    cm->tiles_info.min_log2_tile_rows = 0; // CHKN Tiles
    cm->tiles_info.min_log2_tiles = tile_log2(max_tile_area_sb, sb_cols * sb_rows);
    cm->tiles_info.min_log2_tiles = AOMMAX(cm->tiles_info.min_log2_tiles, cm->tiles_info.min_log2_tile_cols);
}

void av1_calculate_tile_cols(PictureParentControlSet * pcs_ptr) {
    Av1Common *const cm = pcs_ptr->av1_cm;

    int mi_cols = ALIGN_POWER_OF_TWO(cm->mi_cols, pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2);
    int mi_rows = ALIGN_POWER_OF_TWO(cm->mi_rows, pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2);
    int sb_cols = mi_cols >> pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2;
    int sb_rows = mi_rows >> pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2;
    int i;

    if (cm->tiles_info.uniform_tile_spacing_flag) {
        int start_sb;
        int size_sb = ALIGN_POWER_OF_TWO(sb_cols, cm->log2_tile_cols);
        size_sb >>= cm->log2_tile_cols;
        assert(size_sb > 0);
        for (i = 0, start_sb = 0; start_sb < sb_cols; i++) {
            cm->tiles_info.tile_col_start_sb[i] = start_sb;
            start_sb += size_sb;
        }
        cm->tiles_info.tile_cols = i;
        cm->tiles_info.tile_col_start_sb[i] = sb_cols;
        cm->tiles_info.min_log2_tile_rows = AOMMAX(cm->tiles_info.min_log2_tiles - cm->log2_tile_cols, 0);
        cm->tiles_info.max_tile_height_sb = sb_rows >> cm->tiles_info.min_log2_tile_rows;

        cm->tile_width = size_sb << pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2;
        cm->tile_width = AOMMIN(cm->tile_width, cm->mi_cols);
    }
    else {
        int max_tile_area_sb = (sb_rows * sb_cols);
        int widest_tile_sb = 1;
        cm->log2_tile_cols = tile_log2(1, cm->tiles_info.tile_cols);
        for (i = 0; i < cm->tiles_info.tile_cols; i++) {
            int size_sb = cm->tiles_info.tile_col_start_sb[i + 1] - cm->tiles_info.tile_col_start_sb[i];
            widest_tile_sb = AOMMAX(widest_tile_sb, size_sb);
        }
        if (cm->tiles_info.min_log2_tiles)
            max_tile_area_sb >>= (cm->tiles_info.min_log2_tiles + 1);

        cm->tiles_info.max_tile_height_sb = AOMMAX(max_tile_area_sb / widest_tile_sb, 1);
    }
}

void av1_calculate_tile_rows(PictureParentControlSet * pcs_ptr)
{
    Av1Common *const cm = pcs_ptr->av1_cm;

    int mi_rows = ALIGN_POWER_OF_TWO(cm->mi_rows, pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2);
    int sb_rows = mi_rows >> pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2;
    int start_sb, size_sb, i;

    if (cm->tiles_info.uniform_tile_spacing_flag) {
        size_sb = ALIGN_POWER_OF_TWO(sb_rows, cm->log2_tile_rows);
        size_sb >>= cm->log2_tile_rows;
        assert(size_sb > 0);
        for (i = 0, start_sb = 0; start_sb < sb_rows; i++) {
            cm->tiles_info.tile_row_start_sb[i] = start_sb;
            start_sb += size_sb;
        }
        cm->tiles_info.tile_rows = i;
        cm->tiles_info.tile_row_start_sb[i] = sb_rows;

        cm->tile_height = size_sb << pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2;
        cm->tile_height = AOMMIN(cm->tile_height, cm->mi_rows);
    }
    else
        cm->log2_tile_rows = tile_log2(1, cm->tiles_info.tile_rows);
}

 void set_tile_info(PictureParentControlSet * pcs_ptr)
{
     /*  Tiling algorithm:
        input : log2_tile_count ==> tile_count = 1<<log2_tile_count

        step1) compute pic_size_in_sb
        step2) then round up to the closed n.tile_count.
        step3) tile_size = rounded_pic_size_in_sb / tile_count.
        step4) we fill tiles of size tile_size until we reach the end of the pic

        Note that: the last tile could have smaller size, and the final number
        of tiles could be less than tile_count
     */

    Av1Common * cm = pcs_ptr->av1_cm;
    int i, start_sb;
    //to connect later if non uniform tile spacing is needed.
    int tile_width_count = 0;
    int tile_height_count= 0;
    int tile_widths[MAX_TILE_COLS] = {0};
    int tile_heights[MAX_TILE_ROWS] = { 0 };

    av1_get_tile_limits(pcs_ptr);

    // configure tile columns
    if (tile_width_count == 0 || tile_height_count == 0)
    {
        cm->tiles_info.uniform_tile_spacing_flag = 1;
        cm->log2_tile_cols = AOMMAX(pcs_ptr->sequence_control_set_ptr->static_config.tile_columns, cm->tiles_info.min_log2_tile_cols);
        cm->log2_tile_cols = AOMMIN(cm->log2_tile_cols, cm->tiles_info.max_log2_tile_cols);
    }
    else {
        int mi_cols = ALIGN_POWER_OF_TWO(cm->mi_cols, pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2);
        int sb_cols = mi_cols >> pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2;
        int size_sb, j = 0;
        cm->tiles_info.uniform_tile_spacing_flag = 0;
        for (i = 0, start_sb = 0; start_sb < sb_cols && i < MAX_TILE_COLS; i++) {
            cm->tiles_info.tile_col_start_sb[i] = start_sb;
            size_sb = tile_widths[j++];
            if (j >= tile_width_count) j = 0;
            start_sb += AOMMIN(size_sb, cm->tiles_info.max_tile_width_sb);
        }
        cm->tiles_info.tile_cols = i;
        cm->tiles_info.tile_col_start_sb[i] = sb_cols;
    }
    av1_calculate_tile_cols(pcs_ptr);

    // configure tile rows
    if (cm->tiles_info.uniform_tile_spacing_flag) {
        cm->log2_tile_rows = AOMMAX(pcs_ptr->sequence_control_set_ptr->static_config.tile_rows, cm->tiles_info.min_log2_tile_rows);
        cm->log2_tile_rows = AOMMIN(cm->log2_tile_rows, cm->tiles_info.max_log2_tile_rows);
    }
    else {
        int mi_rows = ALIGN_POWER_OF_TWO(cm->mi_rows, pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2);
        int sb_rows = mi_rows >> pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2;
        int size_sb, j = 0;
        for (i = 0, start_sb = 0; start_sb < sb_rows && i < MAX_TILE_ROWS; i++) {
            cm->tiles_info.tile_row_start_sb[i] = start_sb;
            size_sb = tile_heights[j++];
            if (j >= tile_height_count) j = 0;
            start_sb += AOMMIN(size_sb, cm->tiles_info.max_tile_height_sb);
        }
        cm->tiles_info.tile_rows = i;
        cm->tiles_info.tile_row_start_sb[i] = sb_rows;
    }
    av1_calculate_tile_rows(pcs_ptr);
}

 void av1_tile_set_row(TileInfo *tile, PictureParentControlSet * pcs_ptr, int row)
 {
     Av1Common *const cm = pcs_ptr->av1_cm;

     assert(row < cm->tiles_info.tile_rows);
     int mi_row_start = cm->tiles_info.tile_row_start_sb[row]    << pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2;
     int mi_row_end  = cm->tiles_info.tile_row_start_sb[row + 1] << pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2;
     tile->tile_row = row;
     tile->mi_row_start = mi_row_start;
     tile->mi_row_end = AOMMIN(mi_row_end, cm->mi_rows);
     tile->tg_horz_boundary = 0;
     assert(tile->mi_row_end > tile->mi_row_start);
 }

 void av1_tile_set_col(TileInfo *tile, PictureParentControlSet * pcs_ptr, int col) {
     Av1Common *const cm = pcs_ptr->av1_cm;
     assert(col < cm->tiles_info.tile_cols);
     int mi_col_start = cm->tiles_info.tile_col_start_sb[col] << pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2;
     int mi_col_end = cm->tiles_info.tile_col_start_sb[col + 1]
         << pcs_ptr->sequence_control_set_ptr->seq_header.sb_size_log2;
     tile->tile_col = col;
     tile->mi_col_start = mi_col_start;
     tile->mi_col_end = AOMMIN(mi_col_end, cm->mi_cols);
     assert(tile->mi_col_end > tile->mi_col_start);
 }

static void write_tile_info(const PictureParentControlSet *const pcs_ptr,
    //struct AomWriteBitBuffer *saved_wb,
    struct AomWriteBitBuffer *wb) {
    av1_get_tile_limits((PictureParentControlSet *)pcs_ptr);
    write_tile_info_max_tile(pcs_ptr, wb);

    if (pcs_ptr->av1_cm->tiles_info.tile_rows * pcs_ptr->av1_cm->tiles_info.tile_cols > 1) {
        // tile id used for cdf update
        aom_wb_write_literal(wb, 0, pcs_ptr->av1_cm->log2_tile_cols + pcs_ptr->av1_cm->log2_tile_rows);
        // Number of bytes in tile size - 1
        aom_wb_write_literal(wb, 3, 2);
    }
}

static void write_frame_size(PictureParentControlSet *pcs_ptr,
    int32_t frame_size_override,
    struct AomWriteBitBuffer *wb) {
    SequenceControlSet *scs_ptr = (SequenceControlSet*)pcs_ptr->sequence_control_set_wrapper_ptr->object_ptr;
    (void)(*pcs_ptr);
    (void)frame_size_override;
    //const int32_t coded_width = cm->superres_upscaled_width - 1;
    //const int32_t coded_height = cm->superres_upscaled_height - 1;

    //if (frame_size_override) {
    //    const SequenceHeader *seq_params = &cm->seq_params;
    //    int32_t num_bits_width = seq_params->num_bits_width;
    //    int32_t num_bits_height = seq_params->num_bits_height;
    //    aom_wb_write_literal(wb, coded_width, num_bits_width);
    //    aom_wb_write_literal(wb, coded_height, num_bits_height);
    //}
    if (scs_ptr->seq_header.enable_superres) {
        printf("ERROR[AN]: enable_superres not supported yet\n");
        //write_superres_scale(cm, wb);
    }

    aom_wb_write_bit(wb, 0);
    //write_render_size(cm, wb);
}

static void WriteProfile(BitstreamProfile profile,
    struct AomWriteBitBuffer *wb) {
    assert(profile >= PROFILE_0 && profile < MAX_PROFILES);
    aom_wb_write_literal(wb, profile, PROFILE_BITS);
}

static void write_bitdepth(SequenceControlSet *scs_ptr/*Av1Common *const cm*/,
    struct AomWriteBitBuffer *wb) {
    // Profile 0/1: [0] for 8 bit, [1]  10-bit
    // Profile   2: [0] for 8 bit, [10] 10-bit, [11] - 12-bit
    aom_wb_write_bit(wb, scs_ptr->static_config.encoder_bit_depth == EB_8BIT ? 0 : 1);
    if (scs_ptr->static_config.profile == PROFILE_2 && scs_ptr->static_config.encoder_bit_depth != EB_8BIT) {
        printf("ERROR[AN]: Profile 2 Not supported\n");
        aom_wb_write_bit(wb, scs_ptr->static_config.encoder_bit_depth == EB_10BIT ? 0 : 1);
    }
}
static void write_color_config(
    SequenceControlSet *scs_ptr/*Av1Common *const cm*/, struct AomWriteBitBuffer *wb) {
    //write_bitdepth(cm, wb);
    write_bitdepth(scs_ptr, wb);
    const int32_t is_monochrome = 0;// cm->seq_params.monochrome;
    // monochrome bit
    aom_wb_write_bit(wb, is_monochrome);
    //if (cm->profile != PROFILE_1)
    //    aom_wb_write_bit(wb, is_monochrome);
    //else
    //    assert(!is_monochrome);
    if (1/*cm->color_primaries == AOM_CICP_CP_UNSPECIFIED &&
         cm->transfer_characteristics == AOM_CICP_TC_UNSPECIFIED &&
         cm->matrix_coefficients == AOM_CICP_MC_UNSPECIFIED*/) {
        aom_wb_write_bit(wb, 0);  // No color description present
    }
    else {
        //aom_wb_write_bit(wb, 1);  // Color description present
        //aom_wb_write_literal(wb, cm->color_primaries, 8);
        //aom_wb_write_literal(wb, cm->transfer_characteristics, 8);
        //aom_wb_write_literal(wb, cm->matrix_coefficients, 8);
    }
    if (is_monochrome) {
        printf("ERROR[AN]: is_monochrome not supported yet\n");
        return;
    }
    if (0/*cm->color_primaries == EB_CICP_CP_BT_709 &&
         cm->transfer_characteristics == EB_CICP_TC_SRGB &&
         cm->matrix_coefficients ==
         AOM_CICP_MC_IDENTITY*/) {  // it would be better to remove this
         // dependency too
         //assert(cm->subsampling_x == 0 && cm->subsampling_y == 0);
         //assert(cm->profile == PROFILE_1 ||
         //    (cm->profile == PROFILE_2 && cm->bit_depth == AOM_BITS_12));
    }
    else {
        // 0: [16, 235] (i.e. xvYCC), 1: [0, 255]
        aom_wb_write_bit(wb, 0);
        //aom_wb_write_bit(wb, cm->color_range);

        //if (cm->profile == PROFILE_0) {
        //    // 420 only
        //    assert(cm->subsampling_x == 1 && cm->subsampling_y == 1);
        //}
        //else if (cm->profile == PROFILE_1) {
        //    // 444 only
        //    assert(cm->subsampling_x == 0 && cm->subsampling_y == 0);
        //}
        //else if (cm->profile == PROFILE_2) {
        //    if (cm->bit_depth == AOM_BITS_12) {
        //        // 420, 444 or 422
        //        aom_wb_write_bit(wb, cm->subsampling_x);
        //        if (cm->subsampling_x == 0) {
        //            assert(cm->subsampling_y == 0 &&
        //                "4:4:0 subsampling not allowed in AV1");
        //        }
        //        else {
        //            aom_wb_write_bit(wb, cm->subsampling_y);
        //        }
        //    }
        //    else {
        //        // 422 only
        //        assert(cm->subsampling_x == 1 && cm->subsampling_y == 0);
        //    }
        //}
        //if (cm->matrix_coefficients == AOM_CICP_MC_IDENTITY) {
        //    assert(cm->subsampling_x == 0 && cm->subsampling_y == 0);
        //}
        aom_wb_write_literal(wb, 0, 2);
        //if (cm->subsampling_x == 1 && cm->subsampling_y == 1) {
        //    aom_wb_write_literal(wb, cm->chroma_sample_position, 2);
        //}
    }
    aom_wb_write_bit(wb, 0);
    //aom_wb_write_bit(wb, cm->separate_uv_delta_q);
}

void write_sequence_header(SequenceControlSet *scs_ptr/*Av1Comp *cpi*/, struct AomWriteBitBuffer *wb) {
    //    Av1Common *const cm = &cpi->common;
    //    SequenceHeader *seq_params = &cm->seq_params;
    //

    int32_t max_frame_width = scs_ptr->seq_header.max_frame_width;
    /*        cpi->oxcf.forced_max_frame_width
    ? cpi->oxcf.forced_max_frame_width
    : cpi->oxcf.width;*/
    int32_t max_frame_height = scs_ptr->seq_header.max_frame_height;
    /*cpi->oxcf.forced_max_frame_height
    ? cpi->oxcf.forced_max_frame_height
    : cpi->oxcf.height;*/

    aom_wb_write_literal(wb, scs_ptr->seq_header.frame_width_bits - 1, 4);
    aom_wb_write_literal(wb, scs_ptr->seq_header.frame_height_bits - 1, 4);
    aom_wb_write_literal(wb, max_frame_width - 1, scs_ptr->seq_header.frame_width_bits);
    aom_wb_write_literal(wb, max_frame_height - 1, scs_ptr->seq_header.frame_height_bits);
    //
    /* Placeholder for actually writing to the bitstream */

    if (!scs_ptr->seq_header.reduced_still_picture_header) {
        //scs_ptr->frame_id_numbers_present_flag = 0;
        //    cm->large_scale_tile ? 0 : cm->error_resilient_mode;

        aom_wb_write_bit(wb, scs_ptr->seq_header.frame_id_numbers_present_flag);
        if (scs_ptr->seq_header.frame_id_numbers_present_flag) {
            // We must always have delta_frame_id_length < frame_id_length,
            // in order for a frame to be referenced with a unique delta.
            // Avoid wasting bits by using a coding that enforces this restriction.
            aom_wb_write_literal(wb, scs_ptr->seq_header.delta_frame_id_length - 2, 4);
            aom_wb_write_literal(
                wb, ((scs_ptr->seq_header.frame_id_length) - (scs_ptr->seq_header.delta_frame_id_length) - 1),
                3);
        }
    }

    aom_wb_write_bit(wb, scs_ptr->seq_header.sb_size == BLOCK_128X128 ? 1 : 0);
    //    write_sb_size(seq_params, wb);
    aom_wb_write_bit(wb, scs_ptr->seq_header.enable_filter_intra);

        scs_ptr->seq_header.enable_intra_edge_filter = 1;
    aom_wb_write_bit(wb, scs_ptr->seq_header.enable_intra_edge_filter);

    if (!scs_ptr->seq_header.reduced_still_picture_header) {
        aom_wb_write_bit(wb, scs_ptr->seq_header.enable_interintra_compound);
        aom_wb_write_bit(wb, scs_ptr->seq_header.enable_masked_compound);
        aom_wb_write_bit(wb, scs_ptr->static_config.enable_warped_motion);
        aom_wb_write_bit(wb, scs_ptr->seq_header.enable_dual_filter);

        aom_wb_write_bit(wb, scs_ptr->seq_header.order_hint_info.enable_order_hint);

        if (scs_ptr->seq_header.order_hint_info.enable_order_hint) {
            aom_wb_write_bit(wb, scs_ptr->seq_header.order_hint_info.enable_jnt_comp);
            aom_wb_write_bit(wb, scs_ptr->seq_header.order_hint_info.enable_ref_frame_mvs);
        }

        if (scs_ptr->seq_header.seq_force_screen_content_tools == 2)
            aom_wb_write_bit(wb, 1);
        else {
            aom_wb_write_bit(wb, 0);
            aom_wb_write_bit(wb, scs_ptr->seq_header.seq_force_screen_content_tools);
        }
        //
        if (scs_ptr->seq_header.seq_force_screen_content_tools > 0) {
            if (scs_ptr->seq_header.seq_force_integer_mv == 2)
                aom_wb_write_bit(wb, 1);
            else {
                aom_wb_write_bit(wb, 0);
                aom_wb_write_bit(wb, scs_ptr->seq_header.seq_force_integer_mv);
            }
        }
        else
            assert(scs_ptr->seq_header.seq_force_integer_mv == 2);
        if (scs_ptr->seq_header.order_hint_info.enable_order_hint)
            aom_wb_write_literal(wb, scs_ptr->seq_header.order_hint_info.order_hint_bits - 1, 3);
    }

    aom_wb_write_bit(wb, scs_ptr->seq_header.enable_superres);
    aom_wb_write_bit(wb, scs_ptr->seq_header.enable_cdef);
    aom_wb_write_bit(wb, scs_ptr->seq_header.enable_restoration);
}

// Recenters a non-negative literal v around a reference r
static uint16_t recenter_nonneg(uint16_t r, uint16_t v) {
    if (v > (r << 1))
        return v;
    else if (v >= r)
        return ((v - r) << 1);
    else
        return ((r - v) << 1) - 1;
}

// Recenters a non-negative literal v in [0, n-1] around a
// reference r also in [0, n-1]
static uint16_t recenter_finite_nonneg(uint16_t n, uint16_t r, uint16_t v) {
    if ((r << 1) <= n)
        return recenter_nonneg(r, v);
    else
        return recenter_nonneg(n - 1 - r, n - 1 - v);
}

int32_t aom_count_primitive_symmetric(int16_t v, uint32_t abs_bits) {
    return (v == 0 ? 1 : abs_bits + 2);
}

// Encodes a value v in [0, n-1] quasi-uniformly
void aom_write_primitive_quniform(AomWriter *w, uint16_t n, uint16_t v) {
    if (n <= 1) return;
    const int32_t l = get_msb(n - 1) + 1;
    const int32_t m = (1 << l) - n;
    if (v < m)
        aom_write_literal(w, v, l - 1);
    else {
        aom_write_literal(w, m + ((v - m) >> 1), l - 1);
        aom_write_bit(w, (v - m) & 1);
    }
}

static void aom_wb_write_primitive_quniform(struct AomWriteBitBuffer *wb,
    uint16_t n, uint16_t v) {
    if (n <= 1) return;
    const int32_t l = get_msb(n - 1) + 1;
    const int32_t m = (1 << l) - n;
    if (v < m)
        aom_wb_write_literal(wb, v, l - 1);
    else {
        aom_wb_write_literal(wb, m + ((v - m) >> 1), l - 1);
        aom_wb_write_bit(wb, (v - m) & 1);
    }
}

int32_t aom_count_primitive_quniform(uint16_t n, uint16_t v) {
    if (n <= 1) return 0;
    const int32_t l = get_msb(n - 1) + 1;
    const int32_t m = (1 << l) - n;
    return v < m ? l - 1 : l;
}

// Finite subexponential code that codes a symbol v in [0, n-1] with parameter k
void aom_write_primitive_subexpfin(AomWriter *w, uint16_t n, uint16_t k,
    uint16_t v) {
    int32_t i = 0;
    int32_t mk = 0;
    while (1) {
        int32_t b = (i ? k + i - 1 : k);
        int32_t a = (1 << b);
        if (n <= mk + 3 * a) {
            aom_write_primitive_quniform(w, (uint16_t)(n - mk), (uint16_t)(v - mk));
            break;
        }
        else {
            int32_t t = (v >= mk + a);
            aom_write_bit(w, t);
            if (t) {
                i = i + 1;
                mk += a;
            }
            else {
                aom_write_literal(w, v - mk, b);
                break;
            }
        }
    }
}

static void aom_wb_write_primitive_subexpfin(struct AomWriteBitBuffer *wb,
    uint16_t n, uint16_t k,
    uint16_t v) {
    int32_t i = 0;
    int32_t mk = 0;
    while (1) {
        int32_t b = (i ? k + i - 1 : k);
        int32_t a = (1 << b);
        if (n <= mk + 3 * a) {
            aom_wb_write_primitive_quniform(wb, (uint16_t)(n - mk), (uint16_t)(v - mk));
            break;
        }
        else {
            int32_t t = (v >= mk + a);
            aom_wb_write_bit(wb, t);
            if (t) {
                i = i + 1;
                mk += a;
            }
            else {
                aom_wb_write_literal(wb, v - mk, b);
                break;
            }
        }
    }
}

int32_t aom_count_primitive_subexpfin(uint16_t n, uint16_t k, uint16_t v) {
    int32_t count = 0;
    int32_t i = 0;
    int32_t mk = 0;
    while (1) {
        int32_t b = (i ? k + i - 1 : k);
        int32_t a = (1 << b);
        if (n <= mk + 3 * a) {
            count += aom_count_primitive_quniform((uint16_t)(n - mk), (uint16_t)(v - mk));
            break;
        }
        else {
            int32_t t = (v >= mk + a);
            count++;
            if (t) {
                i = i + 1;
                mk += a;
            }
            else {
                count += b;
                break;
            }
        }
    }
    return count;
}
// Finite subexponential code that codes a symbol v in[0, n - 1] with parameter k
// based on a reference ref also in [0, n-1].
// Recenters symbol around r first and then uses a finite subexponential code.
void aom_write_primitive_refsubexpfin(AomWriter *w, uint16_t n, uint16_t k,
    uint16_t ref, uint16_t v) {
    aom_write_primitive_subexpfin(w, n, k, recenter_finite_nonneg(n, ref, v));
}

static void aom_wb_write_primitive_refsubexpfin(struct AomWriteBitBuffer *wb,
    uint16_t n, uint16_t k,
    uint16_t ref, uint16_t v) {
    aom_wb_write_primitive_subexpfin(wb, n, k, recenter_finite_nonneg(n, ref, v));
}

void aom_wb_write_signed_primitive_refsubexpfin(struct AomWriteBitBuffer *wb,
    uint16_t n, uint16_t k,
    int16_t ref, int16_t v) {
    ref += n - 1;
    v += n - 1;
    const uint16_t scaled_n = (n << 1) - 1;
    aom_wb_write_primitive_refsubexpfin(wb, scaled_n, k, ref, v);
}

int32_t aom_count_primitive_refsubexpfin(uint16_t n, uint16_t k, uint16_t ref,
    uint16_t v) {
    return aom_count_primitive_subexpfin(n, k, recenter_finite_nonneg(n, ref, v));
}

static void write_global_motion_params(const EbWarpedMotionParams *params,
    const EbWarpedMotionParams *ref_params,
    struct AomWriteBitBuffer *wb,
    int32_t allow_hp) {
    const TransformationType type = params->wmtype;
    assert(type == TRANSLATION || type == IDENTITY);
    aom_wb_write_bit(wb, type != IDENTITY);
    if (type != IDENTITY) {
#if GLOBAL_TRANS_TYPES > 4
        aom_wb_write_literal(wb, type - 1, GLOBAL_TYPE_BITS);
#else
        aom_wb_write_bit(wb, type == ROTZOOM);
        if (type != ROTZOOM) aom_wb_write_bit(wb, type == TRANSLATION);
#endif  // GLOBAL_TRANS_TYPES > 4
    }

    if (type >= ROTZOOM) {
        int16_t ref2 = (int16_t)((ref_params->wmmat[2] >> GM_ALPHA_PREC_DIFF) - (1 << GM_ALPHA_PREC_BITS));
        int16_t v2 = (int16_t)((params->wmmat[2] >> GM_ALPHA_PREC_DIFF) - (1 << GM_ALPHA_PREC_BITS));

        int16_t ref3 = (int16_t)(ref_params->wmmat[3] >> GM_ALPHA_PREC_DIFF);
        int16_t v3 = (int16_t)(params->wmmat[3] >> GM_ALPHA_PREC_DIFF);

        aom_wb_write_signed_primitive_refsubexpfin(
            wb, GM_ALPHA_MAX + 1, SUBEXPFIN_K,
            ref2/*(ref_params->wmmat[2] >> GM_ALPHA_PREC_DIFF) - (1 << GM_ALPHA_PREC_BITS)*/,
            v2/*(int16_t)((params->wmmat[2] >> GM_ALPHA_PREC_DIFF) - (1 << GM_ALPHA_PREC_BITS))*/);
        aom_wb_write_signed_primitive_refsubexpfin(
            wb, GM_ALPHA_MAX + 1, SUBEXPFIN_K,
            ref3/*(ref_params->wmmat[3] >> GM_ALPHA_PREC_DIFF)*/,
            v3/*(int16_t)(params->wmmat[3] >> GM_ALPHA_PREC_DIFF)*/);
    }

    if (type >= AFFINE) {
        int16_t ref4 = (int16_t)(ref_params->wmmat[4] >> GM_ALPHA_PREC_DIFF);
        int16_t v4 = (int16_t)(params->wmmat[4] >> GM_ALPHA_PREC_DIFF);

        int16_t ref5 = (int16_t)((ref_params->wmmat[5] >> GM_ALPHA_PREC_DIFF) - (1 << GM_ALPHA_PREC_BITS));
        int16_t v5 = (int16_t)((params->wmmat[5] >> GM_ALPHA_PREC_DIFF) - (1 << GM_ALPHA_PREC_BITS));

        aom_wb_write_signed_primitive_refsubexpfin(
            wb, GM_ALPHA_MAX + 1, SUBEXPFIN_K,
            ref4/*(ref_params->wmmat[4] >> GM_ALPHA_PREC_DIFF)*/,
            v4/*(int16_t)(params->wmmat[4] >> GM_ALPHA_PREC_DIFF)*/);
        aom_wb_write_signed_primitive_refsubexpfin(
            wb, GM_ALPHA_MAX + 1, SUBEXPFIN_K,
            ref5/*(ref_params->wmmat[5] >> GM_ALPHA_PREC_DIFF) -    (1 << GM_ALPHA_PREC_BITS)*/,
            v5/*(int16_t)(params->wmmat[5] >> GM_ALPHA_PREC_DIFF) - (1 << GM_ALPHA_PREC_BITS)*/);
    }

    if (type >= TRANSLATION) {
        const int32_t trans_bits = (type == TRANSLATION)
            ? GM_ABS_TRANS_ONLY_BITS - !allow_hp
            : GM_ABS_TRANS_BITS;
        const int32_t trans_prec_diff = (type == TRANSLATION)
            ? GM_TRANS_ONLY_PREC_DIFF + !allow_hp
            : GM_TRANS_PREC_DIFF;
        aom_wb_write_signed_primitive_refsubexpfin(
            wb, (1 << trans_bits) + 1, SUBEXPFIN_K,
            (int16_t)(ref_params->wmmat[0] >> trans_prec_diff),
            (int16_t)(params->wmmat[0] >> trans_prec_diff));
        aom_wb_write_signed_primitive_refsubexpfin(
            wb, (1 << trans_bits) + 1, SUBEXPFIN_K,
            (int16_t)(ref_params->wmmat[1] >> trans_prec_diff),
            (int16_t)(params->wmmat[1] >> trans_prec_diff));
    }
}
static void WriteGlobalMotion(
    PictureParentControlSet *pcs_ptr,
    struct AomWriteBitBuffer *wb)

{
    int32_t frame;
    FrameHeader *frm_hdr = &pcs_ptr->frm_hdr;
    for (frame = LAST_FRAME; frame <= ALTREF_FRAME; ++frame) {
        const EbWarpedMotionParams *ref_params = &default_warp_params;
        //pcs_ptr->prev_frame ? &pcs_ptr->prev_frame->global_motion[frame] : &default_warp_params;

        write_global_motion_params(&pcs_ptr->global_motion[frame], ref_params, wb,
            frm_hdr->allow_high_precision_mv);
        // TODO(sarahparker, debargha): The logic in the commented out code below
        // does not work currently and causes mismatches when resize is on.
        // Fix it before turning the optimization back on.
        /*
        Yv12BufferConfig *ref_buf = get_ref_frame_buffer(cpi, frame);
        if (cpi->source->y_crop_width == ref_buf->y_crop_width &&
        cpi->source->y_crop_height == ref_buf->y_crop_height) {
        write_global_motion_params(&cm->global_motion[frame],
        &cm->prev_frame->global_motion[frame], wb,
        cm->allow_high_precision_mv);
        } else {
        assert(cm->global_motion[frame].wmtype == IDENTITY &&
        "Invalid warp type for frames of different resolutions");
        }
        */
        /*
        printf("Frame %d/%d: Enc Ref %d: %d %d %d %d\n",
        cm->current_video_frame, cm->show_frame, frame,
        cm->global_motion[frame].wmmat[0],
        cm->global_motion[frame].wmmat[1], cm->global_motion[frame].wmmat[2],
        cm->global_motion[frame].wmmat[3]);
        */
    }
}

static void write_film_grain_params(PictureParentControlSet *pcs_ptr,
    struct AomWriteBitBuffer *wb) {

    FrameHeader *frm_hdr = &pcs_ptr->frm_hdr;
    aom_film_grain_t *pars = &frm_hdr->film_grain_params;

    aom_wb_write_bit(wb, pars->apply_grain);
    if (!pars->apply_grain) return;

    aom_wb_write_literal(wb, pars->random_seed, 16);

    if (frm_hdr->frame_type == INTER_FRAME) {
        EbReferenceObject* refObj0 = (EbReferenceObject*)pcs_ptr->childPcs->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
        int32_t ref_idx = 0;
        pars->update_parameters = 1;
        if (film_grain_params_equal(&refObj0->film_grain_params, pars)) {
            pars->update_parameters = 0;
            ref_idx = get_ref_frame_map_idx(pcs_ptr, LAST_FRAME);
        }
        else if (pcs_ptr->childPcs->slice_type == B_SLICE)
        {
            EbReferenceObject* refObj1 = (EbReferenceObject*)pcs_ptr->childPcs->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
            if (film_grain_params_equal(&refObj1->film_grain_params, pars)) {
                pars->update_parameters = 0;
                ref_idx = get_ref_frame_map_idx(pcs_ptr, ALTREF_FRAME);  //todo: will it always be ALF_REF in L1?
            }
        }
        aom_wb_write_bit(wb, pars->update_parameters);
        if (!pars->update_parameters) {
            aom_wb_write_literal(wb, ref_idx, 3);
            return;
        }
    }
    else
        pars->update_parameters = 1;

    // Scaling functions parameters
    aom_wb_write_literal(wb, pars->num_y_points, 4);  // max 14
    for (int32_t i = 0; i < pars->num_y_points; i++) {
        aom_wb_write_literal(wb, pars->scaling_points_y[i][0], 8);
        aom_wb_write_literal(wb, pars->scaling_points_y[i][1], 8);
    }

    if (!pcs_ptr->sequence_control_set_ptr->seq_header.color_config.mono_chrome)
        aom_wb_write_bit(wb, pars->chroma_scaling_from_luma);
    else
        pars->chroma_scaling_from_luma = 0;  // for monochrome override to 0

    if (pcs_ptr->sequence_control_set_ptr->seq_header.color_config.mono_chrome || pars->chroma_scaling_from_luma ||
        // todo: add corresponding check when subsampling variables are present
        ((pcs_ptr->sequence_control_set_ptr->subsampling_x == 1) &&
        (pcs_ptr->sequence_control_set_ptr->subsampling_y == 1) &&
            (pars->num_y_points == 0))) {
        pars->num_cb_points = 0;
        pars->num_cr_points = 0;
    }
    else {
        aom_wb_write_literal(wb, pars->num_cb_points, 4);  // max 10
        for (int32_t i = 0; i < pars->num_cb_points; i++) {
            aom_wb_write_literal(wb, pars->scaling_points_cb[i][0], 8);
            aom_wb_write_literal(wb, pars->scaling_points_cb[i][1], 8);
        }

        aom_wb_write_literal(wb, pars->num_cr_points, 4);  // max 10
        for (int32_t i = 0; i < pars->num_cr_points; i++) {
            aom_wb_write_literal(wb, pars->scaling_points_cr[i][0], 8);
            aom_wb_write_literal(wb, pars->scaling_points_cr[i][1], 8);
        }
    }

    aom_wb_write_literal(wb, pars->scaling_shift - 8, 2);  // 8 + value

    // AR coefficients
    // Only sent if the corresponsing scaling function has
    // more than 0 points

    aom_wb_write_literal(wb, pars->ar_coeff_lag, 2);

    int32_t num_pos_luma = 2 * pars->ar_coeff_lag * (pars->ar_coeff_lag + 1);
    int32_t num_pos_chroma = num_pos_luma;
    if (pars->num_y_points > 0) ++num_pos_chroma;

    if (pars->num_y_points)
        for (int32_t i = 0; i < num_pos_luma; i++)
            aom_wb_write_literal(wb, pars->ar_coeffs_y[i] + 128, 8);

    if (pars->num_cb_points || pars->chroma_scaling_from_luma)
        for (int32_t i = 0; i < num_pos_chroma; i++)
            aom_wb_write_literal(wb, pars->ar_coeffs_cb[i] + 128, 8);

    if (pars->num_cr_points || pars->chroma_scaling_from_luma)
        for (int32_t i = 0; i < num_pos_chroma; i++)
            aom_wb_write_literal(wb, pars->ar_coeffs_cr[i] + 128, 8);

    aom_wb_write_literal(wb, pars->ar_coeff_shift - 6, 2);  // 8 + value

    aom_wb_write_literal(wb, pars->grain_scale_shift, 2);

    if (pars->num_cb_points) {
        aom_wb_write_literal(wb, pars->cb_mult, 8);
        aom_wb_write_literal(wb, pars->cb_luma_mult, 8);
        aom_wb_write_literal(wb, pars->cb_offset, 9);
    }

    if (pars->num_cr_points) {
        aom_wb_write_literal(wb, pars->cr_mult, 8);
        aom_wb_write_literal(wb, pars->cr_luma_mult, 8);
        aom_wb_write_literal(wb, pars->cr_offset, 9);
    }

    aom_wb_write_bit(wb, pars->overlap_flag);

    aom_wb_write_bit(wb, pars->clip_to_restricted_range);
}

// New function based on HLS R18
static void WriteUncompressedHeaderObu(SequenceControlSet *scs_ptr/*Av1Comp *cpi*/,
    PictureParentControlSet *pcs_ptr,
    //struct AomWriteBitBuffer *saved_wb,
    struct AomWriteBitBuffer *wb,
    uint8_t showExisting) {
    // Av1Common *const cm = &cpi->common;
    // MacroBlockD *const xd = &cpi->td.mb.e_mbd;

    // NOTE: By default all coded frames to be used as a reference
    pcs_ptr->is_reference_frame = 1;

    FrameHeader *frm_hdr = &pcs_ptr->frm_hdr;
    if (!scs_ptr->seq_header.reduced_still_picture_header) {
        if (showExisting) {
            //printf("ERROR[AN]: show_existing_frame not supported yet\n");
            //RefCntBuffer *const frame_bufs = cm->buffer_pool->frame_bufs;
            //const int32_t frame_to_show = cm->ref_frame_map[cpi->show_existing_frame];

            //if (frame_to_show < 0 || frame_bufs[frame_to_show].ref_count < 1) {
            //    aom_internal_error(&cm->error, AOM_CODEC_UNSUP_BITSTREAM,
            //        "Buffer %d does not contain a reconstructed frame",
            //        frame_to_show);
            //}
            //ref_cnt_fb(frame_bufs, &cm->new_fb_idx, frame_to_show);

            aom_wb_write_bit(wb, 1);  // show_existing_frame
            aom_wb_write_literal(wb, frm_hdr->show_existing_frame, 3);

            if (scs_ptr->seq_header.frame_id_numbers_present_flag) {
                printf("ERROR[AN]: frame_id_numbers_present_flag not supported yet\n");
                /*int32_t frame_id_len = cm->seq_params.frame_id_length;
                int32_t display_frame_id = cm->ref_frame_id[cpi->show_existing_frame];
                aom_wb_write_literal(wb, display_frame_id, frame_id_len);*/
            }

            //        if (cm->reset_decoder_state &&
            //            frame_bufs[frame_to_show].frame_type != KEY_FRAME) {
            //            aom_internal_error(
            //                &cm->error, AOM_CODEC_UNSUP_BITSTREAM,
            //                "show_existing_frame to reset state on KEY_FRAME only");
            //        }

            return;
        }
        else
            aom_wb_write_bit(wb, 0);  // show_existing_frame
        //frm_hdr->frame_type = pcs_ptr->intra_only ? INTRA_ONLY_FRAME : frm_hdr->frame_type;

        aom_wb_write_literal(wb, frm_hdr->frame_type, 2);

        // if (frm_hdr->intra_only) frm_hdr->frame_type = INTRA_ONLY_FRAME;

        aom_wb_write_bit(wb, frm_hdr->show_frame);

        if (!frm_hdr->show_frame)
            aom_wb_write_bit(wb, frm_hdr->showable_frame);
        if (frm_hdr->frame_type == S_FRAME)
            assert(frm_hdr->error_resilient_mode);
        else if (!(frm_hdr->frame_type == KEY_FRAME && frm_hdr->show_frame))
            aom_wb_write_bit(wb, frm_hdr->error_resilient_mode);
    }

    aom_wb_write_bit(wb, frm_hdr->disable_cdf_update);

    if (scs_ptr->seq_header.seq_force_screen_content_tools == 2)
        aom_wb_write_bit(wb, frm_hdr->allow_screen_content_tools);
    else {
        assert(frm_hdr->allow_screen_content_tools ==
            scs_ptr->seq_header.seq_force_screen_content_tools);
    }

    if (frm_hdr->allow_screen_content_tools) {
        if (scs_ptr->seq_header.seq_force_integer_mv == 2)
            aom_wb_write_bit(wb, frm_hdr->force_integer_mv);
        else
            assert(frm_hdr->force_integer_mv == scs_ptr->seq_header.seq_force_integer_mv);
    }
    else
        assert(frm_hdr->force_integer_mv == 0);

    if (!scs_ptr->seq_header.reduced_still_picture_header) {
        if (scs_ptr->seq_header.frame_id_numbers_present_flag) {
            int32_t frame_id_len = scs_ptr->seq_header.frame_id_length;
            aom_wb_write_literal(wb, frm_hdr->current_frame_id, frame_id_len);
        }

        //if (cm->width > cm->seq_params.max_frame_width ||
        //    cm->height > cm->seq_params.max_frame_height) {
        //    aom_internal_error(&cm->error, AOM_CODEC_UNSUP_BITSTREAM,
        //        "Frame dimensions are larger than the maximum values");
        //}

        int32_t frame_size_override_flag = 0;
        /*        (pcs_ptr->frame_type == S_FRAME) ? 1
        : (cm->width != cm->seq_params.max_frame_width ||
        cm->height != cm->seq_params.max_frame_height);*/
        if (frm_hdr->frame_type != S_FRAME) aom_wb_write_bit(wb, frame_size_override_flag);

        if (scs_ptr->seq_header.order_hint_info.enable_order_hint)
            aom_wb_write_literal(wb, (int32_t)pcs_ptr->frame_offset,
                scs_ptr->seq_header.order_hint_info.order_hint_bits);

        if (!frm_hdr->error_resilient_mode && !frame_is_intra_only(pcs_ptr))
            aom_wb_write_literal(wb, frm_hdr->primary_ref_frame, PRIMARY_REF_BITS);
    }
    int32_t frame_size_override_flag = 0;
    if (frm_hdr->frame_type == KEY_FRAME) {
        if (!frm_hdr->show_frame)
            aom_wb_write_literal(wb, pcs_ptr->av1_ref_signal.refresh_frame_mask, REF_FRAMES);
    }
    else {
        if (frm_hdr->frame_type == INTRA_ONLY_FRAME) {
            // pcs_ptr->refresh_frame_mask = get_refresh_mask(cpi);
            int32_t updated_fb = -1;
            for (int32_t i = 0; i < REF_FRAMES; i++) {
                // If more than one frame is refreshed, it doesn't matter which one
                // we pick, so pick the first.
                if (pcs_ptr->av1_ref_signal.refresh_frame_mask & (1 << i)) {
                    updated_fb = i;
                    break;
                }
            }
            assert(updated_fb >= 0);
            pcs_ptr->fb_of_context_type[pcs_ptr->frame_context_idx] = updated_fb;

            aom_wb_write_literal(wb, pcs_ptr->av1_ref_signal.refresh_frame_mask, REF_FRAMES);
        }
        else if (frm_hdr->frame_type == INTER_FRAME || frame_is_sframe(pcs_ptr)) {
            //pcs_ptr->refresh_frame_mask = get_refresh_mask(cpi);
            if (frm_hdr->frame_type == INTER_FRAME)
                aom_wb_write_literal(wb, pcs_ptr->av1_ref_signal.refresh_frame_mask, REF_FRAMES);
            else
                assert(frame_is_sframe(pcs_ptr) && pcs_ptr->av1_ref_signal.refresh_frame_mask == 0xFF);
            int32_t updated_fb = -1;
            for (int32_t i = 0; i < REF_FRAMES; i++) {
                // If more than one frame is refreshed, it doesn't matter which one
                // we pick, so pick the first.
                if (pcs_ptr->av1_ref_signal.refresh_frame_mask & (1 << i)) {
                    updated_fb = i;
                    break;
                }
            }
            // large scale tile sometimes won't refresh any fbs
            if (updated_fb >= 0)
                pcs_ptr->fb_of_context_type[pcs_ptr->frame_context_idx] = updated_fb;
            if (!pcs_ptr->av1_ref_signal.refresh_frame_mask) {
                // NOTE: "cpi->refresh_frame_mask == 0" indicates that the coded frame
                //       will not be used as a reference
                pcs_ptr->is_reference_frame = 0;
            }
        }
    }

    if (frm_hdr->frame_type == KEY_FRAME) {
        write_frame_size(pcs_ptr, frame_size_override_flag, wb);
        //assert(av1_superres_unscaled(cm) ||
        //    !(cm->allow_intrabc && NO_FILTER_FOR_IBC));
        if (frm_hdr->allow_screen_content_tools)
            aom_wb_write_bit(wb, frm_hdr->allow_intrabc);
        // all eight fbs are refreshed, pick one that will live long enough
        pcs_ptr->fb_of_context_type[REGULAR_FRAME] = 0;
    }
    else {
        if (frm_hdr->frame_type == INTRA_ONLY_FRAME) {
            write_frame_size(pcs_ptr, frame_size_override_flag, wb);
            if (frm_hdr->allow_screen_content_tools)
                aom_wb_write_bit(wb, frm_hdr->allow_intrabc);
        }
        else if (frm_hdr->frame_type == INTER_FRAME || frame_is_sframe(pcs_ptr)) {
            MvReferenceFrame ref_frame;

            assert(frm_hdr->frame_refs_short_signaling == 0);
            // NOTE: Error resilient mode turns off frame_refs_short_signaling
            //       automatically.
            if (scs_ptr->seq_header.order_hint_info.enable_order_hint)
                aom_wb_write_bit(wb, frm_hdr->frame_refs_short_signaling);

            if (frm_hdr->frame_refs_short_signaling) {
                aom_wb_write_literal(wb, get_ref_frame_map_idx(pcs_ptr, LAST_FRAME),
                    REF_FRAMES_LOG2);
                aom_wb_write_literal(wb, get_ref_frame_map_idx(pcs_ptr, GOLDEN_FRAME),
                    REF_FRAMES_LOG2);
            }
            for (ref_frame = LAST_FRAME; ref_frame <= ALTREF_FRAME; ++ref_frame) {
                assert(get_ref_frame_map_idx(pcs_ptr, ref_frame) != INVALID_IDX);
                if (!frm_hdr->frame_refs_short_signaling)
                    aom_wb_write_literal(wb, get_ref_frame_map_idx(pcs_ptr, ref_frame),
                        REF_FRAMES_LOG2);

                if (scs_ptr->seq_header.frame_id_numbers_present_flag) {
                    printf("ERROR[AN]: frame_id_numbers_present_flag not supported yet\n");
                    //int32_t i = get_ref_frame_map_idx(cpi, ref_frame);
                    //int32_t frame_id_len = cm->seq_params.frame_id_length;
                    //int32_t diff_len = cm->seq_params.delta_frame_id_length;
                    //int32_t delta_frame_id_minus1 =
                    //    ((cm->current_frame_id - cm->ref_frame_id[i] +
                    //    (1 << frame_id_len)) %
                    //    (1 << frame_id_len)) -
                    //    1;
                    //if (delta_frame_id_minus1 < 0 ||
                    //    delta_frame_id_minus1 >= (1 << diff_len))
                    //    cm->invalid_delta_frame_id_minus1 = 1;
                    //aom_wb_write_literal(wb, delta_frame_id_minus1, diff_len);
                }
            }

            if (!frm_hdr->error_resilient_mode && frame_size_override_flag) {
                printf("ERROR[AN]: frame_size_override_flag not supported yet\n");
                //write_frame_size_with_refs(pcs_ptr, wb);
            }
            else
                write_frame_size(pcs_ptr, frame_size_override_flag, wb);
            if (frm_hdr->force_integer_mv)
                frm_hdr->allow_high_precision_mv = 0;
            else
                aom_wb_write_bit(wb, frm_hdr->allow_high_precision_mv);
#define LOG_SWITCHABLE_FILTERS  2

            // OPT fix_interp_filter(cm, cpi->td.counts);
            // write_frame_interp_filter(cm->interp_filter, wb);
            aom_wb_write_bit(wb, pcs_ptr->av1_cm->interp_filter == SWITCHABLE);
            if (pcs_ptr->av1_cm->interp_filter != SWITCHABLE)
                aom_wb_write_literal(wb, pcs_ptr->av1_cm->interp_filter, LOG_SWITCHABLE_FILTERS);

            aom_wb_write_bit(wb, frm_hdr->is_motion_mode_switchable);
            if (frame_might_allow_ref_frame_mvs(pcs_ptr, scs_ptr))
                aom_wb_write_bit(wb, frm_hdr->use_ref_frame_mvs);
        }
    }

    //if (scs_ptr->frame_id_numbers_present_flag)
    //    pcs_ptr->refresh_mask = get_refresh_mask(pcs_ptr);
    const int32_t might_bwd_adapt =
        !(scs_ptr->seq_header.reduced_still_picture_header) && !(frm_hdr->disable_cdf_update);
    if (pcs_ptr->large_scale_tile)
        pcs_ptr->refresh_frame_context = REFRESH_FRAME_CONTEXT_DISABLED;
    if (might_bwd_adapt) {
        aom_wb_write_bit(
            wb, pcs_ptr->refresh_frame_context == REFRESH_FRAME_CONTEXT_DISABLED);
    }

    write_tile_info(pcs_ptr, /*saved_wb,*/ wb);

    encode_quantization(pcs_ptr, wb);
    encode_segmentation(pcs_ptr, wb);
    //aom_wb_write_bit(wb, 0);
    //encode_segmentation(cm, xd, wb);
        //if (pcs_ptr->delta_q_present_flag)
           // assert(delta_q_allowed == 1 && frm_hdr->quantisation_params.base_q_idx > 0);

    if (frm_hdr->quantization_params.base_q_idx > 0) {
        aom_wb_write_bit(wb, frm_hdr->delta_q_params.delta_q_present);
        if (frm_hdr->delta_q_params.delta_q_present) {
#if ADD_DELTA_QP_SUPPORT //PART 0
            aom_wb_write_literal(wb, OD_ILOG_NZ(frm_hdr->delta_q_params.delta_q_res) - 1, 2);
            pcs_ptr->prev_qindex = frm_hdr->quantization_params.base_q_idx;
            if (pcs_ptr->allow_intrabc)
                assert(frm_hdr->delta_lf_params.delta_lf_present == 0);
            else
                aom_wb_write_bit(wb, frm_hdr->delta_lf_params.delta_lf_present);
            if (frm_hdr->delta_lf_params.delta_lf_present) {
                aom_wb_write_literal(wb, OD_ILOG_NZ(frm_hdr->delta_lf_params.delta_lf_res) - 1, 2);
                pcs_ptr->prev_delta_lf_from_base = 0;
                aom_wb_write_bit(wb, frm_hdr->delta_lf_params.delta_lf_multi);
                const int32_t frame_lf_count =
                    pcs_ptr->monochrome == 0 ? FRAME_LF_COUNT : FRAME_LF_COUNT - 2;
                for (int32_t lf_id = 0; lf_id < frame_lf_count; ++lf_id)
                    pcs_ptr->prev_delta_lf[lf_id] = 0;
            }
#else
            printf("ERROR[AN]: delta_q_present_flag not supported yet\n");
#endif
            //                aom_wb_write_literal(wb, OD_ILOG_NZ(frm_hdr->delta_q_params.delta_q_res) - 1, 2);
            //                xd->prev_qindex = frm_hdr->quantization_params.base_q_idx;
            //                if (cm->allow_intrabc)
            //                    assert(frm_hdr->delta_lf_params.delta_lf_present == 0);
            //                else
            //                    aom_wb_write_bit(wb, frm_hdr->delta_lf_params.delta_lf_present);
            //                if (frm_hdr->delta_lf_params.delta_lf_present) {
            //                    aom_wb_write_literal(wb, OD_ILOG_NZ(frm_hdr->delta_lf_params.delta_lf_res) - 1, 2);
            //                    xd->prev_delta_lf_from_base = 0;
            //                    aom_wb_write_bit(wb, frm_hdr->delta_lf_params.delta_lf_multi);
            //                    const int32_t frame_lf_count =
            //                        av1_num_planes(cm) > 1 ? FRAME_LF_COUNT : FRAME_LF_COUNT - 2;
            //                    for (int32_t lf_id = 0; lf_id < frame_lf_count; ++lf_id)
            //                        xd->prev_delta_lf[lf_id] = 0;
            //                }
        }
    }
    if (frm_hdr->all_lossless) {
        printf("ERROR[AN]: all_lossless\n");
        //assert(av1_superres_unscaled(pcs_ptr));
    }
    else {
        if (!frm_hdr->coded_lossless) {
            encode_loopfilter(pcs_ptr, wb);
            if (scs_ptr->seq_header.enable_cdef)
                encode_cdef(pcs_ptr, wb);
        }

        if (scs_ptr->seq_header.enable_restoration)
            encode_restoration_mode(pcs_ptr, wb);
    }

    aom_wb_write_bit(wb, frm_hdr->tx_mode == TX_MODE_SELECT);
    //write_tx_mode(cm, &pcs_ptr->tx_mode, wb);

    if (pcs_ptr->allow_comp_inter_inter) {
        const int32_t use_hybrid_pred = frm_hdr->reference_mode == REFERENCE_MODE_SELECT;

        aom_wb_write_bit(wb, use_hybrid_pred);
    }

    if (pcs_ptr->is_skip_mode_allowed) aom_wb_write_bit(wb, pcs_ptr->skip_mode_flag);

    if (frame_might_allow_warped_motion(pcs_ptr, scs_ptr))
        aom_wb_write_bit(wb, frm_hdr->allow_warped_motion);
    else
        assert(!frm_hdr->allow_warped_motion);

    aom_wb_write_bit(wb, frm_hdr->reduced_tx_set);

    if (!frame_is_intra_only(pcs_ptr)) {
        //  printf("ERROR[AN]: Global motion not supported yet\n");
        WriteGlobalMotion(pcs_ptr, wb);
    }
    if (scs_ptr->seq_header.film_grain_params_present && (frm_hdr->show_frame || frm_hdr->showable_frame))
        write_film_grain_params(pcs_ptr, wb);
}

uint32_t WriteObuHeader(obuType obuType, int32_t obuExtension,
    uint8_t *const dst) {
    struct AomWriteBitBuffer wb = { dst, 0 };
    uint32_t size = 0;

    aom_wb_write_literal(&wb, 0, 1);  // forbidden bit.
    aom_wb_write_literal(&wb, (int32_t)obuType, 4);
    aom_wb_write_literal(&wb, obuExtension ? 1 : 0, 1);
    aom_wb_write_literal(&wb, 1, 1);  // obu_has_payload_length_field
    aom_wb_write_literal(&wb, 0, 1);  // reserved

    if (obuExtension)
        aom_wb_write_literal(&wb, obuExtension & 0xFF, 8);
    size = aom_wb_bytes_written(&wb);
    return size;
}
int32_t WriteUlebObuSize(uint32_t obuHeaderSize, uint32_t obuPayloadSize,
    uint8_t *dest) {
    const uint32_t obuSize = obuPayloadSize;
    const uint32_t offset = obuHeaderSize;
    size_t codedObuSize = 0;

    if (aom_uleb_encode(obuSize, sizeof(obuSize), dest + offset,
        &codedObuSize) != 0) {
        return AOM_CODEC_ERROR;
    }

    return AOM_CODEC_OK;
}
static size_t ObuMemMove(
    uint32_t obuHeaderSize,
    uint32_t obuPayloadSize,
    uint8_t *data)
{
    const size_t lengthFieldSize = aom_uleb_size_in_bytes(obuPayloadSize);
    const uint32_t moveDstOffset =
        (uint32_t)lengthFieldSize + obuHeaderSize;
    const uint32_t moveSrcOffset = obuHeaderSize;
    const uint32_t moveSize = obuPayloadSize;
    memmove(data + moveDstOffset, data + moveSrcOffset, moveSize);
    return lengthFieldSize;
}

static void add_trailing_bits(struct AomWriteBitBuffer *wb) {
    if (aom_wb_is_byte_aligned(wb))
        aom_wb_write_literal(wb, 0x80, 8);
    else {
        // assumes that the other bits are already 0s
        aom_wb_write_bit(wb, 1);
    }
}
static void write_bitstream_level(BitstreamLevel bl,
    struct AomWriteBitBuffer *wb) {
    uint8_t seq_level_idx = major_minor_to_seq_level_idx(bl);
    assert(is_valid_seq_level_idx(seq_level_idx));
    aom_wb_write_literal(wb, seq_level_idx, LEVEL_BITS);
}
static uint32_t WriteSequenceHeaderObu(
    SequenceControlSet *scs_ptr,
    uint8_t *const dst,
    uint8_t numberSpatialLayers)
{
    struct AomWriteBitBuffer wb = { dst, 0 };
    uint32_t size = 0;

    SetBitstreamLevelTier(scs_ptr);

    WriteProfile((BitstreamProfile)scs_ptr->static_config.profile, &wb);

    // Still picture or not
    aom_wb_write_bit(&wb, scs_ptr->seq_header.still_picture);
    assert(IMPLIES(!scs_ptr->seq_header.still_picture,
        !scs_ptr->seq_header.reduced_still_picture_header));

    // whether to use reduced still picture header
    aom_wb_write_bit(&wb, scs_ptr->seq_header.reduced_still_picture_header);

    if (scs_ptr->seq_header.reduced_still_picture_header) {
        printf("ERROR[AN]: reduced_still_picture_hdr not supported\n");
        //write_bitstream_level(cm->seq_params.level[0], &wb);
    }
    else {
        aom_wb_write_bit(&wb, scs_ptr->seq_header.timing_info.timing_info_present);  // timing info present flag

        if (scs_ptr->seq_header.timing_info.timing_info_present) {
            // timing_info
            printf("ERROR[AN]: timing_info_present not supported\n");
            /*write_timing_info_header(cm, &wb);
            aom_wb_write_bit(&wb, cm->decoder_model_info_present_flag);
            if (cm->decoder_model_info_present_flag) write_decoder_model_info(cm, &wb);*/
        }
        aom_wb_write_bit(&wb, scs_ptr->seq_header.initial_display_delay_present_flag);

        uint8_t operating_points_cnt_minus_1 =
            numberSpatialLayers > 1 ? numberSpatialLayers - 1 : 0;
        aom_wb_write_literal(&wb, operating_points_cnt_minus_1,
            OP_POINTS_CNT_MINUS_1_BITS);
        int32_t i;
        for (i = 0; i < operating_points_cnt_minus_1 + 1; i++) {
            aom_wb_write_literal(&wb, scs_ptr->seq_header.operating_point[i].op_idc,
                OP_POINTS_IDC_BITS);
            write_bitstream_level(scs_ptr->level[i], &wb);
            if (scs_ptr->level[i].major > 3)
                aom_wb_write_bit(&wb, scs_ptr->seq_header.operating_point[i].seq_tier);
            if (scs_ptr->seq_header.decoder_model_info_present_flag) {
                printf("ERROR[AN]: decoder_model_info_present_flag not supported\n");
                //aom_wb_write_bit(&wb,
                //    cm->op_params[i].decoder_model_param_present_flag);
                //if (cm->op_params[i].decoder_model_param_present_flag)
                //    write_dec_model_op_parameters(cm, &wb, i);
            }
            if (scs_ptr->seq_header.initial_display_delay_present_flag) {
                printf("ERROR[AN]: display_model_info_present_flag not supported\n");
                //aom_wb_write_bit(&wb,
                //    cm->op_params[i].display_model_param_present_flag);
                //if (cm->op_params[i].display_model_param_present_flag) {
                //    assert(cm->op_params[i].initial_display_delay <= 10);
                //    aom_wb_write_literal(&wb, cm->op_params[i].initial_display_delay - 1,
                //        4);
                //}
            }
        }
    }
    write_sequence_header(scs_ptr, &wb);

    write_color_config(scs_ptr, &wb);

    aom_wb_write_bit(&wb, scs_ptr->seq_header.film_grain_params_present);

    add_trailing_bits(&wb);

    size = aom_wb_bytes_written(&wb);
    return size;
}
static uint32_t write_tile_group_header(uint8_t *const dst, int startTile,
    int endTile, int tiles_log2,
    int tile_start_and_end_present_flag)
{
    struct AomWriteBitBuffer wb = { dst, 0 };
    uint32_t size = 0;

    if (!tiles_log2) return size;
    aom_wb_write_bit(&wb, tile_start_and_end_present_flag);

    if (tile_start_and_end_present_flag) {
        aom_wb_write_literal(&wb, startTile, tiles_log2);
        aom_wb_write_literal(&wb, endTile, tiles_log2);
    }

    size = aom_wb_bytes_written(&wb);
    return size;
}

static uint32_t WriteFrameHeaderObu(
    SequenceControlSet      *scs_ptr,
    PictureParentControlSet *pcs_ptr,
    uint8_t                   *const dst,
    uint8_t showExisting,
    int32_t appendTrailingBits
)
{
    struct AomWriteBitBuffer wb = { dst, 0 };
    uint32_t totalSize = 0;

    WriteUncompressedHeaderObu(scs_ptr, pcs_ptr,/* saved_wb,*/ &wb, showExisting);

    if (appendTrailingBits)
        add_trailing_bits(&wb);

    if (showExisting) {
        totalSize = aom_wb_bytes_written(&wb);
        return totalSize;
    }

    totalSize = aom_wb_bytes_written(&wb);
    return totalSize;
}

/**************************************************
* EncodeFrameHeaderHeader
**************************************************/
EbErrorType write_frame_header_av1(
    Bitstream *bitstream_ptr,
    SequenceControlSet *scs_ptr,
    PictureControlSet *pcs_ptr,
    uint8_t showExisting)
{
    EbErrorType                 return_error = EB_ErrorNone;
    OutputBitstreamUnit       *output_bitstream_ptr = (OutputBitstreamUnit*)bitstream_ptr->output_bitstream_ptr;
    PictureParentControlSet   *parent_pcs_ptr = pcs_ptr->parent_pcs_ptr;
    uint8_t                     *data = output_bitstream_ptr->buffer_av1;
    uint32_t obuHeaderSize = 0;

    int32_t currDataSize = 0;

    const uint8_t obuExtensionHeader = 0;

    // A new tile group begins at this tile.  Write the obu header and
    // tile group header
    const obuType obuType = showExisting ? OBU_FRAME_HEADER : OBU_FRAME;
    currDataSize =
        WriteObuHeader(obuType, obuExtensionHeader, data);
    obuHeaderSize = currDataSize;

    currDataSize +=
        WriteFrameHeaderObu(scs_ptr, parent_pcs_ptr, /*saved_wb,*/ data + currDataSize, showExisting, showExisting);

    const int n_log2_tiles = parent_pcs_ptr->av1_cm->log2_tile_rows + parent_pcs_ptr->av1_cm->log2_tile_cols;
    int tile_start_and_end_present_flag = 0;

   currDataSize += write_tile_group_header(data + currDataSize,0,
        0, n_log2_tiles, tile_start_and_end_present_flag);

    if (!showExisting) {
        // Add data from EC stream to Picture Stream.
        int32_t frameSize = (int32_t)(parent_pcs_ptr->av1_cm->tiles_info.tile_cols*parent_pcs_ptr->av1_cm->tiles_info.tile_rows==1 ? pcs_ptr->entropy_coder_ptr->ec_writer.pos : pcs_ptr->entropy_coder_ptr->ec_frame_size);
        OutputBitstreamUnit *ec_output_bitstream_ptr = (OutputBitstreamUnit*)pcs_ptr->entropy_coder_ptr->ec_output_bitstream_ptr;
        //****************************************************************//
        // Copy from EC stream to frame stream
        memcpy(data + currDataSize, ec_output_bitstream_ptr->buffer_begin_av1, frameSize);
        currDataSize += (frameSize);
    }
    const uint32_t obuPayloadSize = currDataSize - obuHeaderSize;
    const size_t lengthFieldSize =
        ObuMemMove(obuHeaderSize, obuPayloadSize, data);
    if (WriteUlebObuSize(obuHeaderSize, obuPayloadSize, data) !=
        AOM_CODEC_OK) {
        assert(0);
    }
    currDataSize += (int32_t)lengthFieldSize;
    data += currDataSize;

    output_bitstream_ptr->buffer_av1 = data;
    return return_error;
}

/**************************************************
* encode_sps_av1
**************************************************/
EbErrorType encode_sps_av1(
    Bitstream *bitstream_ptr,
    SequenceControlSet *scs_ptr)
{
    EbErrorType            return_error = EB_ErrorNone;
    OutputBitstreamUnit  *output_bitstream_ptr = (OutputBitstreamUnit*)bitstream_ptr->output_bitstream_ptr;
    uint8_t                *data = output_bitstream_ptr->buffer_av1;
    uint32_t                obuHeaderSize = 0;
    uint32_t                obuPayloadSize = 0;
    const uint8_t enhancementLayersCnt = 0;// cm->enhancementLayersCnt;

    // write sequence header obu if KEY_FRAME, preceded by 4-byte size
    obuHeaderSize = WriteObuHeader(OBU_SEQUENCE_HEADER, 0, data);

    obuPayloadSize = WriteSequenceHeaderObu(scs_ptr,/*cpi,*/ data + obuHeaderSize,
        enhancementLayersCnt);

    const size_t lengthFieldSize = ObuMemMove(obuHeaderSize, obuPayloadSize, data);
    if (WriteUlebObuSize(obuHeaderSize, obuPayloadSize, data) !=
        AOM_CODEC_OK) {
        // return AOM_CODEC_ERROR;
    }

    data += obuHeaderSize + obuPayloadSize + lengthFieldSize;
    output_bitstream_ptr->buffer_av1 = data;
    return return_error;
}
/**************************************************
* encode_td_av1
**************************************************/
EbErrorType encode_td_av1(
    uint8_t *output_bitstream_ptr)
{
    EbErrorType              return_error = EB_ErrorNone;
    //OutputBitstreamUnit   *output_bitstream_ptr = (OutputBitstreamUnit*)bitstream_ptr->output_bitstream_ptr;
    assert(output_bitstream_ptr != NULL);

    uint8_t                 *data = output_bitstream_ptr;

    // move data and insert OBU_TD preceded by optional 4 byte size
    uint32_t obu_header_size = 1;
    const uint32_t obu_payload_size = 0;
    const size_t length_field_size = aom_uleb_size_in_bytes(obu_payload_size);

    obu_header_size = WriteObuHeader(
        OBU_TEMPORAL_DELIMITER, 0, data);

    // OBUs are preceded/succeeded by an unsigned leb128 coded integer.
    if (WriteUlebObuSize(obu_header_size, obu_payload_size, data) != AOM_CODEC_OK)
        //return AOM_CODEC_ERROR;
    data += obu_header_size + obu_payload_size + length_field_size;
    output_bitstream_ptr = data;

    return return_error;
}

#if ADD_DELTA_QP_SUPPORT
static void Av1writeDeltaQindex(
    FRAME_CONTEXT *frameContext,
    int32_t           delta_qindex,
    AomWriter    *w)
{
    int32_t sign = delta_qindex < 0;
    int32_t abs = sign ? -delta_qindex : delta_qindex;
    int32_t rem_bits, thr;
    int32_t smallval = abs < DELTA_Q_SMALL ? 1 : 0;
    //FRAME_CONTEXT *ec_ctx = xd->tile_ctx;

    aom_write_symbol(w, AOMMIN(abs, DELTA_Q_SMALL), frameContext->delta_q_cdf,
        DELTA_Q_PROBS + 1);

    if (!smallval) {
        rem_bits = OD_ILOG_NZ(abs - 1) - 1;
        thr = (1 << rem_bits) + 1;
        aom_write_literal(w, rem_bits - 1, 3);
        aom_write_literal(w, abs - thr, rem_bits);
    }
    if (abs > 0)
        aom_write_bit(w, sign);
}
#endif

static void write_cdef(
    SequenceControlSet     *seqCSetPtr,
    PictureControlSet     *p_pcs_ptr,
    //Av1Common *cm,
    MacroBlockD *const xd,
    AomWriter *w,
    int32_t skip, int32_t mi_col, int32_t mi_row)
{
    (void)xd;
    Av1Common *cm = p_pcs_ptr->parent_pcs_ptr->av1_cm;
    FrameHeader *frm_hdr = &p_pcs_ptr->parent_pcs_ptr->frm_hdr;

    if (frm_hdr->coded_lossless || frm_hdr->allow_intrabc) {
        // Initialize to indicate no CDEF for safety.
        frm_hdr->CDEF_params.cdef_bits = 0;
        frm_hdr->CDEF_params.cdef_y_strength[0] = 0;
        p_pcs_ptr->parent_pcs_ptr->nb_cdef_strengths = 1;
        frm_hdr->CDEF_params.cdef_uv_strength[0] = 0;
        return;
    }

    const int32_t m = ~((1 << (6 - MI_SIZE_LOG2)) - 1);
    const ModeInfo *mi =
        p_pcs_ptr->mi_grid_base[(mi_row & m) * cm->mi_stride + (mi_col & m)];
    //cm->mi_grid_visible[(mi_row & m) * cm->mi_stride + (mi_col & m)];

// Initialise when at top left part of the superblock
    if (!(mi_row & (seqCSetPtr->seq_header.sb_mi_size - 1)) &&
        !(mi_col & (seqCSetPtr->seq_header.sb_mi_size - 1))) {  // Top left?
        p_pcs_ptr->cdef_preset[0] = p_pcs_ptr->cdef_preset[1] = p_pcs_ptr->cdef_preset[2] =
            p_pcs_ptr->cdef_preset[3] = -1;
    }

    // Emit CDEF param at first non-skip coding block
    const int32_t mask = 1 << (6 - MI_SIZE_LOG2);
    const int32_t index = seqCSetPtr->seq_header.sb_size == BLOCK_128X128
        ? !!(mi_col & mask) + 2 * !!(mi_row & mask)
        : 0;

    if (p_pcs_ptr->cdef_preset[index] == -1 && !skip) {
        aom_write_literal(w, mi->mbmi.cdef_strength, frm_hdr->CDEF_params.cdef_bits);
        p_pcs_ptr->cdef_preset[index] = mi->mbmi.cdef_strength;
    }
}

void av1_reset_loop_restoration(PictureControlSet     *piCSetPtr) {
    for (int32_t p = 0; p < 3; ++p) {
        set_default_wiener(piCSetPtr->wiener_info + p);
        set_default_sgrproj(piCSetPtr->sgrproj_info + p);
    }
}
static void write_wiener_filter(int32_t wiener_win, const WienerInfo *wiener_info,
    WienerInfo *ref_wiener_info, AomWriter *wb) {
    if (wiener_win == WIENER_WIN)
        aom_write_primitive_refsubexpfin(
            wb, WIENER_FILT_TAP0_MAXV - WIENER_FILT_TAP0_MINV + 1,
            WIENER_FILT_TAP0_SUBEXP_K,
            ref_wiener_info->vfilter[0] - WIENER_FILT_TAP0_MINV,
            wiener_info->vfilter[0] - WIENER_FILT_TAP0_MINV);
    else
        assert(wiener_info->vfilter[0] == 0 &&
            wiener_info->vfilter[WIENER_WIN - 1] == 0);
    aom_write_primitive_refsubexpfin(
        wb, WIENER_FILT_TAP1_MAXV - WIENER_FILT_TAP1_MINV + 1,
        WIENER_FILT_TAP1_SUBEXP_K,
        ref_wiener_info->vfilter[1] - WIENER_FILT_TAP1_MINV,
        wiener_info->vfilter[1] - WIENER_FILT_TAP1_MINV);
    aom_write_primitive_refsubexpfin(
        wb, WIENER_FILT_TAP2_MAXV - WIENER_FILT_TAP2_MINV + 1,
        WIENER_FILT_TAP2_SUBEXP_K,
        ref_wiener_info->vfilter[2] - WIENER_FILT_TAP2_MINV,
        wiener_info->vfilter[2] - WIENER_FILT_TAP2_MINV);
    if (wiener_win == WIENER_WIN)
        aom_write_primitive_refsubexpfin(
            wb, WIENER_FILT_TAP0_MAXV - WIENER_FILT_TAP0_MINV + 1,
            WIENER_FILT_TAP0_SUBEXP_K,
            ref_wiener_info->hfilter[0] - WIENER_FILT_TAP0_MINV,
            wiener_info->hfilter[0] - WIENER_FILT_TAP0_MINV);
    else
        assert(wiener_info->hfilter[0] == 0 &&
            wiener_info->hfilter[WIENER_WIN - 1] == 0);
    aom_write_primitive_refsubexpfin(
        wb, WIENER_FILT_TAP1_MAXV - WIENER_FILT_TAP1_MINV + 1,
        WIENER_FILT_TAP1_SUBEXP_K,
        ref_wiener_info->hfilter[1] - WIENER_FILT_TAP1_MINV,
        wiener_info->hfilter[1] - WIENER_FILT_TAP1_MINV);
    aom_write_primitive_refsubexpfin(
        wb, WIENER_FILT_TAP2_MAXV - WIENER_FILT_TAP2_MINV + 1,
        WIENER_FILT_TAP2_SUBEXP_K,
        ref_wiener_info->hfilter[2] - WIENER_FILT_TAP2_MINV,
        wiener_info->hfilter[2] - WIENER_FILT_TAP2_MINV);
    memcpy(ref_wiener_info, wiener_info, sizeof(*wiener_info));
}

static void write_sgrproj_filter(const SgrprojInfo *sgrproj_info,
    SgrprojInfo *ref_sgrproj_info,
    AomWriter *wb) {
    aom_write_literal(wb, sgrproj_info->ep, SGRPROJ_PARAMS_BITS);
    const SgrParamsType *params = &sgr_params[sgrproj_info->ep];

    if (params->r[0] == 0) {
        assert(sgrproj_info->xqd[0] == 0);
        aom_write_primitive_refsubexpfin(
            wb, SGRPROJ_PRJ_MAX1 - SGRPROJ_PRJ_MIN1 + 1, SGRPROJ_PRJ_SUBEXP_K,
            (uint16_t)(ref_sgrproj_info->xqd[1] - SGRPROJ_PRJ_MIN1),
            (uint16_t)(sgrproj_info->xqd[1] - SGRPROJ_PRJ_MIN1));
    }
    else if (params->r[1] == 0) {
        aom_write_primitive_refsubexpfin(
            wb, SGRPROJ_PRJ_MAX0 - SGRPROJ_PRJ_MIN0 + 1, SGRPROJ_PRJ_SUBEXP_K,
            (uint16_t)(ref_sgrproj_info->xqd[0] - SGRPROJ_PRJ_MIN0),
            (uint16_t)(sgrproj_info->xqd[0] - SGRPROJ_PRJ_MIN0));
    }
    else {
        aom_write_primitive_refsubexpfin(
            wb, SGRPROJ_PRJ_MAX0 - SGRPROJ_PRJ_MIN0 + 1, SGRPROJ_PRJ_SUBEXP_K,
            (uint16_t)(ref_sgrproj_info->xqd[0] - SGRPROJ_PRJ_MIN0),
            (uint16_t)(sgrproj_info->xqd[0] - SGRPROJ_PRJ_MIN0));
        aom_write_primitive_refsubexpfin(
            wb, SGRPROJ_PRJ_MAX1 - SGRPROJ_PRJ_MIN1 + 1, SGRPROJ_PRJ_SUBEXP_K,
            (uint16_t)(ref_sgrproj_info->xqd[1] - SGRPROJ_PRJ_MIN1),
            (uint16_t)(sgrproj_info->xqd[1] - SGRPROJ_PRJ_MIN1));
    }

    memcpy(ref_sgrproj_info, sgrproj_info, sizeof(*sgrproj_info));
}
static void loop_restoration_write_sb_coeffs(PictureControlSet     *piCSetPtr, FRAME_CONTEXT           *frameContext, const Av1Common *const cm,
    //MacroBlockD *xd,
    const RestorationUnitInfo *rui,
    AomWriter *const w, int32_t plane/*,
    FRAME_COUNTS *counts*/)
{
    const RestorationInfo *rsi = cm->rst_info + plane;
    RestorationType frame_rtype = rsi->frame_restoration_type;
    if (frame_rtype == RESTORE_NONE) return;

    //(void)counts;
//    assert(!cm->all_lossless);

    const int32_t wiener_win = (plane > 0) ? WIENER_WIN_CHROMA : WIENER_WIN;
    WienerInfo *wiener_info = piCSetPtr->wiener_info + plane;
    SgrprojInfo *sgrproj_info = piCSetPtr->sgrproj_info + plane;
    RestorationType unit_rtype = rui->restoration_type;

    assert(unit_rtype < CDF_SIZE(RESTORE_SWITCHABLE_TYPES));

    if (frame_rtype == RESTORE_SWITCHABLE) {
        aom_write_symbol(w, unit_rtype, /*xd->tile_ctx->*/frameContext->switchable_restore_cdf,
            RESTORE_SWITCHABLE_TYPES);
#if CONFIG_ENTROPY_STATS
        ++counts->switchable_restore[unit_rtype];
#endif
        switch (unit_rtype) {
        case RESTORE_WIENER:
            write_wiener_filter(wiener_win, &rui->wiener_info, wiener_info, w);
            //printf("POC:%i plane:%i v:%i %i %i  h:%i %i %i\n", piCSetPtr->picture_number, plane, rui->wiener_info.vfilter[0], rui->wiener_info.vfilter[1], rui->wiener_info.vfilter[2], rui->wiener_info.hfilter[0], rui->wiener_info.hfilter[1], rui->wiener_info.hfilter[2]);
            break;
        case RESTORE_SGRPROJ:
            write_sgrproj_filter(&rui->sgrproj_info, sgrproj_info, w);
            //printf("POC:%i plane:%i ep:%i xqd_0:%i  xqd_1:%i\n", piCSetPtr->picture_number, plane, rui->sgrproj_info.ep, rui->sgrproj_info.xqd[0], rui->sgrproj_info.xqd[1]);
            break;
        default: assert(unit_rtype == RESTORE_NONE);// printf("POC:%i plane:%i OFF\n", piCSetPtr->picture_number, plane);
            break;
        }
    }
    else if (frame_rtype == RESTORE_WIENER) {
        aom_write_symbol(w, unit_rtype != RESTORE_NONE,
            /*xd->tile_ctx->*/frameContext->wiener_restore_cdf, 2);
#if CONFIG_ENTROPY_STATS
        ++counts->wiener_restore[unit_rtype != RESTORE_NONE];
#endif
        if (unit_rtype != RESTORE_NONE) {
            write_wiener_filter(wiener_win, &rui->wiener_info, wiener_info, w);
            //printf("POC:%i plane:%i v:%i %i %i  h:%i %i %i\n", piCSetPtr->picture_number, plane, rui->wiener_info.vfilter[0], rui->wiener_info.vfilter[1], rui->wiener_info.vfilter[2], rui->wiener_info.hfilter[0], rui->wiener_info.hfilter[1], rui->wiener_info.hfilter[2]);
        }
        //else
            //printf("POC:%i plane:%i OFF\n", piCSetPtr->picture_number, plane);
    }
    else if (frame_rtype == RESTORE_SGRPROJ) {
        aom_write_symbol(w, unit_rtype != RESTORE_NONE,
            /*xd->tile_ctx->*/frameContext->sgrproj_restore_cdf, 2);
#if CONFIG_ENTROPY_STATS
        ++counts->sgrproj_restore[unit_rtype != RESTORE_NONE];
#endif
        if (unit_rtype != RESTORE_NONE) {
            write_sgrproj_filter(&rui->sgrproj_info, sgrproj_info, w);
            //printf("POC:%i plane:%i ep:%i xqd_0:%i  xqd_1:%i\n", piCSetPtr->picture_number, plane, rui->sgrproj_info.ep, rui->sgrproj_info.xqd[0], rui->sgrproj_info.xqd[1]);
        }
        //else
        //    printf("POC:%i plane:%i OFF\n", piCSetPtr->picture_number, plane);
    }
}

EbErrorType ec_update_neighbors(
    PictureControlSet     *picture_control_set_ptr,
    EntropyCodingContext  *context_ptr,
    uint32_t                 blkOriginX,
    uint32_t                 blkOriginY,
    CodingUnit            *cu_ptr,
    BlockSize                bsize,
    EbPictureBufferDesc   *coeff_ptr)
{
    UNUSED(coeff_ptr);
    EbErrorType return_error = EB_ErrorNone;
    NeighborArrayUnit     *mode_type_neighbor_array = picture_control_set_ptr->mode_type_neighbor_array;
    NeighborArrayUnit     *partition_context_neighbor_array = picture_control_set_ptr->partition_context_neighbor_array;
    NeighborArrayUnit     *skip_flag_neighbor_array = picture_control_set_ptr->skip_flag_neighbor_array;
    NeighborArrayUnit     *skip_coeff_neighbor_array = picture_control_set_ptr->skip_coeff_neighbor_array;
    NeighborArrayUnit     *luma_dc_sign_level_coeff_neighbor_array = picture_control_set_ptr->luma_dc_sign_level_coeff_neighbor_array;
    NeighborArrayUnit     *cr_dc_sign_level_coeff_neighbor_array = picture_control_set_ptr->cr_dc_sign_level_coeff_neighbor_array;
    NeighborArrayUnit     *cb_dc_sign_level_coeff_neighbor_array = picture_control_set_ptr->cb_dc_sign_level_coeff_neighbor_array;
    NeighborArrayUnit     *inter_pred_dir_neighbor_array = picture_control_set_ptr->inter_pred_dir_neighbor_array;
    NeighborArrayUnit     *ref_frame_type_neighbor_array = picture_control_set_ptr->ref_frame_type_neighbor_array;
    NeighborArrayUnit32   *interpolation_type_neighbor_array = picture_control_set_ptr->interpolation_type_neighbor_array;
    const BlockGeom         *blk_geom = get_blk_geom_mds(cu_ptr->mds_idx);
    EbBool                   skipCoeff = EB_FALSE;
    PartitionContext         partition;

    skipCoeff = cu_ptr->block_has_coeff ? 0 : 1;
    // Update the Leaf Depth Neighbor Array
    partition.above = partition_context_lookup[bsize].above;
    partition.left = partition_context_lookup[bsize].left;

    neighbor_array_unit_mode_write(
        partition_context_neighbor_array,
        (uint8_t*)&partition,
        blkOriginX,
        blkOriginY,
        blk_geom->bwidth,
        blk_geom->bheight,
        NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

    // Update the Mode Type Neighbor Array
    {
        uint8_t prediction_mode_flag = (uint8_t)cu_ptr->prediction_mode_flag;
        neighbor_array_unit_mode_write(
            mode_type_neighbor_array,
            &prediction_mode_flag,
            blkOriginX,
            blkOriginY,
            blk_geom->bwidth,
            blk_geom->bheight,
            NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
    }

    // Update the Skip Flag Neighbor Array
    {
        uint8_t skip_flag = (uint8_t)cu_ptr->skip_flag;
        neighbor_array_unit_mode_write(
            skip_flag_neighbor_array,
            (uint8_t*)&skip_flag,
            blkOriginX,
            blkOriginY,
            blk_geom->bwidth,
            blk_geom->bheight,
            NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
    }

    // Update the Skip Coeff Neighbor Array
    {
        //
        neighbor_array_unit_mode_write(
            skip_coeff_neighbor_array,
            (uint8_t*)&skipCoeff,
            blkOriginX,
            blkOriginY,
            blk_geom->bwidth,
            blk_geom->bheight,
            NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
    }

    if (skipCoeff)
    {
        uint8_t dc_sign_level_coeff = 0;

        neighbor_array_unit_mode_write(
            luma_dc_sign_level_coeff_neighbor_array,
            (uint8_t*)&dc_sign_level_coeff,
            blkOriginX,
            blkOriginY,
            blk_geom->bwidth,
            blk_geom->bheight,
            NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

        if (blk_geom->has_uv)
            neighbor_array_unit_mode_write(
                cb_dc_sign_level_coeff_neighbor_array,
                (uint8_t*)&dc_sign_level_coeff,
                ((blkOriginX >> 3) << 3) >> 1,
                ((blkOriginY >> 3) << 3) >> 1,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

        if (blk_geom->has_uv)
            neighbor_array_unit_mode_write(
                cr_dc_sign_level_coeff_neighbor_array,
                (uint8_t*)&dc_sign_level_coeff,
                ((blkOriginX >> 3) << 3) >> 1,
                ((blkOriginY >> 3) << 3) >> 1,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

        context_ptr->coded_area_sb += blk_geom->bwidth * blk_geom->bheight;

        if (blk_geom->has_uv)
            context_ptr->coded_area_sb_uv += blk_geom->bwidth_uv * blk_geom->bheight_uv;
    }

    // Update the Inter Pred Type Neighbor Array
    {
        uint8_t inter_pred_direction_index = (uint8_t)cu_ptr->prediction_unit_array->inter_pred_direction_index;
        neighbor_array_unit_mode_write(
            inter_pred_dir_neighbor_array,
            (uint8_t*)&(inter_pred_direction_index),
            blkOriginX,
            blkOriginY,
            blk_geom->bwidth,
            blk_geom->bheight,
            NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
    }

    // Update the refFrame Type Neighbor Array
    {
        uint8_t ref_frame_type = (uint8_t)cu_ptr->prediction_unit_array[0].ref_frame_type;
        neighbor_array_unit_mode_write(
            ref_frame_type_neighbor_array,
            (uint8_t*)&(ref_frame_type),
            blkOriginX,
            blkOriginY,
            blk_geom->bwidth,
            blk_geom->bheight,
            NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
    }

    // Update the interpolation Type Neighbor Array
    {
        uint32_t interpolationType = cu_ptr->interp_filters;
        neighbor_array_unit_mode_write32(
            interpolation_type_neighbor_array,
            interpolationType,
            blkOriginX,
            blkOriginY,
            blk_geom->bwidth,
            blk_geom->bheight,
            NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
    }
    return return_error;
}
int32_t is_chroma_reference(int32_t mi_row, int32_t mi_col, BlockSize bsize,
    int32_t subsampling_x, int32_t subsampling_y);

static INLINE int av1_allow_palette(int allow_screen_content_tools,
    BlockSize sb_type) {
    return allow_screen_content_tools && block_size_wide[sb_type] <= 64 &&
        block_size_high[sb_type] <= 64 && sb_type >= BLOCK_8X8;
}

static INLINE int av1_get_palette_bsize_ctx(BlockSize bsize) {
    return num_pels_log2_lookup[bsize] - num_pels_log2_lookup[BLOCK_8X8];
}
static void write_palette_mode_info(
    FRAME_CONTEXT           *ec_ctx,
    CodingUnit            *cu_ptr,
    BlockSize bsize,
    int mi_row,
    int mi_col, AomWriter *w)
{
    const uint32_t intra_luma_mode = cu_ptr->pred_mode;
    uint32_t intra_chroma_mode = cu_ptr->prediction_unit_array->intra_chroma_mode;

    const int num_planes = 3;// av1_num_planes(cm);
    //const BLOCK_SIZE bsize = mbmi->sb_type;
    //assert(av1_allow_palette(cm->allow_screen_content_tools, bsize));
   // const PALETTE_MODE_INFO *const pmi = &mbmi->palette_mode_info;
    const int bsize_ctx = av1_get_palette_bsize_ctx(bsize);
    assert(bsize_ctx >= 0);
    if (intra_luma_mode == DC_PRED) {
        const int n = 0;// pmi->palette_size[0];
        const int palette_y_mode_ctx = 0;// av1_get_palette_mode_ctx(xd);
        aom_write_symbol(
            w, n > 0,
            ec_ctx->palette_y_mode_cdf[bsize_ctx][palette_y_mode_ctx], 2);
        if (n > 0) {
            //aom_write_symbol(w, n - PALETTE_MIN_SIZE,
            //    xd->tile_ctx->palette_y_size_cdf[bsize_ctx],
            //    PALETTE_SIZES);
            //write_palette_colors_y(xd, pmi, cm->seq_params.bit_depth, w);
        }
    }

    const int uv_dc_pred =
        num_planes > 1 && intra_chroma_mode == UV_DC_PRED &&
        is_chroma_reference(mi_row, mi_col, bsize, 1, 1);
    if (uv_dc_pred) {
        const int n = 0;// pmi->palette_size[1];
        const int palette_uv_mode_ctx = 0;// (pmi->palette_size[0] > 0);
        aom_write_symbol(w, n > 0,
            ec_ctx->palette_uv_mode_cdf[palette_uv_mode_ctx], 2);
        if (n > 0) {
            /*           aom_write_symbol(w, n - PALETTE_MIN_SIZE,
                           xd->tile_ctx->palette_uv_size_cdf[bsize_ctx],
                           PALETTE_SIZES);
                       write_palette_colors_uv(xd, pmi, cm->seq_params.bit_depth, w);*/
        }
    }
}
void av1_encode_dv(AomWriter *w, const MV *mv, const MV *ref,
    NmvContext *mvctx) {
    // DV and ref DV should not have sub-pel.
    assert((mv->col & 7) == 0);
    assert((mv->row & 7) == 0);
    assert((ref->col & 7) == 0);
    assert((ref->row & 7) == 0);
    const MV diff = { mv->row - ref->row, mv->col - ref->col };
    const MvJointType j = av1_get_mv_joint((int32_t*)&diff);

    aom_write_symbol(w, j, mvctx->joints_cdf, MV_JOINTS);
    if (mv_joint_vertical(j))
        encode_mv_component(w, diff.row, &mvctx->comps[0], MV_SUBPEL_NONE);

    if (mv_joint_horizontal(j))
        encode_mv_component(w, diff.col, &mvctx->comps[1], MV_SUBPEL_NONE);
}

int av1_allow_intrabc(const Av1Common *const cm) {
    return (cm->p_pcs_ptr->slice_type == I_SLICE && cm->p_pcs_ptr->frm_hdr.allow_screen_content_tools && cm->p_pcs_ptr->frm_hdr.allow_intrabc);
}

static void write_intrabc_info(
    FRAME_CONTEXT           *ec_ctx,
    CodingUnit            *cu_ptr,
    AomWriter *w) {
    int use_intrabc = cu_ptr->av1xd->use_intrabc;
    aom_write_symbol(w, use_intrabc, ec_ctx->intrabc_cdf, 2);
    if (use_intrabc) {
        //assert(mbmi->mode == DC_PRED);
        //assert(mbmi->uv_mode == UV_DC_PRED);
        //assert(mbmi->motion_mode == SIMPLE_TRANSLATION);
        IntMv dv_ref = cu_ptr->predmv[0];// mbmi_ext->ref_mv_stack[INTRA_FRAME][0].this_mv;
        MV mv;
        mv.row = cu_ptr->prediction_unit_array[0].mv[INTRA_FRAME].y;
        mv.col = cu_ptr->prediction_unit_array[0].mv[INTRA_FRAME].x;

        av1_encode_dv(w, &mv, &dv_ref.as_mv, &ec_ctx->ndvc);
    }
}


static INLINE int block_signals_txsize(BlockSize bsize) {
    return bsize > BLOCK_4X4;
}

static INLINE int is_intrabc_block(const MbModeInfo *mbmi) {
    return mbmi->use_intrabc;
}
static INLINE int get_vartx_max_txsize(/*const MbModeInfo *xd,*/ BlockSize bsize,
    int plane) {
    /* if (xd->lossless[xd->mi[0]->segment_id]) return TX_4X4;*/
    const TxSize max_txsize = max_txsize_rect_lookup[bsize];
    if (plane == 0) return max_txsize;            // luma
    return av1_get_adjusted_tx_size(max_txsize);  // chroma
}
static INLINE int max_block_wide(const MacroBlockD *xd, BlockSize bsize,
    int plane) {
    int max_blocks_wide = block_size_wide[bsize];
    const struct macroblockd_plane *const pd = &xd->plane[plane];

    if (xd->mb_to_right_edge < 0)
        max_blocks_wide += xd->mb_to_right_edge >> (3 + pd->subsampling_x);

    // Scale the width in the transform block unit.
    return max_blocks_wide >> tx_size_wide_log2[0];
}

static INLINE int max_block_high(const MacroBlockD *xd, BlockSize bsize,
    int plane) {
    int max_blocks_high = block_size_high[bsize];
    const struct macroblockd_plane *const pd = &xd->plane[plane];

    if (xd->mb_to_bottom_edge < 0)
        max_blocks_high += xd->mb_to_bottom_edge >> (3 + pd->subsampling_y);

    // Scale the height in the transform block unit.
    return max_blocks_high >> tx_size_high_log2[0];
}
static INLINE void txfm_partition_update(TXFM_CONTEXT *above_ctx,
    TXFM_CONTEXT *left_ctx,
    TxSize tx_size, TxSize txb_size) {
    BlockSize bsize = txsize_to_bsize[txb_size];
    int bh = mi_size_high[bsize];
    int bw = mi_size_wide[bsize];
    uint8_t txw = tx_size_wide[tx_size];
    uint8_t txh = tx_size_high[tx_size];
    int i;
    for (i = 0; i < bh; ++i) left_ctx[i] = txh;
    for (i = 0; i < bw; ++i) above_ctx[i] = txw;
}
static INLINE TxSize get_sqr_tx_size(int tx_dim) {
    switch (tx_dim) {
    case 128:
    case 64: return TX_64X64; break;
    case 32: return TX_32X32; break;
    case 16: return TX_16X16; break;
    case 8: return TX_8X8; break;
    default: return TX_4X4;
    }
}
static INLINE int txfm_partition_context(TXFM_CONTEXT *above_ctx,
    TXFM_CONTEXT *left_ctx,
    BlockSize bsize, TxSize tx_size) {
    const uint8_t txw = tx_size_wide[tx_size];
    const uint8_t txh = tx_size_high[tx_size];
    const int above = *above_ctx < txw;
    const int left = *left_ctx < txh;
    int category = TXFM_PARTITION_CONTEXTS;

    // dummy return, not used by others.
    if (tx_size <= TX_4X4) return 0;

    TxSize max_tx_size =
        get_sqr_tx_size(AOMMAX(block_size_wide[bsize], block_size_high[bsize]));

    if (max_tx_size >= TX_8X8) {
        category =
            (txsize_sqr_up_map[tx_size] != max_tx_size && max_tx_size > TX_8X8) +
            (TX_SIZES - 1 - max_tx_size) * 2;
    }
    assert(category != TXFM_PARTITION_CONTEXTS);
    return category * 3 + above + left;
}

static void write_tx_size_vartx(MacroBlockD *xd, const MbModeInfo *mbmi,
    TxSize tx_size, int depth, int blk_row,
    int blk_col, FRAME_CONTEXT *ec_ctx, AomWriter *w) {
    //FRAME_CONTEXT *ec_ctx = xd->tile_ctx;
    const int max_blocks_high = max_block_high(xd, mbmi->sb_type, 0);
    const int max_blocks_wide = max_block_wide(xd, mbmi->sb_type, 0);

    if (blk_row >= max_blocks_high || blk_col >= max_blocks_wide) return;

    if (depth == MAX_VARTX_DEPTH) {
        txfm_partition_update(xd->above_txfm_context + blk_col,
            xd->left_txfm_context + blk_row, tx_size, tx_size);
        return;
    }

    const int ctx = txfm_partition_context(xd->above_txfm_context + blk_col,
        xd->left_txfm_context + blk_row,
        mbmi->sb_type, tx_size);
    const int write_txfm_partition = (tx_size == tx_depth_to_tx_size[mbmi->tx_depth][mbmi->sb_type]);

    if (write_txfm_partition) {
        aom_write_symbol(w, 0, ec_ctx->txfm_partition_cdf[ctx], 2);

        txfm_partition_update(xd->above_txfm_context + blk_col,
            xd->left_txfm_context + blk_row, tx_size, tx_size);
    }
    else {
        const TxSize sub_txs = sub_tx_size_map[tx_size];
        const int bsw = tx_size_wide_unit[sub_txs];
        const int bsh = tx_size_high_unit[sub_txs];

        aom_write_symbol(w, 1, ec_ctx->txfm_partition_cdf[ctx], 2);

        if (sub_txs == TX_4X4) {
            txfm_partition_update(xd->above_txfm_context + blk_col,
                xd->left_txfm_context + blk_row, sub_txs, tx_size);
            return;
        }

        assert(bsw > 0 && bsh > 0);
        for (int row = 0; row < tx_size_high_unit[tx_size]; row += bsh)
            for (int col = 0; col < tx_size_wide_unit[tx_size]; col += bsw) {
                int offsetr = blk_row + row;
                int offsetc = blk_col + col;
                write_tx_size_vartx(xd, mbmi, sub_txs, depth + 1, offsetr, offsetc, ec_ctx, w);
            }
    }
}

static INLINE void set_txfm_ctx(TXFM_CONTEXT *txfm_ctx, uint8_t txs, int len) {
    int i;
    for (i = 0; i < len; ++i) txfm_ctx[i] = txs;
}

static INLINE void set_txfm_ctxs(TxSize tx_size, int n8_w, int n8_h, int skip,
    const MacroBlockD *xd) {
    uint8_t bw = tx_size_wide[tx_size];
    uint8_t bh = tx_size_high[tx_size];

    if (skip) {
        bw = n8_w * MI_SIZE;
        bh = n8_h * MI_SIZE;
    }

    set_txfm_ctx(xd->above_txfm_context, bw, n8_w);
    set_txfm_ctx(xd->left_txfm_context, bh, n8_h);
}
static INLINE int tx_size_to_depth(TxSize tx_size, BlockSize bsize) {
    TxSize ctx_size = max_txsize_rect_lookup[bsize];
    int depth = 0;
    while (tx_size != ctx_size) {
        depth++;
        ctx_size = sub_tx_size_map[ctx_size];
        assert(depth <= MAX_TX_DEPTH);
    }
    return depth;
}

static INLINE int bsize_to_max_depth(BlockSize bsize) {
    TxSize tx_size = max_txsize_rect_lookup[bsize];
    int depth = 0;
    while (depth < MAX_TX_DEPTH && tx_size != TX_4X4) {
        depth++;
        tx_size = sub_tx_size_map[tx_size];
    }
    return depth;
}
static INLINE int bsize_to_tx_size_cat(BlockSize bsize) {
    TxSize tx_size = max_txsize_rect_lookup[bsize];
    assert(tx_size != TX_4X4);
    int depth = 0;
    while (tx_size != TX_4X4) {
        depth++;
        tx_size = sub_tx_size_map[tx_size];
        assert(depth < 10);
    }
    assert(depth <= MAX_TX_CATS);
    return depth - 1;
}

// Returns a context number for the given MB prediction signal
// The mode info data structure has a one element border above and to the
// left of the entries corresponding to real blocks.
// The prediction flags in these dummy entries are initialized to 0.
static INLINE int get_tx_size_context(const MacroBlockD *xd) {
    const ModeInfo *mi = xd->mi[0];
    const MbModeInfo *mbmi = &mi->mbmi;
    const MbModeInfo *const above_mbmi = xd->above_mbmi;
    const MbModeInfo *const left_mbmi = xd->left_mbmi;
    const TxSize max_tx_size = max_txsize_rect_lookup[mbmi->sb_type];
    const int max_tx_wide = tx_size_wide[max_tx_size];
    const int max_tx_high = tx_size_high[max_tx_size];
    const int has_above = xd->up_available;
    const int has_left = xd->left_available;

    int above = xd->above_txfm_context[0] >= max_tx_wide;
    int left = xd->left_txfm_context[0] >= max_tx_high;

    if (has_above)
        if (is_inter_block(above_mbmi))
            above = block_size_wide[above_mbmi->sb_type] >= max_tx_wide;

    if (has_left)
        if (is_inter_block(left_mbmi))
            left = block_size_high[left_mbmi->sb_type] >= max_tx_high;

    if (has_above && has_left)
        return (above + left);
    else if (has_above)
        return above;
    else if (has_left)
        return left;
    else
        return 0;
}
static void write_selected_tx_size(
    const MacroBlockD *xd,
    FRAME_CONTEXT *ec_ctx,
    AomWriter *w) {
    const ModeInfo *const mi = xd->mi[0];
    const MbModeInfo *const mbmi = &mi->mbmi;
    const BlockSize bsize = mbmi->sb_type;

    if (block_signals_txsize(bsize)) {
        const TxSize tx_size = mbmi->tx_size;
        const int tx_size_ctx = get_tx_size_context(xd);
        const int depth = tx_size_to_depth(tx_size, bsize);
        const int max_depths = bsize_to_max_depth(bsize);
        const int32_t tx_size_cat = bsize_to_tx_size_cat(bsize);

        assert(depth >= 0 && depth <= max_depths);
        assert(!is_inter_block(mbmi));
        assert(IMPLIES(is_rect_tx(tx_size), is_rect_tx_allowed(/*xd,*/ mbmi)));

        aom_write_symbol(w, depth, ec_ctx->tx_size_cdf[tx_size_cat][tx_size_ctx],
            max_depths + 1);
    }
}
static EbErrorType av1_code_tx_size(
    FRAME_CONTEXT    *ec_ctx,
    AomWriter        *w,
    MacroBlockD      *xd,
    const MbModeInfo *mbmi,
    TxMode            tx_mode,
    BlockSize         bsize,
    uint8_t           skip) {
    EbErrorType return_error = EB_ErrorNone;
    int is_inter_tx = is_inter_block(mbmi) || is_intrabc_block(mbmi);
    //int skip = mbmi->skip;
    //int segment_id = 0;// mbmi->segment_id;
    if (tx_mode == TX_MODE_SELECT && block_signals_txsize(bsize) &&
        !(is_inter_tx && skip) /*&& !xd->lossless[segment_id]*/) {
        if (is_inter_tx) {  // This implies skip flag is 0.
            const TxSize max_tx_size = get_vartx_max_txsize(/*xd,*/ bsize, 0);
            const int txbh = tx_size_high_unit[max_tx_size];
            const int txbw = tx_size_wide_unit[max_tx_size];
            const int width = block_size_wide[bsize] >> tx_size_wide_log2[0];
            const int height = block_size_high[bsize] >> tx_size_high_log2[0];
            int idx, idy;
            for (idy = 0; idy < height; idy += txbh)
                for (idx = 0; idx < width; idx += txbw)
                    write_tx_size_vartx(xd, mbmi, max_tx_size, 0, idy, idx, ec_ctx, w);
        }
        else {
            write_selected_tx_size(xd, ec_ctx, w);
            set_txfm_ctxs(mbmi->tx_size, xd->n8_w, xd->n8_h, 0, xd);
        }
    }
    else {
        set_txfm_ctxs(mbmi->tx_size, xd->n8_w, xd->n8_h,
            skip && is_inter_block(mbmi), xd);
    }

    return return_error;
}

static INLINE void set_mi_row_col(
    PictureControlSet       *picture_control_set_ptr,
    MacroBlockD             *xd,
    TileInfo *              tile,
    int                     mi_row,
    int                     bh,
    int                     mi_col,
    int                     bw,
    uint32_t                mi_stride,
    int                     mi_rows,
    int                     mi_cols) {
    xd->mb_to_top_edge = -((mi_row * MI_SIZE) * 8);
    xd->mb_to_bottom_edge = ((mi_rows - bh - mi_row) * MI_SIZE) * 8;
    xd->mb_to_left_edge = -((mi_col * MI_SIZE) * 8);
    xd->mb_to_right_edge = ((mi_cols - bw - mi_col) * MI_SIZE) * 8;

    xd->mi_stride = mi_stride;

    // NM: To be updated when tile is supported.
    tile->mi_row_start = 0;
    tile->mi_col_start = 0;

    // Are edges available for intra prediction?
    xd->up_available = (mi_row > tile->mi_row_start);
    xd->left_available = (mi_col > tile->mi_col_start);
    const int32_t offset = mi_row * mi_stride + mi_col;
    xd->mi = picture_control_set_ptr->mi_grid_base + offset;

    if (xd->up_available)
        xd->above_mbmi = &xd->mi[-xd->mi_stride]->mbmi;
    else
        xd->above_mbmi = NULL;

    if (xd->left_available)
        xd->left_mbmi = &xd->mi[-1]->mbmi;
    else
        xd->left_mbmi = NULL;

    xd->n8_h = bh;
    xd->n8_w = bw;
    xd->is_sec_rect = 0;
    if (xd->n8_w < xd->n8_h) {
        // Only mark is_sec_rect as 1 for the last block.
        // For PARTITION_VERT_4, it would be (0, 0, 0, 1);
        // For other partitions, it would be (0, 1).
        if (!((mi_col + xd->n8_w) & (xd->n8_h - 1))) xd->is_sec_rect = 1;
    }

    if (xd->n8_w > xd->n8_h)
        if (mi_row & (xd->n8_w - 1)) xd->is_sec_rect = 1;
}

void code_tx_size(
    PictureControlSet *pcsPtr,
    uint32_t           cu_origin_x,
    uint32_t           cu_origin_y,
    CodingUnit        *cu_ptr,
    const BlockGeom   *blk_geom,
    NeighborArrayUnit *txfm_context_array,
    FRAME_CONTEXT     *ec_ctx,
    AomWriter         *w,
    uint8_t            skip) {
    uint32_t txfm_context_left_index = get_neighbor_array_unit_left_index(
        txfm_context_array,
        cu_origin_y);
    uint32_t txfm_context_above_index = get_neighbor_array_unit_top_index(
        txfm_context_array,
        cu_origin_x);
    TxMode tx_mode = pcsPtr->parent_pcs_ptr->frm_hdr.tx_mode;
    Av1Common  *cm = pcsPtr->parent_pcs_ptr->av1_cm;
    MacroBlockD *xd = cu_ptr->av1xd;
    TileInfo * tile = &xd->tile;
    int32_t mi_row = cu_origin_y >> MI_SIZE_LOG2;
    int32_t mi_col = cu_origin_x >> MI_SIZE_LOG2;
    BlockSize bsize = blk_geom->bsize;
    const int32_t bw = mi_size_wide[bsize];
    const int32_t bh = mi_size_high[bsize];
#if INCOMPLETE_SB_FIX
    uint32_t mi_stride = pcsPtr->mi_stride;
#else
    uint32_t mi_stride = pcsPtr->parent_pcs_ptr->sequence_control_set_ptr->picture_width_in_sb*(BLOCK_SIZE_64 >> MI_SIZE_LOG2);
#endif
    set_mi_row_col(
        pcsPtr,
        xd,
        tile,
        mi_row,
        bh,
        mi_col,
        bw,
        mi_stride,
        cm->mi_rows,
        cm->mi_cols);

    const MbModeInfo *const mbmi = &xd->mi[0]->mbmi;
    xd->above_txfm_context = &txfm_context_array->top_array[txfm_context_above_index];
    xd->left_txfm_context = &txfm_context_array->left_array[txfm_context_left_index];

    av1_code_tx_size(
        ec_ctx,
        w,
        xd,
        mbmi,
        tx_mode,
        bsize,
        skip);
}

static INLINE int get_segment_id(Av1Common *cm,
                               const uint8_t *segment_ids,
                               BlockSize bsize,
                               int mi_row, int mi_col) {
    const int mi_offset = mi_row * cm->mi_cols + mi_col;
    const int bw = mi_size_wide[bsize];
    const int bh = mi_size_high[bsize];
    const int xmis = AOMMIN(cm->mi_cols - mi_col, bw);
    const int ymis = AOMMIN(cm->mi_rows - mi_row, bh);
    int x, y, segment_id = MAX_SEGMENTS;

    for (y = 0; y < ymis; ++y)
        for (x = 0; x < xmis; ++x)
            segment_id =
                    AOMMIN(segment_id, segment_ids[mi_offset + y * cm->mi_cols + x]);

    assert(segment_id >= 0 && segment_id < MAX_SEGMENTS);
    return segment_id;
}

int get_spatial_seg_prediction(PictureControlSet *picture_control_set_ptr,
                               uint32_t blkOriginX,
                               uint32_t blkOriginY,
                               int *cdf_index) {

    int prev_ul = -1;  // top left segment_id
    int prev_l = -1;   // left segment_id
    int prev_u = -1;   // top segment_id

    uint32_t mi_col = blkOriginX >> MI_SIZE_LOG2;
    uint32_t mi_row = blkOriginY >> MI_SIZE_LOG2;

    EbBool left_available = mi_col > 0 ? EB_TRUE : EB_FALSE;
    EbBool up_available = mi_row > 0 ? EB_TRUE : EB_FALSE;
    Av1Common *cm = picture_control_set_ptr->parent_pcs_ptr->av1_cm;
    SegmentationNeighborMap *segmentation_map = picture_control_set_ptr->segmentation_neighbor_map;

//    SVT_LOG("Left available = %d, Up Available = %d ", left_available, up_available);

    if ((up_available) && (left_available))
        prev_ul = get_segment_id(cm, segmentation_map->data, BLOCK_4X4, mi_row - 1, mi_col - 1);

    if (up_available)
        prev_u = get_segment_id(cm, segmentation_map->data, BLOCK_4X4, mi_row - 1, mi_col - 0);

    if (left_available)
        prev_l = get_segment_id(cm, segmentation_map->data, BLOCK_4X4, mi_row - 0, mi_col - 1);

    // Pick CDF index based on number of matching/out-of-bounds segment IDs.
    if (prev_ul < 0 || prev_u < 0 || prev_l < 0) /* Edge case */
        *cdf_index = 0;
    else if ((prev_ul == prev_u) && (prev_ul == prev_l))
        *cdf_index = 2;
    else if ((prev_ul == prev_u) || (prev_ul == prev_l) || (prev_u == prev_l))
        *cdf_index = 1;
    else
        *cdf_index = 0;

    // If 2 or more are identical returns that as predictor, otherwise prev_l.
    if (prev_u == -1)  // edge case
        return prev_l == -1 ? 0 : prev_l;
    if (prev_l == -1)  // edge case
        return prev_u;
    return (prev_ul == prev_u) ? prev_u : prev_l;

}


int av1_neg_interleave(int x, int ref, int max) {
    assert(x < max);
    const int diff = x - ref;
    if (!ref) return x;
    if (ref >= (max - 1)) return -x + max - 1;
    if (2 * ref < max) {
        if (abs(diff) <= ref) {
            if (diff > 0)
                return (diff << 1) - 1;
            else
                return ((-diff) << 1);
        }
        return x;
    } else {
        if (abs(diff) < (max - ref)) {
            if (diff > 0)
                return (diff << 1) - 1;
            else
                return ((-diff) << 1);
        }
        return (max - x) - 1;
    }
}


int av1_get_pred_context_seg_id(PictureControlSet *picture_control_set_ptr,
                                CodingUnit *cu_ptr,
                                uint32_t blkOriginX,
                                uint32_t blkOriginY) {
    NeighborArrayUnit *seg_id_pred_neighbor_array = picture_control_set_ptr->segmentation_id_pred_array;
    uint32_t top_idx = get_neighbor_array_unit_top_index(seg_id_pred_neighbor_array, blkOriginX);
    uint32_t left_idx = get_neighbor_array_unit_left_index(seg_id_pred_neighbor_array, blkOriginY);

    const int above_pred = cu_ptr->av1xd->up_available ? seg_id_pred_neighbor_array->top_array[top_idx] : 0;
    const int left_pred = cu_ptr->av1xd->left_available ? seg_id_pred_neighbor_array->left_array[left_idx] : 0;
    return above_pred + left_pred;
}

AomCdfProb *av1_get_pred_cdf_seg_id(PictureControlSet *picture_control_set_ptr,
                                      FRAME_CONTEXT *frameContext,
                                      CodingUnit *cu_ptr,
                                      uint32_t blkOriginX,
                                      uint32_t blkOriginY) {
    struct segmentation_probs *segp = &frameContext->seg;
    return segp->spatial_pred_seg_cdf[av1_get_pred_context_seg_id(picture_control_set_ptr, cu_ptr, blkOriginX, blkOriginY)];
}

static INLINE void update_segmentation_map(PictureControlSet *picture_control_set_ptr,
                                           BlockSize bsize,
                                           uint32_t blkOriginX,
                                           uint32_t blkOriginY,
                                           uint8_t segment_id){

    Av1Common *cm = picture_control_set_ptr->parent_pcs_ptr->av1_cm;
    uint8_t *segment_ids = picture_control_set_ptr->segmentation_neighbor_map->data;
    uint32_t mi_col = blkOriginX >> MI_SIZE_LOG2;
    uint32_t mi_row = blkOriginY >> MI_SIZE_LOG2;
    const int mi_offset = mi_row * cm->mi_cols + mi_col;
    const int bw = mi_size_wide[bsize];
    const int bh = mi_size_high[bsize];
    const int xmis = AOMMIN((int)(cm->mi_cols - mi_col), bw);
    const int ymis = AOMMIN((int)(cm->mi_rows - mi_row), bh);
    int x, y;

    for (y = 0; y < ymis; ++y)
        for (x = 0; x < xmis; ++x)
            segment_ids[mi_offset + y * cm->mi_cols + x] = segment_id;

}

void write_segment_id(PictureControlSet *picture_control_set_ptr,
                      FRAME_CONTEXT *frameContext,
                      AomWriter *ecWriter,
                      BlockSize bsize,
                      uint32_t blkOriginX,
                      uint32_t blkOriginY,
                      CodingUnit *cu_ptr,
                      EbBool skip_coeff) {

    SegmentationParams *segmentationParams = &picture_control_set_ptr->parent_pcs_ptr->frm_hdr.segmentation_params;
    if (!segmentationParams->segmentation_enabled)
        return;
    int cdf_num;
    const int pred = get_spatial_seg_prediction(picture_control_set_ptr, blkOriginX, blkOriginY, &cdf_num);
    if (skip_coeff) {
//        SVT_LOG("BlockY = %d, BlockX = %d \n", blkOriginY>>2, blkOriginX>>2);
        update_segmentation_map(picture_control_set_ptr, bsize, blkOriginX, blkOriginY, pred);
        cu_ptr->segment_id = pred;
        return;
    }
    const int coded_id = av1_neg_interleave(cu_ptr->segment_id, pred, segmentationParams->last_active_seg_id + 1);
    struct segmentation_probs *segp = &frameContext->seg;
    AomCdfProb *pred_cdf = segp->spatial_pred_seg_cdf[cdf_num];
    aom_write_symbol(ecWriter, coded_id, pred_cdf, MAX_SEGMENTS);
    update_segmentation_map(picture_control_set_ptr, bsize, blkOriginX, blkOriginY, cu_ptr->segment_id);
//    SVT_LOG("BlockY = %d, BlockX = %d, Segmentation  pred = %d, skip_coeff = %d, coded_id = %d, pred_cdf = %d \n", blkOriginY>>2, blkOriginX>>2, pred, skip_coeff, coded_id, *pred_cdf);
}


void write_inter_segment_id(PictureControlSet *picture_control_set_ptr,
                            FRAME_CONTEXT *frameContext,
                            AomWriter *ecWriter,
                            const BlockGeom *blockGeom,
                            uint32_t blkOriginX,
                            uint32_t blkOriginY,
                            CodingUnit *cu_ptr,
                            EbBool skip,
                            int pre_skip) {
    SegmentationParams *segmentationParams = &picture_control_set_ptr->parent_pcs_ptr->frm_hdr.segmentation_params;
    if (!segmentationParams->segmentation_enabled)
        return;

    if (segmentationParams->segmentation_update_map) {
        if (pre_skip) {
            if (!segmentationParams->seg_id_pre_skip)
                return;
        } else {
            if (segmentationParams->seg_id_pre_skip)
                return;
            if (skip) {
                write_segment_id(picture_control_set_ptr, frameContext, ecWriter, blockGeom->bsize, blkOriginX,
                                 blkOriginY, cu_ptr, 1);
                if (segmentationParams->segmentation_temporal_update){
                    printf("ERROR: Temporal update is not supported yet! \n");
                    assert(0);
//                    cu_ptr->seg_id_predicted = 0;
                }
                return;
            }
        }

        if (segmentationParams->segmentation_temporal_update) {
            printf("ERROR: Temporal update is not supported yet! \n");
            assert(0);
//            const int pred_flag = cu_ptr->seg_id_predicted;
//            aom_cdf_prob *pred_cdf = av1_get_pred_cdf_seg_id(picture_control_set_ptr, frameContext, cu_ptr, blkOriginX, blkOriginY);
//            aom_write_symbol(ecWriter, pred_flag, pred_cdf, 2);
//            if (!pred_flag) {
//                WriteSegmentId(picture_control_set_ptr, frameContext, ecWriter, blockGeom->bsize, blkOriginX, blkOriginY, cu_ptr, 0);
//            }
//            if (pred_flag) {2
//                update_segmentation_map(picture_control_set_ptr, blockGeom->bsize, blkOriginX, blkOriginY, cu_ptr->segment_id);
//            }
//            neighbor_array_unit_mode_write(
//                    picture_control_set_ptr->segmentation_id_pred_array,
//                    &(cu_ptr->segment_id),
//                    blkOriginX,
//                    blkOriginY,
//                    blockGeom->bwidth,
//                    blockGeom->bheight,
//                    NEIGHBOR_ARRAY_UNIT_FULL_MASK);

        } else {
            write_segment_id(picture_control_set_ptr, frameContext, ecWriter, blockGeom->bsize, blkOriginX, blkOriginY,
                             cu_ptr, 0);
        }


    }


}


EbErrorType write_modes_b(
    PictureControlSet     *picture_control_set_ptr,
    EntropyCodingContext  *context_ptr,
    EntropyCoder          *entropy_coder_ptr,
    LargestCodingUnit     *tb_ptr,
    CodingUnit            *cu_ptr,
    EbPictureBufferDesc   *coeff_ptr)
{
    UNUSED(tb_ptr);
    EbErrorType return_error = EB_ErrorNone;
    FRAME_CONTEXT           *frameContext = entropy_coder_ptr->fc;
    AomWriter              *ec_writer = &entropy_coder_ptr->ec_writer;
    SequenceControlSet     *sequence_control_set_ptr = (SequenceControlSet*)picture_control_set_ptr->sequence_control_set_wrapper_ptr->object_ptr;
    FrameHeader *frm_hdr = &picture_control_set_ptr->parent_pcs_ptr->frm_hdr;

    NeighborArrayUnit     *mode_type_neighbor_array = picture_control_set_ptr->mode_type_neighbor_array;
    NeighborArrayUnit     *intra_luma_mode_neighbor_array = picture_control_set_ptr->intra_luma_mode_neighbor_array;
    NeighborArrayUnit     *skip_flag_neighbor_array = picture_control_set_ptr->skip_flag_neighbor_array;
    NeighborArrayUnit     *skip_coeff_neighbor_array = picture_control_set_ptr->skip_coeff_neighbor_array;
    NeighborArrayUnit     *luma_dc_sign_level_coeff_neighbor_array = picture_control_set_ptr->luma_dc_sign_level_coeff_neighbor_array;
    NeighborArrayUnit     *cr_dc_sign_level_coeff_neighbor_array = picture_control_set_ptr->cr_dc_sign_level_coeff_neighbor_array;
    NeighborArrayUnit     *cb_dc_sign_level_coeff_neighbor_array = picture_control_set_ptr->cb_dc_sign_level_coeff_neighbor_array;
    NeighborArrayUnit     *ref_frame_type_neighbor_array = picture_control_set_ptr->ref_frame_type_neighbor_array;
    NeighborArrayUnit32   *interpolation_type_neighbor_array = picture_control_set_ptr->interpolation_type_neighbor_array;
    NeighborArrayUnit     *txfm_context_array = picture_control_set_ptr->txfm_context_array;
    const BlockGeom          *blk_geom = get_blk_geom_mds(cu_ptr->mds_idx);
    uint32_t blkOriginX = context_ptr->sb_origin_x + blk_geom->origin_x;
    uint32_t blkOriginY = context_ptr->sb_origin_y + blk_geom->origin_y;
    BlockSize bsize = blk_geom->bsize;
    EbBool                   skipCoeff = EB_FALSE;
    skipCoeff = cu_ptr->block_has_coeff ? 0 : 1;

assert(bsize < BlockSizeS_ALL);
    int32_t mi_row = blkOriginY >> MI_SIZE_LOG2;
    int32_t mi_col = blkOriginX >> MI_SIZE_LOG2;
    int mi_stride = picture_control_set_ptr->parent_pcs_ptr->av1_cm->mi_stride;
    const int32_t offset = mi_row * mi_stride + mi_col;
    cu_ptr->av1xd->mi = picture_control_set_ptr->parent_pcs_ptr->av1_cm->pcs_ptr->mi_grid_base + offset;
    ModeInfo *mi_ptr = *cu_ptr->av1xd->mi;

    cu_ptr->av1xd->up_available = (mi_row > tb_ptr->tile_info.mi_row_start);
    cu_ptr->av1xd->left_available = (mi_col > tb_ptr->tile_info.mi_col_start);
    if (cu_ptr->av1xd->up_available)
        cu_ptr->av1xd->above_mbmi = &mi_ptr[-mi_stride].mbmi;
    else
        cu_ptr->av1xd->above_mbmi = NULL;
    if (cu_ptr->av1xd->left_available)
        cu_ptr->av1xd->left_mbmi = &mi_ptr[-1].mbmi;
    else
        cu_ptr->av1xd->left_mbmi = NULL;
    cu_ptr->av1xd->tile_ctx = frameContext;
    if (picture_control_set_ptr->slice_type == I_SLICE) {
        //const int32_t skip = write_skip(cm, xd, mbmi->segment_id, mi, w);
        if (picture_control_set_ptr->parent_pcs_ptr->frm_hdr.segmentation_params.seg_id_pre_skip)
            write_segment_id(picture_control_set_ptr, frameContext, ec_writer, blk_geom->bsize, blkOriginX, blkOriginY,
                             cu_ptr,
                             skipCoeff);

        EncodeSkipCoeffAv1(
            frameContext,
            ec_writer,
            skipCoeff,
            blkOriginX,
            blkOriginY,
            skip_coeff_neighbor_array);

        if (!picture_control_set_ptr->parent_pcs_ptr->frm_hdr.segmentation_params.seg_id_pre_skip)
            write_segment_id(picture_control_set_ptr, frameContext, ec_writer, blk_geom->bsize, blkOriginX, blkOriginY,
                             cu_ptr,
                             skipCoeff);

        write_cdef(
            sequence_control_set_ptr,
            picture_control_set_ptr,
            cu_ptr->av1xd,
            ec_writer,
            skipCoeff,
            blkOriginX >> MI_SIZE_LOG2,
            blkOriginY >> MI_SIZE_LOG2);

#if ADD_DELTA_QP_SUPPORT //PART 1
        if (picture_control_set_ptr->parent_pcs_ptr->delta_q_present_flag) {
            int32_t current_q_index = cu_ptr->qp;

            int32_t super_block_upper_left = (((blkOriginY >> 2) & (sequence_control_set_ptr->mib_size - 1)) == 0) && (((blkOriginX >> 2) & (sequence_control_set_ptr->mib_size - 1)) == 0);
            /*((mi_row & (cm->seq_params.mib_size - 1)) == 0) && ((mi_col & (cm->seq_params.mib_size - 1)) == 0);*/

            BlockSize bsize = cu_size == 64 ? BLOCK_64X64 : cu_size == 32 ? BLOCK_32X32 : cu_size == 16 ? BLOCK_16X16 : cu_size == 8 ? BLOCK_8X8 : BLOCK_4X4;
            if (cu_size == 8 && cu_ptr->prediction_mode_flag == INTRA_MODE && cu_ptr->pred_mode == INTRA_MODE_4x4)
                bsize = BLOCK_4X4;
            if ((bsize != sequence_control_set_ptr->sb_size || skipCoeff == 0) && super_block_upper_left) {
                assert(current_q_index > 0);
                int32_t reduced_delta_qindex = (current_q_index - picture_control_set_ptr->parent_pcs_ptr->prev_qindex) / frm_hdr->delta_q_params.delta_q_res;

                //write_delta_qindex(xd, reduced_delta_qindex, w);
                Av1writeDeltaQindex(
                    frameContext,
                    reduced_delta_qindex,
                    ec_writer);
                /*if (picture_control_set_ptr->picture_number == 0){
                printf("%d\t%d\t%d\t%d\n",
                blkOriginX,
                blkOriginY,
                current_q_index,
                picture_control_set_ptr->parent_pcs_ptr->prev_qindex);
                }*/
                picture_control_set_ptr->parent_pcs_ptr->prev_qindex = current_q_index;
            }
        }
#endif

        {
            const uint32_t intra_luma_mode = cu_ptr->pred_mode;
            uint32_t intra_chroma_mode = cu_ptr->prediction_unit_array->intra_chroma_mode;

            if (av1_allow_intrabc(picture_control_set_ptr->parent_pcs_ptr->av1_cm))
                write_intrabc_info(frameContext, cu_ptr, ec_writer);

            if (cu_ptr->av1xd->use_intrabc == 0)
            EncodeIntraLumaModeAv1(
                frameContext,
                ec_writer,
                cu_ptr,
                blkOriginX,
                blkOriginY,
                bsize,
                intra_luma_mode,
                mode_type_neighbor_array,
                intra_luma_mode_neighbor_array);

            neighbor_array_unit_mode_write(
                intra_luma_mode_neighbor_array,
                (uint8_t*)&intra_luma_mode,
                blkOriginX,
                blkOriginY,
                blk_geom->bwidth,
                blk_geom->bheight,
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

            if (cu_ptr->av1xd->use_intrabc == 0)
                if (blk_geom->has_uv)
                    EncodeIntraChromaModeAv1(
                        frameContext,
                        ec_writer,
                        cu_ptr,
                        bsize,
                        intra_luma_mode,
                        intra_chroma_mode,
                        blk_geom->bwidth <= 32 && blk_geom->bheight <= 32);

            if (cu_ptr->av1xd->use_intrabc == 0 && av1_allow_palette(frm_hdr->allow_screen_content_tools, blk_geom->bsize))
                write_palette_mode_info(
                    frameContext,
                    cu_ptr,
                    blk_geom->bsize,
                    blkOriginY >> MI_SIZE_LOG2,
                    blkOriginX >> MI_SIZE_LOG2,
                    ec_writer);

            if (frm_hdr->tx_mode == TX_MODE_SELECT) {
                code_tx_size(
                    picture_control_set_ptr,
                    blkOriginX,
                    blkOriginY,
                    cu_ptr,
                    blk_geom,
                    txfm_context_array,
                    frameContext,
                    ec_writer,
                    skipCoeff);
            }
            if (!skipCoeff) {
                Av1EncodeCoeff1D(
                    picture_control_set_ptr,
                    context_ptr,
                    frameContext,
                    ec_writer,
                    cu_ptr,
                    blkOriginX,
                    blkOriginY,
                    intra_luma_mode,
                    bsize,
                    coeff_ptr,
                    luma_dc_sign_level_coeff_neighbor_array,
                    cr_dc_sign_level_coeff_neighbor_array,
                    cb_dc_sign_level_coeff_neighbor_array);
            }
        }
    }
    else {
        write_inter_segment_id(picture_control_set_ptr, frameContext, ec_writer, blk_geom, blkOriginX, blkOriginY,
                               cu_ptr,
                               0, 1);
        if (picture_control_set_ptr->parent_pcs_ptr->skip_mode_flag && is_comp_ref_allowed(bsize)) {
            EncodeSkipModeAv1(
                frameContext,
                ec_writer,
                cu_ptr->skip_flag,
                blkOriginX,
                blkOriginY,
                skip_flag_neighbor_array);
        }
        if (!picture_control_set_ptr->parent_pcs_ptr->skip_mode_flag && cu_ptr->skip_flag)
            printf("ERROR[AN]: SKIP not supported\n");
        if (!cu_ptr->skip_flag) {
            //const int32_t skip = write_skip(cm, xd, mbmi->segment_id, mi, w);
            EncodeSkipCoeffAv1(
                frameContext,
                ec_writer,
                skipCoeff,
                blkOriginX,
                blkOriginY,
                skip_coeff_neighbor_array);
        }

        write_inter_segment_id(picture_control_set_ptr, frameContext, ec_writer, blk_geom, blkOriginX, blkOriginY,
                               cu_ptr,
                               skipCoeff, 0);
        write_cdef(
            sequence_control_set_ptr,
            picture_control_set_ptr, /*cm,*/
            cu_ptr->av1xd,
            ec_writer,
            cu_ptr->skip_flag ? 1 : skipCoeff,
            blkOriginX >> MI_SIZE_LOG2,
            blkOriginY >> MI_SIZE_LOG2);
#if ADD_DELTA_QP_SUPPORT//PART 2
        if (picture_control_set_ptr->parent_pcs_ptr->delta_q_present_flag) {
            int32_t current_q_index = cu_ptr->qp;

            int32_t super_block_upper_left = (((blkOriginY >> 2) & (sequence_control_set_ptr->mib_size - 1)) == 0) && (((blkOriginX >> 2) & (sequence_control_set_ptr->mib_size - 1)) == 0);
            /*((mi_row & (cm->seq_params.mib_size - 1)) == 0) && ((mi_col & (cm->seq_params.mib_size - 1)) == 0);*/

            BlockSize bsize = cu_size == 64 ? BLOCK_64X64 : cu_size == 32 ? BLOCK_32X32 : cu_size == 16 ? BLOCK_16X16 : cu_size == 8 ? BLOCK_8X8 : BLOCK_4X4;
            if (cu_size == 8 && cu_ptr->prediction_mode_flag == INTRA_MODE && cu_ptr->pred_mode == INTRA_MODE_4x4)
                bsize = BLOCK_4X4;
            if ((bsize != sequence_control_set_ptr->sb_size || skipCoeff == 0) && super_block_upper_left) {
                assert(current_q_index > 0);

                int32_t reduced_delta_qindex = (current_q_index - picture_control_set_ptr->parent_pcs_ptr->prev_qindex) / picture_control_set_ptr->parent_pcs_ptr->delta_q_res;

                //write_delta_qindex(xd, reduced_delta_qindex, w);

                Av1writeDeltaQindex(
                    frameContext,
                    reduced_delta_qindex,
                    ec_writer);

                picture_control_set_ptr->parent_pcs_ptr->prev_qindex = current_q_index;
            }
        }

#endif
        if (frm_hdr->tx_mode == TX_MODE_SELECT) {
            if (cu_ptr->skip_flag) {
                code_tx_size(
                    picture_control_set_ptr,
                    blkOriginX,
                    blkOriginY,
                    cu_ptr,
                    blk_geom,
                    txfm_context_array,
                    frameContext,
                    ec_writer,
                    cu_ptr->skip_flag);
            }
        }
        if (!cu_ptr->skip_flag) {
            //write_is_inter(cm, xd, mbmi->segment_id, w, is_inter)
            EncodePredModeAv1(
                frameContext,
                ec_writer,
                cu_ptr->prediction_mode_flag,
                blkOriginX,
                blkOriginY,
                mode_type_neighbor_array);

            if (cu_ptr->prediction_mode_flag == INTRA_MODE) {
                uint32_t intra_luma_mode = cu_ptr->pred_mode;
                uint32_t intra_chroma_mode = cu_ptr->prediction_unit_array->intra_chroma_mode;

                EncodeIntraLumaModeNonKeyAv1(
                    frameContext,
                    ec_writer,
                    cu_ptr,
                    bsize,
                    intra_luma_mode);

                neighbor_array_unit_mode_write(
                    intra_luma_mode_neighbor_array,
                    (uint8_t*)&intra_luma_mode,
                    blkOriginX,
                    blkOriginY,
                    blk_geom->bwidth,
                    blk_geom->bheight,
                    NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

                if (blk_geom->has_uv)
                    EncodeIntraChromaModeAv1(
                        frameContext,
                        ec_writer,
                        cu_ptr,
                        bsize,
                        intra_luma_mode,
                        intra_chroma_mode,
                        blk_geom->bwidth <= 32 && blk_geom->bheight <= 32);
            }
            else {
                av1_collect_neighbors_ref_counts_new(cu_ptr->av1xd);

                write_ref_frames(
                    frameContext,
                    picture_control_set_ptr->parent_pcs_ptr,
                    cu_ptr->av1xd,
                    ec_writer);

                MvReferenceFrame rf[2];
                av1_set_ref_frame(rf, cu_ptr->prediction_unit_array[0].ref_frame_type);
                int16_t mode_ctx = Av1ModeContextAnalyzer(cu_ptr->inter_mode_ctx, rf);
                PredictionMode inter_mode = (PredictionMode)cu_ptr->prediction_unit_array[0].inter_mode;
                const int32_t is_compound = (cu_ptr->prediction_unit_array[0].inter_pred_direction_index == BI_PRED);

                // If segment skip is not enabled code the mode.
                if (1) {
                    if (is_inter_compound_mode(inter_mode)) {
                        WriteInterCompoundMode(
                            frameContext,
                            ec_writer,
                            inter_mode,
                            mode_ctx);
                    }
                    else if (is_inter_singleref_mode(inter_mode))
                        WriteInterMode(
                            frameContext,
                            ec_writer,
                            inter_mode,
                            mode_ctx,
                            blkOriginX,
                            blkOriginY);

                    if (inter_mode == NEWMV || inter_mode == NEW_NEWMV || have_nearmv_in_inter_mode(inter_mode)) {
                        WriteDrlIdx(
                            frameContext,
                            ec_writer,
                            cu_ptr);
                    }
                }

                if (inter_mode == NEWMV || inter_mode == NEW_NEWMV) {
                    IntMv ref_mv;

                    for (uint8_t ref = 0; ref < 1 + is_compound; ++ref) {
                        NmvContext *nmvc = &frameContext->nmvc;
                        ref_mv = cu_ptr->predmv[ref];

                        MV mv;
                        mv.row = cu_ptr->prediction_unit_array[0].mv[ref].y;
                        mv.col = cu_ptr->prediction_unit_array[0].mv[ref].x;
                        if (cu_ptr->prediction_unit_array[0].inter_pred_direction_index == UNI_PRED_LIST_1)
                        {
                            mv.row = cu_ptr->prediction_unit_array[0].mv[1].y;
                            mv.col = cu_ptr->prediction_unit_array[0].mv[1].x;
                        }

                        av1_encode_mv(
                            picture_control_set_ptr->parent_pcs_ptr,
                            ec_writer,
                            &mv,
                            &ref_mv.as_mv,
                            nmvc,
                            frm_hdr->allow_high_precision_mv);
                    }
                }
                else if (inter_mode == NEAREST_NEWMV || inter_mode == NEAR_NEWMV) {
                    NmvContext *nmvc = &frameContext->nmvc;
                    IntMv ref_mv = cu_ptr->predmv[1];

                    MV mv;
                    mv.row = cu_ptr->prediction_unit_array[0].mv[1].y;
                    mv.col = cu_ptr->prediction_unit_array[0].mv[1].x;

                    av1_encode_mv(
                        picture_control_set_ptr->parent_pcs_ptr,
                        ec_writer,
                        &mv,
                        &ref_mv.as_mv,
                        nmvc,
                        frm_hdr->allow_high_precision_mv);
                }
                else if (inter_mode == NEW_NEARESTMV || inter_mode == NEW_NEARMV) {
                    NmvContext *nmvc = &frameContext->nmvc;
                    IntMv ref_mv = cu_ptr->predmv[0];

                    MV mv;
                    mv.row = cu_ptr->prediction_unit_array[0].mv[0].y;
                    mv.col = cu_ptr->prediction_unit_array[0].mv[0].x;

                    av1_encode_mv(
                        picture_control_set_ptr->parent_pcs_ptr,
                        ec_writer,
                        &mv,
                        &ref_mv.as_mv,
                        nmvc,
                        frm_hdr->allow_high_precision_mv);
                }

                if (frm_hdr->is_motion_mode_switchable
                    && rf[1] != INTRA_FRAME) {
                    write_motion_mode(
                        frameContext,
                        ec_writer,
                        bsize,
                        cu_ptr->prediction_unit_array[0].motion_mode,
                        rf[0],
                        rf[1],
                        cu_ptr,
                        picture_control_set_ptr);
                }

                if (sequence_control_set_ptr->seq_header.enable_masked_compound || sequence_control_set_ptr->seq_header.order_hint_info.enable_jnt_comp)
                    printf("ERROR[AN]: masked_compound_used and enable_jnt_comp not supported\n");

                // No filter for Global MV
                write_mb_interp_filter(
                    ref_frame_type_neighbor_array,
                    bsize,
                    rf[0],
                    rf[1],
                    picture_control_set_ptr->parent_pcs_ptr,
                    ec_writer,
                    cu_ptr,
                    entropy_coder_ptr,
                    interpolation_type_neighbor_array,
                    blkOriginX,
                    blkOriginY
                );
            }
            if (frm_hdr->tx_mode == TX_MODE_SELECT) {
                code_tx_size(
                    picture_control_set_ptr,
                    blkOriginX,
                    blkOriginY,
                    cu_ptr,
                    blk_geom,
                    txfm_context_array,
                    frameContext,
                    ec_writer,
                    skipCoeff);
            }
            if (!skipCoeff) {
                uint32_t intra_luma_mode = DC_PRED;
                if (cu_ptr->prediction_mode_flag == INTRA_MODE)
                    intra_luma_mode = (uint32_t)cu_ptr->pred_mode;

                {
                    Av1EncodeCoeff1D(
                        picture_control_set_ptr,
                        context_ptr,
                        frameContext,
                        ec_writer,
                        cu_ptr,
                        blkOriginX,
                        blkOriginY,
                        intra_luma_mode,
                        bsize,
                        coeff_ptr,
                        luma_dc_sign_level_coeff_neighbor_array,
                        cr_dc_sign_level_coeff_neighbor_array,
                        cb_dc_sign_level_coeff_neighbor_array);
                }
            }
        }
    }
    // Update the neighbors
    ec_update_neighbors(
        picture_control_set_ptr,
        context_ptr,
        blkOriginX,
        blkOriginY,
        cu_ptr,
        bsize,
        coeff_ptr);

    return return_error;
}
/**********************************************
* Write sb
**********************************************/
EB_EXTERN EbErrorType write_sb(
    EntropyCodingContext  *context_ptr,
    LargestCodingUnit     *tb_ptr,
    PictureControlSet     *picture_control_set_ptr,
    EntropyCoder          *entropy_coder_ptr,
    EbPictureBufferDesc   *coeff_ptr)
{
    EbErrorType return_error = EB_ErrorNone;
    FRAME_CONTEXT           *frameContext = entropy_coder_ptr->fc;
    AomWriter              *ec_writer = &entropy_coder_ptr->ec_writer;
    SequenceControlSet     *sequence_control_set_ptr = (SequenceControlSet*)picture_control_set_ptr->sequence_control_set_wrapper_ptr->object_ptr;
    NeighborArrayUnit     *partition_context_neighbor_array = picture_control_set_ptr->partition_context_neighbor_array;

    // CU Varaiables
    const BlockGeom          *blk_geom;
    CodingUnit             *cu_ptr;
    uint32_t                    cu_index = 0;
    uint32_t                    final_cu_index = 0;
    uint32_t                    cu_origin_x;
    uint32_t                    cu_origin_y;
    BlockSize                bsize;

    context_ptr->coded_area_sb = 0;
    context_ptr->coded_area_sb_uv = 0;
    EbBool checkCuOutOfBound = EB_FALSE;

    SbGeom * sb_geom = &sequence_control_set_ptr->sb_geom[tb_ptr->index];// .block_is_inside_md_scan[blk_index])

    if (!(sb_geom->is_complete_sb))
        checkCuOutOfBound = EB_TRUE;
    do {
        EbBool codeCuCond = EB_TRUE; // Code cu only if it is inside the picture

        cu_ptr = &tb_ptr->final_cu_arr[final_cu_index];

        blk_geom = get_blk_geom_mds(cu_index); // AMIR to be replaced with /*cu_ptr->mds_idx*/

        bsize = blk_geom->bsize;
        assert(bsize < BlockSizeS_ALL);
        cu_origin_x = context_ptr->sb_origin_x + blk_geom->origin_x;
        cu_origin_y = context_ptr->sb_origin_y + blk_geom->origin_y;
        if (checkCuOutOfBound) {
#if  INCOMPLETE_SB_FIX
            if (blk_geom->shape != PART_N)
                blk_geom = get_blk_geom_mds(blk_geom->sqi_mds);
            codeCuCond = EB_FALSE;
            if (((cu_origin_x + blk_geom->bwidth / 2 < sequence_control_set_ptr->seq_header.max_frame_width) || (cu_origin_y + blk_geom->bheight / 2 < sequence_control_set_ptr->seq_header.max_frame_height)) &&
                cu_origin_x < sequence_control_set_ptr->seq_header.max_frame_width && cu_origin_y < sequence_control_set_ptr->seq_header.max_frame_height)
#else
            codeCuCond = (EbBool)sb_geom->block_is_inside_md_scan[cu_index]; // check if cu is inside the picture

            if ((cu_origin_x < sequence_control_set_ptr->seq_header.max_frame_width) && (cu_origin_y < sequence_control_set_ptr->seq_header.max_frame_height))
#endif
                codeCuCond = EB_TRUE;
        }

        if (codeCuCond) {
            uint32_t blkOriginX = cu_origin_x;
            uint32_t blkOriginY = cu_origin_y;

            const int32_t hbs = mi_size_wide[bsize] >> 1;
            const int32_t quarter_step = mi_size_wide[bsize] >> 2;
            Av1Common* cm = picture_control_set_ptr->parent_pcs_ptr->av1_cm;
            int32_t mi_row = blkOriginY >> MI_SIZE_LOG2;
            int32_t mi_col = blkOriginX >> MI_SIZE_LOG2;

            if (bsize >= BLOCK_8X8) {
                for (int32_t plane = 0; plane < 3; ++plane) {
                    int32_t rcol0, rcol1, rrow0, rrow1, tile_tl_idx;
                    if (av1_loop_restoration_corners_in_sb(cm, plane, mi_row, mi_col, bsize,
                        &rcol0, &rcol1, &rrow0, &rrow1,
                        &tile_tl_idx)) {
                        const int32_t rstride = cm->rst_info[plane].horz_units_per_tile;
                        for (int32_t rrow = rrow0; rrow < rrow1; ++rrow) {
                            for (int32_t rcol = rcol0; rcol < rcol1; ++rcol) {
                                const int32_t runit_idx = tile_tl_idx + rcol + rrow * rstride;
                                const RestorationUnitInfo *rui =
                                    &cm->rst_info[plane].unit_info[runit_idx];
                                loop_restoration_write_sb_coeffs(picture_control_set_ptr, frameContext, cm, /*xd,*/ rui, ec_writer, plane);
                            }
                        }
                    }
                }

                // Code Split Flag
                EncodePartitionAv1(
                    sequence_control_set_ptr,
                    frameContext,
                    ec_writer,
                    bsize,
                    tb_ptr->cu_partition_array[cu_index],
                    blkOriginX,
                    blkOriginY,
                    partition_context_neighbor_array);
            }

            switch (tb_ptr->cu_partition_array[cu_index]) {
            case PARTITION_NONE:
                write_modes_b(
                    picture_control_set_ptr,
                    context_ptr,
                    entropy_coder_ptr,
                    tb_ptr,
                    cu_ptr,
                    coeff_ptr);
                break;

            case PARTITION_HORZ:
                write_modes_b(
                    picture_control_set_ptr,
                    context_ptr,
                    entropy_coder_ptr,
                    tb_ptr,
                    cu_ptr,
                    coeff_ptr);

                if (mi_row + hbs < cm->mi_rows) {
                    final_cu_index++;
                    cu_ptr = &tb_ptr->final_cu_arr[final_cu_index];
                    write_modes_b(
                        picture_control_set_ptr,
                        context_ptr,
                        entropy_coder_ptr,
                        tb_ptr,
                        cu_ptr,
                        coeff_ptr);
                }
                break;

            case PARTITION_VERT:
                write_modes_b(
                    picture_control_set_ptr,
                    context_ptr,
                    entropy_coder_ptr,
                    tb_ptr,
                    cu_ptr,
                    coeff_ptr);
                if (mi_col + hbs < cm->mi_cols) {
                    final_cu_index++;
                    cu_ptr = &tb_ptr->final_cu_arr[final_cu_index];
                    write_modes_b(
                        picture_control_set_ptr,
                        context_ptr,
                        entropy_coder_ptr,
                        tb_ptr,
                        cu_ptr,
                        coeff_ptr);
                }
                break;
            case PARTITION_SPLIT:
                break;
            case PARTITION_HORZ_A:
                write_modes_b(
                    picture_control_set_ptr,
                    context_ptr,
                    entropy_coder_ptr,
                    tb_ptr,
                    cu_ptr,
                    coeff_ptr);

                final_cu_index++;
                cu_ptr = &tb_ptr->final_cu_arr[final_cu_index];
                write_modes_b(
                    picture_control_set_ptr,
                    context_ptr,
                    entropy_coder_ptr,
                    tb_ptr,
                    cu_ptr,
                    coeff_ptr);

                final_cu_index++;
                cu_ptr = &tb_ptr->final_cu_arr[final_cu_index];
                write_modes_b(
                    picture_control_set_ptr,
                    context_ptr,
                    entropy_coder_ptr,
                    tb_ptr,
                    cu_ptr,
                    coeff_ptr);

                break;
            case PARTITION_HORZ_B:
                write_modes_b(
                    picture_control_set_ptr,
                    context_ptr,
                    entropy_coder_ptr,
                    tb_ptr,
                    cu_ptr,
                    coeff_ptr);

                final_cu_index++;
                cu_ptr = &tb_ptr->final_cu_arr[final_cu_index];
                write_modes_b(
                    picture_control_set_ptr,
                    context_ptr,
                    entropy_coder_ptr,
                    tb_ptr,
                    cu_ptr,
                    coeff_ptr);

                final_cu_index++;
                cu_ptr = &tb_ptr->final_cu_arr[final_cu_index];
                write_modes_b(
                    picture_control_set_ptr,
                    context_ptr,
                    entropy_coder_ptr,
                    tb_ptr,
                    cu_ptr,
                    coeff_ptr);

                break;
            case PARTITION_VERT_A:
                write_modes_b(
                    picture_control_set_ptr,
                    context_ptr,
                    entropy_coder_ptr,
                    tb_ptr,
                    cu_ptr,
                    coeff_ptr);

                final_cu_index++;
                cu_ptr = &tb_ptr->final_cu_arr[final_cu_index];
                write_modes_b(
                    picture_control_set_ptr,
                    context_ptr,
                    entropy_coder_ptr,
                    tb_ptr,
                    cu_ptr,
                    coeff_ptr);

                final_cu_index++;
                cu_ptr = &tb_ptr->final_cu_arr[final_cu_index];
                write_modes_b(
                    picture_control_set_ptr,
                    context_ptr,
                    entropy_coder_ptr,
                    tb_ptr,
                    cu_ptr,
                    coeff_ptr);

                break;
            case PARTITION_VERT_B:
                write_modes_b(
                    picture_control_set_ptr,
                    context_ptr,
                    entropy_coder_ptr,
                    tb_ptr,
                    cu_ptr,
                    coeff_ptr);

                final_cu_index++;
                cu_ptr = &tb_ptr->final_cu_arr[final_cu_index];
                write_modes_b(
                    picture_control_set_ptr,
                    context_ptr,
                    entropy_coder_ptr,
                    tb_ptr,
                    cu_ptr,
                    coeff_ptr);

                final_cu_index++;
                cu_ptr = &tb_ptr->final_cu_arr[final_cu_index];
                write_modes_b(
                    picture_control_set_ptr,
                    context_ptr,
                    entropy_coder_ptr,
                    tb_ptr,
                    cu_ptr,
                    coeff_ptr);

                break;
            case PARTITION_HORZ_4:
                for (int32_t i = 0; i < 4; ++i) {
                    int32_t this_mi_row = mi_row + i * quarter_step;
                    if (i > 0 && this_mi_row >= cm->mi_rows) break;

                    if (i > 0) {
                        final_cu_index++;
                        cu_ptr = &tb_ptr->final_cu_arr[final_cu_index];
                    }
                    write_modes_b(
                        picture_control_set_ptr,
                        context_ptr,
                        entropy_coder_ptr,
                        tb_ptr,
                        cu_ptr,
                        coeff_ptr);
                }
                break;
            case PARTITION_VERT_4:
                for (int32_t i = 0; i < 4; ++i) {
                    int32_t this_mi_col = mi_col + i * quarter_step;
                    if (i > 0 && this_mi_col >= cm->mi_cols) break;
                    if (i > 0) {
                        final_cu_index++;
                        cu_ptr = &tb_ptr->final_cu_arr[final_cu_index];
                    }
                    write_modes_b(
                        picture_control_set_ptr,
                        context_ptr,
                        entropy_coder_ptr,
                        tb_ptr,
                        cu_ptr,
                        coeff_ptr);
                }
                break;
            default: assert(0);
            }

            if (tb_ptr->cu_partition_array[cu_index] != PARTITION_SPLIT) {
                final_cu_index++;
                cu_index += ns_depth_offset[sequence_control_set_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth];
            }
            else
                cu_index += d1_depth_offset[sequence_control_set_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth];
        }
        else
            ++cu_index;
    } while (cu_index < sequence_control_set_ptr->max_block_cnt);
    return return_error;
}
