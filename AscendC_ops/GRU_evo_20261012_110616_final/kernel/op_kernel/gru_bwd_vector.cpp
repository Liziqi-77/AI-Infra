/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

/*!
 * \file gru_bwd_vector.cpp
 * \brief Vector-side kernels for the GRU backward pass.
 *
 *   gru_bwd_hprev          hp[:, 0] = 0 ; hp[:, t] = out[:, t-1]     (BPTT needs h_{t-1})
 *   gru_bwd_cell           one reverse cell for a single timestep
 *   gru_bwd_colsum_partial per-core column sums of dGx / dGh (bias gradients, pass 1)
 *   gru_bwd_colsum_final   reduce the per-core partials into db_ih / db_hh (pass 2)
 *   gru_bwd_add            dst += src (cross-chunk accumulation of the parameter gradients)
 *
 * Reverse cell (dht = dL/dh_t is the incoming gradient, dyt = dL/dout[:, t, :]):
 *
 *   dht   = dyt + dha + dhb       (dha: dL/dh_{t+1} direct; dhb: via gh_{t+1})
 *   r     = sigmoid(gx_r + gh_r)  z = sigmoid(gx_z + gh_z)
 *   n     = tanh(gx_n + r * gh_n) hp = h_{t-1}
 *   dn    = dht * (1 - z)         dz = dht * (hp - n)
 *   dun   = dn * (1 - n^2)        duz = dz * z * (1 - z)
 *   dur   = dun * gh_n * r * (1 - r)
 *   dgx   = (dur, duz, dun)       dgh = (dur, duz, dun * r)
 *   dha'  = dht * z               (contribution to dL/dh_{t-1} through h_t)
 *
 * dgh is emitted twice: strided into the [rows, S, 3H] buffer consumed by the
 * final dW_hh GEMM, and contiguously into the [rows, 3H] scratch consumed by the
 * per-step recurrent GEMM (dhb = dgh @ w_hh). Both transfers read the same UB
 * tile, so no extra buffer is needed.
 *
 * sigmoid / tanh are built from exp + Div against a constant-1 tensor; Reciprocal
 * stays below fp32 accuracy, exactly as in the forward cell.
 */

#include "gru_common.h"

using namespace AscendC;
using namespace gru_kernel;

namespace {

// ---------------------------------------------------------------------------
// 1) hprev = shift(out)
// ---------------------------------------------------------------------------
constexpr int64_t BWD_SHIFT_CHUNK = 8192;  // floats per UB transfer

__aicore__ inline void BwdShiftBody(GM_ADDR out_, GM_ADDR hp_, GM_ADDR tilingGM)
{
    GruBwdTilingData td;
    CopyBwdTilingToLocal(&td, tilingGM);
    const int64_t rows = td.chunkRows;
    const int64_t S = td.S;
    const int64_t H = td.H;

    const int64_t blkNum = (int64_t)GetBlockNum();
    const int64_t blk = (int64_t)GetBlockIdx();

    TPipe pipe;
    TQue<QuePosition::VECIN, 1> qIn;
    TQue<QuePosition::VECOUT, 1> qOut;
    pipe.InitBuffer(qIn, 1, BWD_SHIFT_CHUNK * sizeof(float));
    // hp is fp16 (GEMM operand precision); `out` stays fp32.
    pipe.InitBuffer(qOut, 1, BWD_SHIFT_CHUNK * sizeof(half));

    GlobalTensor<float> outG;
    outG.SetGlobalBuffer((__gm__ float *)out_);
    GlobalTensor<half> hpG;
    hpG.SetGlobalBuffer((__gm__ half *)hp_);

    // hp is written step-major [S, rows, H] (hp[t, b, :] = out[b, t-1, :]) so
    // that the recurrent GEMM's step-t operand is one contiguous block and the
    // dW_hh GEMM sees hp and dgh under the same row permutation. `out` stays
    // the caller's row-major [rows, S, H], so the copy is a strided read of H
    // floats per (b, t) into a contiguous destination.
    const int64_t rowsPerCore = (rows + blkNum - 1) / blkNum;
    const int64_t b0 = blk * rowsPerCore;
    if (b0 >= rows) {
        return;
    }
    int64_t n = rowsPerCore;
    if (b0 + n > rows) {
        n = rows - b0;
    }

    const uint16_t rowLenF = (uint16_t)(H * sizeof(float) / 32);
    const uint16_t srcGap = (uint16_t)((S * H - H) * sizeof(float) / 32);
    // Assumes H <= BWD_SHIFT_CHUNK so a whole row fits one transfer (H = 512 here).
    int64_t rowsPerTile = BWD_SHIFT_CHUNK / H;
    if (rowsPerTile < 1) {
        rowsPerTile = 1;
    }

    // t = 0 block: n * H contiguous floats.
    for (int64_t off = 0; off < n * H; off += BWD_SHIFT_CHUNK) {
        int64_t c = n * H - off;
        if (c > BWD_SHIFT_CHUNK) {
            c = BWD_SHIFT_CHUNK;
        }
        auto o = qOut.AllocTensor<half>();
        Duplicate(o, (half)0.0f, (int32_t)c);
        qOut.EnQue(o);
        auto od = qOut.DeQue<half>();
        DataCopy(hpG[b0 * H + off], od, (uint32_t)c);
        qOut.FreeTensor(od);
    }

    for (int64_t t = 1; t < S; ++t) {
        const int64_t dstBase = (t * rows + b0) * H;
        for (int64_t r = 0; r < n; r += rowsPerTile) {
            int64_t rc = n - r;
            if (rc > rowsPerTile) {
                rc = rowsPerTile;
            }
            const int64_t cnt = rc * H;
            DataCopyParams dp;
            dp.blockCount = (uint16_t)rc;
            dp.blockLen = rowLenF;
            dp.srcStride = srcGap;
            dp.dstStride = 0;

            auto in = qIn.AllocTensor<float>();
            DataCopy(in, outG[((b0 + r) * S + (t - 1)) * H], dp);
            qIn.EnQue(in);
            auto ind = qIn.DeQue<float>();
            auto o = qOut.AllocTensor<half>();
            Cast(o, ind, RoundMode::CAST_RINT, (int32_t)cnt);
            qOut.EnQue(o);
            qIn.FreeTensor(ind);
            auto od = qOut.DeQue<half>();
            DataCopy(hpG[dstBase + r * H], od, (uint32_t)cnt);
            qOut.FreeTensor(od);
        }
    }
}

// ---------------------------------------------------------------------------
// 2) reverse cell for one timestep
// ---------------------------------------------------------------------------

// Rows of the hidden state per UB tile. Sized so the 25 float tiles this kernel
// keeps live (10 inputs, 5 outputs, 9 scratch, 1 constant) fit comfortably in UB.
// 4 rows => 25*4*512*4 = 200 KB of the 248 KB UB, still with headroom for the
// runtime reserves. The cell is MTE2-limited: its 17 per-tile DataCopies are
// small (<= 8 KB) and strided, so it runs at ~1.36 TB/s against a ~1.57 TB/s
// bus. Doubling the tile halves the number of DMA instructions and doubles the
// burst size without changing the byte count.
constexpr int64_t BWD_CELL_ROWS = 4;
constexpr int64_t BWD_CELL_TEMPS = 8;
// Buffers per queue. Kept at 1: the cell issues 15 queues, and a depth of 2
// (30 buffers) exhausts the AIV hardware event pool -> the kernel wedges and
// the host watchdog fires "Vector core execution timed out" after ~10 min.
// Depth 1 costs the mte2/vec/mte3 pipelining (aiv_time ~= sum of the three
// pipes) but is the only schedule that runs; recover the overlap by reducing
// the number of live queues instead, not by deepening them.
constexpr int64_t BWD_CELL_QDEPTH = 1;

__aicore__ inline void BwdCellBody(GM_ADDR gi_, GM_ADDR gh_, GM_ADDR hp_, GM_ADDR dy_,
                                   GM_ADDR dha_, GM_ADDR dhb_, GM_ADDR dgx_, GM_ADDR dgh_,
                                   GM_ADDR tilingGM, int64_t t)
{
    GruBwdTilingData td;
    CopyBwdTilingToLocal(&td, tilingGM);
    const int64_t B = td.chunkRows;
    const int64_t S = td.S;
    const int64_t H = td.H;

    const int64_t blkNum = (int64_t)GetBlockNum();
    const int64_t blk = (int64_t)GetBlockIdx();
    int64_t rowsPerCore = (B + blkNum - 1) / blkNum;
    const int64_t b0 = blk * rowsPerCore;
    if (b0 >= B) {
        return;
    }
    int64_t rows = rowsPerCore;
    if (b0 + rows > B) {
        rows = B - b0;
    }

    constexpr int64_t R = BWD_CELL_ROWS;
    constexpr int64_t NT = BWD_CELL_TEMPS;
    const uint16_t rowLenF = (uint16_t)(H * sizeof(float) / 32);
    const uint16_t rowLenF16 = (uint16_t)(H * sizeof(half) / 32);
    const uint16_t stride3H = (uint16_t)((S * 3 * H - H) * sizeof(float) / 32);
    const uint16_t strideSH = (uint16_t)((S * H - H) * sizeof(float) / 32);
    const uint16_t strideT = (uint16_t)((3 * H - H) * sizeof(float) / 32);
    // fp16 strides for the dgx / dgh gradient stores (wK: they are GEMM A
    // operands now, so they are kept at the 11-bit precision the cube rounds to).
    const uint16_t stride3Hf16 = (uint16_t)((S * 3 * H - H) * sizeof(half) / 32);
    const uint16_t strideTf16 = (uint16_t)((3 * H - H) * sizeof(half) / 32);
    // hp and dgh are step-major [S, rows, *]; gh is step-major only when the
    // backward recomputed it from hp (gate cache off). See gru_tiling_data.h.
    const bool ghStepMajor = (td.ghStepMajor != 0);

    const int64_t tot3H = B * S * 3 * H;
    const int64_t totSH = B * S * H;
    const int64_t totH = B * H;

    TPipe pipe;
    TQue<QuePosition::VECIN, BWD_CELL_QDEPTH> qGiR, qGiZ, qGiN, qGhR, qGhZ, qGhN, qHp, qDy, qDha, qDhb;
    TQue<QuePosition::VECOUT, BWD_CELL_QDEPTH> qDur, qDuz, qDun, qDghN, qDhaOut;
    // The six gate pre-activations are staged at fp16 (the forward GH/GI GEMMs
    // write them at the cube's operand precision) and widened to fp32 into the
    // tmpBuf scratch right after the DMA lands. They are the largest AIV MTE2
    // stream in the cell, so halving their width is the single biggest win; a
    // proper half->half Cast does not exist, hence the explicit Widen.
    pipe.InitBuffer(qGiR, BWD_CELL_QDEPTH, R * H * sizeof(half));
    pipe.InitBuffer(qGiZ, BWD_CELL_QDEPTH, R * H * sizeof(half));
    pipe.InitBuffer(qGiN, BWD_CELL_QDEPTH, R * H * sizeof(half));
    pipe.InitBuffer(qGhR, BWD_CELL_QDEPTH, R * H * sizeof(half));
    pipe.InitBuffer(qGhZ, BWD_CELL_QDEPTH, R * H * sizeof(half));
    pipe.InitBuffer(qGhN, BWD_CELL_QDEPTH, R * H * sizeof(half));
    // hp is fp16 (the forward stages it at the cube's operand precision).
    pipe.InitBuffer(qHp, BWD_CELL_QDEPTH, R * H * sizeof(half));
    pipe.InitBuffer(qDy, BWD_CELL_QDEPTH, R * H * sizeof(float));
    // dha / dhb are the cell's hidden-gradient working set (read every step,
    // and for dha re-written every step). They are stored at fp16 -- the same
    // 11-bit mantissa the cube already rounds every operand to -- so the cell's
    // single largest remaining fp32 stream halves with the fp32 arithmetic
    // untouched (the tiles are widened straight after the DMA lands).
    pipe.InitBuffer(qDha, BWD_CELL_QDEPTH, R * H * sizeof(half));
    pipe.InitBuffer(qDhb, BWD_CELL_QDEPTH, R * H * sizeof(half));
    // dgx / dgh are the A operands of the WI / WH / DH GEMMs -> fp16.
    pipe.InitBuffer(qDur, BWD_CELL_QDEPTH, R * H * sizeof(half));
    pipe.InitBuffer(qDuz, BWD_CELL_QDEPTH, R * H * sizeof(half));
    pipe.InitBuffer(qDun, BWD_CELL_QDEPTH, R * H * sizeof(half));
    pipe.InitBuffer(qDghN, BWD_CELL_QDEPTH, R * H * sizeof(half));
    pipe.InitBuffer(qDhaOut, BWD_CELL_QDEPTH, R * H * sizeof(half));
    TBuf<TPosition::VECCALC> tmpBuf;
    pipe.InitBuffer(tmpBuf, (NT + 10) * R * H * sizeof(float));

    LocalTensor<float> s0 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 0);
    LocalTensor<float> s1 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), R * H * sizeof(float));
    LocalTensor<float> s2 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 2 * R * H * sizeof(float));
    LocalTensor<float> s3 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 3 * R * H * sizeof(float));
    LocalTensor<float> s4 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 4 * R * H * sizeof(float));
    LocalTensor<float> s5 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 5 * R * H * sizeof(float));
    LocalTensor<float> s6 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 6 * R * H * sizeof(float));
    LocalTensor<float> s7 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 7 * R * H * sizeof(float));
    LocalTensor<float> ones = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 8 * R * H * sizeof(float));
    // fp32 view of the fp16 hp operand.
    LocalTensor<float> hpF32 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 9 * R * H * sizeof(float));
    // fp32 scratch for the widened gate slabs (slots NT+2 .. NT+7).
    LocalTensor<float> giRf = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), (NT + 2) * R * H * sizeof(float));
    LocalTensor<float> giZf = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), (NT + 3) * R * H * sizeof(float));
    LocalTensor<float> giNf = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), (NT + 4) * R * H * sizeof(float));
    LocalTensor<float> ghRf = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), (NT + 5) * R * H * sizeof(float));
    LocalTensor<float> ghZf = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), (NT + 6) * R * H * sizeof(float));
    LocalTensor<float> ghNf = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), (NT + 7) * R * H * sizeof(float));
    // fp32 scratch for the widened dha / dhb gradient tiles (slots NT+8, NT+9).
    LocalTensor<float> dhaF32 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), (NT + 8) * R * H * sizeof(float));
    LocalTensor<float> dhbF32 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), (NT + 9) * R * H * sizeof(float));

    GlobalTensor<float> dyG;
    GlobalTensor<half> dhaG, dhbG;
    GlobalTensor<half> giG, ghG, hpG, dgxG, dghG;
    giG.SetGlobalBuffer((__gm__ half *)gi_, tot3H);
    ghG.SetGlobalBuffer((__gm__ half *)gh_, tot3H);
    hpG.SetGlobalBuffer((__gm__ half *)hp_, totSH);
    dyG.SetGlobalBuffer((__gm__ float *)dy_, totSH);
    dhaG.SetGlobalBuffer((__gm__ half *)dha_, totH);
    dhbG.SetGlobalBuffer((__gm__ half *)dhb_, totH);
    dgxG.SetGlobalBuffer((__gm__ half *)dgx_, tot3H);
    dghG.SetGlobalBuffer((__gm__ half *)dgh_, B * 3 * H);

    for (int64_t r0 = 0; r0 < rows; r0 += R) {
        int64_t r = rows - r0;
        if (r > R) {
            r = R;
        }
        const int64_t rowBase = b0 + r0;
        const uint32_t cnt = (uint32_t)(r * H);
        const uint16_t rr = (uint16_t)r;

        // gi/dy stay row-major [rows*S, *] (x and the incoming gradient are);
        // hp is step-major so this step's [rows, H] slab is contiguous, and
        // dgh's step slab (tgBase) is contiguous inside the host's view.
        const int64_t giBase = (rowBase * S + t) * 3 * H;
        const int64_t shBase = (rowBase * S + t) * H;
        const int64_t hpBase = (t * B + rowBase) * H;
        const int64_t ghBase = ghStepMajor ? ((t * B + rowBase) * 3 * H) : giBase;
        const int64_t rowBaseH = rowBase * H;
        const int64_t tgBase = rowBase * 3 * H;

        DataCopyParams dp3H;  // fp16 strided read of the gi gate slab
        dp3H.blockCount = rr;
        dp3H.blockLen = rowLenF16;
        dp3H.srcStride = stride3Hf16;
        dp3H.dstStride = 0;

        DataCopyParams dpSH;
        dpSH.blockCount = rr;
        dpSH.blockLen = rowLenF;
        dpSH.srcStride = strideSH;
        dpSH.dstStride = 0;

        DataCopyParams dpRowH;  // fp16 contiguous row tile (hp / dha / dhb)
        dpRowH.blockCount = rr;
        dpRowH.blockLen = rowLenF16;
        dpRowH.srcStride = 0;
        dpRowH.dstStride = 0;

        // gh is step-major with a [rows, 3H] slab, so gate g of row i sits at
        // +i*3H: the inter-block gap (blockLen + srcStride) is 3H, not S*3H.
        // strideT == (3H - H) in 32B units, which is what yields that 3H pitch.
        DataCopyParams dpGh;  // fp16 read of the gh gate slab
        dpGh.blockCount = rr;
        dpGh.blockLen = rowLenF16;
        dpGh.srcStride = ghStepMajor ? strideTf16 : stride3Hf16;
        dpGh.dstStride = 0;

        // ---- CopyIn -------------------------------------------------------
        auto giR = qGiR.AllocTensor<half>();
        DataCopy(giR, giG[giBase], dp3H);
        qGiR.EnQue(giR);
        auto giZ = qGiZ.AllocTensor<half>();
        DataCopy(giZ, giG[giBase + H], dp3H);
        qGiZ.EnQue(giZ);
        auto giN = qGiN.AllocTensor<half>();
        DataCopy(giN, giG[giBase + 2 * H], dp3H);
        qGiN.EnQue(giN);

        auto ghR = qGhR.AllocTensor<half>();
        DataCopy(ghR, ghG[ghBase], dpGh);
        qGhR.EnQue(ghR);
        auto ghZ = qGhZ.AllocTensor<half>();
        DataCopy(ghZ, ghG[ghBase + H], dpGh);
        qGhZ.EnQue(ghZ);
        auto ghN = qGhN.AllocTensor<half>();
        DataCopy(ghN, ghG[ghBase + 2 * H], dpGh);
        qGhN.EnQue(ghN);

        auto hp = qHp.AllocTensor<half>();
        DataCopy(hp, hpG[hpBase], dpRowH);
        qHp.EnQue(hp);

        auto dy = qDy.AllocTensor<float>();
        DataCopy(dy, dyG[shBase], dpSH);
        qDy.EnQue(dy);

        auto dha = qDha.AllocTensor<half>();
        DataCopy(dha, dhaG[rowBaseH], dpRowH);
        qDha.EnQue(dha);

        auto dhb = qDhb.AllocTensor<half>();
        DataCopy(dhb, dhbG[rowBaseH], dpRowH);
        qDhb.EnQue(dhb);

        // widen the six fp16 gate slabs into their fp32 scratch slots
        auto giRD = qGiR.DeQue<half>();
        Cast(giRf, giRD, RoundMode::CAST_NONE, cnt);
        qGiR.FreeTensor(giRD);
        auto giZD = qGiZ.DeQue<half>();
        Cast(giZf, giZD, RoundMode::CAST_NONE, cnt);
        qGiZ.FreeTensor(giZD);
        auto giND = qGiN.DeQue<half>();
        Cast(giNf, giND, RoundMode::CAST_NONE, cnt);
        qGiN.FreeTensor(giND);
        auto ghRD = qGhR.DeQue<half>();
        Cast(ghRf, ghRD, RoundMode::CAST_NONE, cnt);
        qGhR.FreeTensor(ghRD);
        auto ghZD = qGhZ.DeQue<half>();
        Cast(ghZf, ghZD, RoundMode::CAST_NONE, cnt);
        qGhZ.FreeTensor(ghZD);
        auto ghND = qGhN.DeQue<half>();
        Cast(ghNf, ghND, RoundMode::CAST_NONE, cnt);
        qGhN.FreeTensor(ghND);

        auto hpL = qHp.DeQue<half>();
        Cast(hpF32, hpL, RoundMode::CAST_NONE, cnt);
        auto dyL = qDy.DeQue<float>();
        auto dhaH = qDha.DeQue<half>();
        Cast(dhaF32, dhaH, RoundMode::CAST_NONE, cnt);
        auto dhbH = qDhb.DeQue<half>();
        Cast(dhbF32, dhbH, RoundMode::CAST_NONE, cnt);

        auto durL = qDur.AllocTensor<half>();
        auto duzL = qDuz.AllocTensor<half>();
        auto dunL = qDun.AllocTensor<half>();
        auto dghNL = qDghN.AllocTensor<half>();
        auto dhaOutL = qDhaOut.AllocTensor<half>();

        // ---- Compute ------------------------------------------------------
        Duplicate(ones, 1.0f, cnt);

        // s0 = dht = dy + dha + dhb   (dha / dhb widened from fp16)
        Add(s0, dyL, dhaF32, cnt);
        Add(s0, s0, dhbF32, cnt);

        // s6 = r = sigmoid(giR + ghR)
        Add(s2, giRf, ghRf, cnt);
        Muls(s2, s2, -1.0f, cnt);
        Exp(s2, s2, cnt);
        Adds(s2, s2, 1.0f, cnt);
        Div(s6, ones, s2, cnt);

        // s7 = z = sigmoid(giZ + ghZ)
        Add(s3, giZf, ghZf, cnt);
        Muls(s3, s3, -1.0f, cnt);
        Exp(s3, s3, cnt);
        Adds(s3, s3, 1.0f, cnt);
        Div(s7, ones, s3, cnt);

        // s1 = n = tanh(giN + r * ghN)
        Mul(s4, s6, ghNf, cnt);
        Add(s4, giNf, s4, cnt);
        Muls(s5, s4, 2.0f, cnt);
        Exp(s5, s5, cnt);
        Adds(s5, s5, 1.0f, cnt);
        Div(s1, ones, s5, cnt);
        Muls(s1, s1, 2.0f, cnt);
        Muls(s1, s1, -1.0f, cnt);
        Adds(s1, s1, 1.0f, cnt);

        // ---- Gradients ----------------------------------------------------
        // s2 = dn = dht * (1 - z)
        Muls(s2, s7, -1.0f, cnt);
        Adds(s2, s2, 1.0f, cnt);
        Mul(s2, s2, s0, cnt);

        // s3 = dz = dht * (hp - n)
        Sub(s3, hpF32, s1, cnt);
        Mul(s3, s3, s0, cnt);

        // s4 = dun = dn * (1 - n^2)
        Mul(s4, s1, s1, cnt);
        Muls(s4, s4, -1.0f, cnt);
        Adds(s4, s4, 1.0f, cnt);
        Mul(s4, s4, s2, cnt);

        // duz = dz * z * (1 - z)
        Muls(s5, s7, -1.0f, cnt);
        Adds(s5, s5, 1.0f, cnt);
        Mul(s5, s5, s7, cnt);
        Mul(s5, s5, s3, cnt);
        Cast(duzL, s5, RoundMode::CAST_RINT, cnt);

        // dghN = dun * r
        Mul(s5, s4, s6, cnt);
        Cast(dghNL, s5, RoundMode::CAST_RINT, cnt);

        // dur = dun * ghN * r * (1 - r)
        Mul(s5, s4, ghNf, cnt);
        Muls(s2, s6, -1.0f, cnt);
        Adds(s2, s2, 1.0f, cnt);
        Mul(s2, s2, s6, cnt);
        Mul(s2, s2, s5, cnt);
        Cast(durL, s2, RoundMode::CAST_RINT, cnt);

        // dun
        Cast(dunL, s4, RoundMode::CAST_RINT, cnt);

        // dha' = dht * z  (fp32 product, narrowed at the store)
        Mul(s0, s0, s7, cnt);
        Cast(dhaOutL, s0, RoundMode::CAST_RINT, cnt);

        qHp.FreeTensor(hpL);
        qDy.FreeTensor(dyL);
        qDha.FreeTensor(dhaH);
        qDhb.FreeTensor(dhbH);

        qDur.EnQue(durL);
        qDuz.EnQue(duzL);
        qDun.EnQue(dunL);
        qDghN.EnQue(dghNL);
        qDhaOut.EnQue(dhaOutL);

        // ---- CopyOut ------------------------------------------------------
        DataCopyParams dpX16;  // strided into dgx [rows*S, 3H], fp16
        dpX16.blockCount = rr;
        dpX16.blockLen = rowLenF16;
        dpX16.srcStride = 0;
        dpX16.dstStride = stride3Hf16;

        DataCopyParams dpG16;  // contiguous [rows, 3H] step slab of dgh, fp16
        dpG16.blockCount = rr;
        dpG16.blockLen = rowLenF16;
        dpG16.srcStride = 0;
        dpG16.dstStride = strideTf16;

        // dgx = (dur, duz, dun); dgh = (dur, duz, dghN). Each shared tile feeds
        // two destinations before it is freed: the [rows*S, 3H] buffer that the
        // parameter-gradient GEMMs and the bias reduction consume, and (for dgh)
        // this step's [rows, 3H] block of the [S, rows, 3H] gradient buffer --
        // which is exactly the A operand the next step's recurrent GEMM reads,
        // so no separate per-step scratch copy is materialised.
        auto durD = qDur.DeQue<half>();
        DataCopy(dgxG[giBase], durD, dpX16);
        DataCopy(dghG[tgBase], durD, dpG16);
        qDur.FreeTensor(durD);

        auto duzD = qDuz.DeQue<half>();
        DataCopy(dgxG[giBase + H], duzD, dpX16);
        DataCopy(dghG[tgBase + H], duzD, dpG16);
        qDuz.FreeTensor(duzD);

        auto dunD = qDun.DeQue<half>();
        DataCopy(dgxG[giBase + 2 * H], dunD, dpX16);
        qDun.FreeTensor(dunD);

        auto dghND = qDghN.DeQue<half>();
        DataCopy(dghG[tgBase + 2 * H], dghND, dpG16);
        qDghN.FreeTensor(dghND);

        // dha is contiguous [rows, H], stored at fp16
        auto dhaOutD = qDhaOut.DeQue<half>();
        DataCopy(dhaG[rowBaseH], dhaOutD, dpRowH);
        qDhaOut.FreeTensor(dhaOutD);
    }
}

// ---------------------------------------------------------------------------
// 3) bias gradients: two-pass column sum of dGx / dGh
// ---------------------------------------------------------------------------
constexpr int64_t BWD_RED_ROWS = 8;    // rows per UB tile in pass 1
constexpr int64_t BWD_RED_COLS = 128;  // column block width in pass 2

__aicore__ inline void BwdColSumPartialBody(GM_ADDR dgx_, GM_ADDR dgh_, GM_ADDR ws_,
                                            GM_ADDR tilingGM)
{
    GruBwdTilingData td;
    CopyBwdTilingToLocal(&td, tilingGM);
    const int64_t H = td.H;
    const int64_t G = 3 * td.H;
    const int64_t n = td.chunkRows * td.S;
    const int64_t blkNum = (int64_t)GetBlockNum();
    const int64_t blk = (int64_t)GetBlockIdx();

    if (n <= 0) {
        return;
    }
    // Contiguous row range per core; rounded up to whole tiles so every tile but
    // the last is full (a partial tile is still a legal DataCopy).
    int64_t perCore = (n + blkNum - 1) / blkNum;
    perCore = ((perCore + BWD_RED_ROWS - 1) / BWD_RED_ROWS) * BWD_RED_ROWS;
    const int64_t r0 = blk * perCore;
    // A core with no rows must NOT bail out: this is a reduction, and every
    // block has to publish its own zero-filled partial. Returning here would
    // leave an unwritten slot in the workspace, which the second pass would then
    // add into the bias gradient as garbage.
    int64_t rn = perCore;
    if (r0 >= n) {
        rn = 0;
    } else if (r0 + rn > n) {
        rn = n - r0;
    }

    constexpr int64_t RR = BWD_RED_ROWS;
    TPipe pipe;
    TQue<QuePosition::VECIN, 2> qLoad;
    TQue<QuePosition::VECOUT, 1> qOut;
    pipe.InitBuffer(qLoad, 2, RR * G * sizeof(half));
    pipe.InitBuffer(qOut, 1, G * sizeof(float));
    TBuf<TPosition::VECCALC> accBuf;
    pipe.InitBuffer(accBuf, (G + H) * sizeof(float));
    TBuf<TPosition::VECCALC> castBuf;
    pipe.InitBuffer(castBuf, RR * G * sizeof(float));

    GlobalTensor<half> dgxG;
    dgxG.SetGlobalBuffer((__gm__ half *)dgx_, td.chunkRows * td.S * G);
    // dgh is step-major [S, rows, 3H] = (dur, duz, dghN). Only the n block
    // (columns 2H..3H) is read here: the r/z blocks carry the very same values
    // the cell stored into dgx (both are DataCopy'd from the same UB tile), so
    // reading them a second time only to re-sum the identical addends is pure
    // redundant traffic. See the store step below.
    GlobalTensor<half> dghG;
    dghG.SetGlobalBuffer((__gm__ half *)dgh_, td.chunkRows * td.S * G);
    GlobalTensor<float> wsG;
    wsG.SetGlobalBuffer((__gm__ float *)ws_, 2 * blkNum * G);

    auto acc = accBuf.GetWithOffset<float>((uint32_t)G, 0);
    auto accN = accBuf.GetWithOffset<float>((uint32_t)H, (uint32_t)(G * sizeof(float)));
    auto ldF = castBuf.Get<float>();
    Duplicate(acc, 0.0f, (int32_t)G);
    Duplicate(accN, 0.0f, (int32_t)H);

    // Pass 1: (dur, duz, dun) straight out of dgx, contiguous rows. dgx is fp16
    // (the cell stores it at the cube's operand precision), so widen before the
    // fp32 accumulation.
    // The next tile's load is issued before the current tile is consumed, so the
    // MTE2 of tile i+1 overlaps the cast + row Adds of tile i. A depth-1 queue
    // serialised copy latency with compute and left MTE2 barely half busy.
    if (rn > 0) {
        {
            int64_t cp = rn;
            if (cp > RR) {
                cp = RR;
            }
            auto lp = qLoad.AllocTensor<half>();
            DataCopy(lp, dgxG[(r0 + 0) * G], (uint32_t)(cp * G));
            qLoad.EnQue(lp);
        }
        for (int64_t rr = 0; rr < rn; rr += RR) {
            int64_t next = rr + RR;
            if (next < rn) {
                int64_t cn = rn - next;
                if (cn > RR) {
                    cn = RR;
                }
                auto ln = qLoad.AllocTensor<half>();
                DataCopy(ln, dgxG[(r0 + next) * G], (uint32_t)(cn * G));
                qLoad.EnQue(ln);
            }
            int64_t c = rn - rr;
            if (c > RR) {
                c = RR;
            }
            auto ldd = qLoad.DeQue<half>();
            Cast(ldF, ldd, RoundMode::CAST_NONE, (int32_t)(c * G));
            for (int64_t i = 0; i < c; ++i) {
                Add(acc, acc, ldF[i * G], (int32_t)G);
            }
            qLoad.FreeTensor(ldd);
        }
    }

    // Pass 2: dghN = dgh[:, 2H:3H] -- one H-wide strided block per row.
    DataCopyParams dpN;
    dpN.blockCount = (uint16_t)RR;
    dpN.blockLen = (uint16_t)(H * sizeof(half) / 32);
    dpN.srcStride = (uint16_t)((G - H) * sizeof(half) / 32);
    dpN.dstStride = 0;
    if (rn > 0) {
        {
            int64_t cp = rn;
            if (cp > RR) {
                cp = RR;
            }
            DataCopyParams dpp = dpN;
            dpp.blockCount = (uint16_t)cp;
            auto lp = qLoad.AllocTensor<half>();
            DataCopy(lp, dghG[(r0 + 0) * G + 2 * H], dpp);
            qLoad.EnQue(lp);
        }
        for (int64_t rr = 0; rr < rn; rr += RR) {
            int64_t next = rr + RR;
            if (next < rn) {
                int64_t cn = rn - next;
                if (cn > RR) {
                    cn = RR;
                }
                DataCopyParams dpn2 = dpN;
                dpn2.blockCount = (uint16_t)cn;
                auto ln = qLoad.AllocTensor<half>();
                DataCopy(ln, dghG[(r0 + next) * G + 2 * H], dpn2);
                qLoad.EnQue(ln);
            }
            int64_t c = rn - rr;
            if (c > RR) {
                c = RR;
            }
            auto ldd = qLoad.DeQue<half>();
            Cast(ldF, ldd, RoundMode::CAST_NONE, (int32_t)(c * H));
            for (int64_t i = 0; i < c; ++i) {
                Add(accN, accN, ldF[i * H], (int32_t)H);
            }
            qLoad.FreeTensor(ldd);
        }
    }

    // Route the stores through a VECOUT queue so the V -> MTE3 edge is
    // queue-managed (the accumulators live in a plain VECCALC buffer).
    //   ws[0] = (colsum dur, colsum duz, colsum dun ) -> db_ih
    //   ws[1] = (colsum dur, colsum duz, colsum dghN) -> db_hh
    // The first 2H lanes of the two partials are the same numbers verbatim.
    {
        auto o = qOut.AllocTensor<float>();
        Adds(o, acc, 0.0f, (int32_t)G);
        qOut.EnQue(o);
        auto od = qOut.DeQue<float>();
        DataCopy(wsG[(0 * blkNum + blk) * G], od, (uint32_t)G);
        qOut.FreeTensor(od);
    }
    {
        auto o = qOut.AllocTensor<float>();
        Adds(o, acc, 0.0f, (int32_t)G);
        Adds(o[2 * H], accN, 0.0f, (int32_t)H);
        qOut.EnQue(o);
        auto od = qOut.DeQue<float>();
        DataCopy(wsG[(1 * blkNum + blk) * G], od, (uint32_t)G);
        qOut.FreeTensor(od);
    }
}

__aicore__ inline void BwdColSumFinalBody(GM_ADDR ws_, GM_ADDR dbih_, GM_ADDR dbhh_,
                                          GM_ADDR tilingGM)
{
    GruBwdTilingData td;
    CopyBwdTilingToLocal(&td, tilingGM);
    const int64_t G = 3 * td.H;
    const int64_t nPart = td.aivNum;
    const int64_t blkNum = (int64_t)GetBlockNum();
    const int64_t blk = (int64_t)GetBlockIdx();
    const int64_t nColBlk = (G + BWD_RED_COLS - 1) / BWD_RED_COLS;
    const int64_t total = 2 * nColBlk;

    const int64_t accSize = (G > BWD_RED_COLS) ? G : BWD_RED_COLS;
    TPipe pipe;
    TQue<QuePosition::VECIN, 1> qLoad;
    TQue<QuePosition::VECOUT, 1> qOut;
    pipe.InitBuffer(qLoad, 1, BWD_RED_COLS * sizeof(float));
    pipe.InitBuffer(qOut, 1, BWD_RED_COLS * sizeof(float));
    TBuf<TPosition::VECCALC> accBuf;
    pipe.InitBuffer(accBuf, accSize * sizeof(float));

    GlobalTensor<float> wsG;
    wsG.SetGlobalBuffer((__gm__ float *)ws_, 2 * nPart * G);
    GlobalTensor<float> dbG[2];
    dbG[0].SetGlobalBuffer((__gm__ float *)dbih_, G);
    dbG[1].SetGlobalBuffer((__gm__ float *)dbhh_, G);

    for (int64_t w = blk; w < total; w += blkNum) {
        const int64_t which = w / nColBlk;
        const int64_t c0 = (w % nColBlk) * BWD_RED_COLS;
        int64_t cw = G - c0;
        if (cw > BWD_RED_COLS) {
            cw = BWD_RED_COLS;
        }
        const int64_t pBase = (which ? nPart : 0) * G + c0;
        auto acc = accBuf.Get<float>();
        for (int64_t p = 0; p < nPart; ++p) {
            auto ld = qLoad.AllocTensor<float>();
            DataCopy(ld, wsG[pBase + p * G], (uint32_t)cw);
            qLoad.EnQue(ld);
            auto ldd = qLoad.DeQue<float>();
            if (p == 0) {
                Adds(acc, ldd, 0.0f, (int32_t)cw);
            } else {
                Add(acc, acc, ldd, (int32_t)cw);
            }
            qLoad.FreeTensor(ldd);
        }
        auto ld2 = qLoad.AllocTensor<float>();
        DataCopy(ld2, dbG[which][c0], (uint32_t)cw);
        qLoad.EnQue(ld2);
        auto ld2d = qLoad.DeQue<float>();
        Add(acc, acc, ld2d, (int32_t)cw);
        qLoad.FreeTensor(ld2d);

        auto o = qOut.AllocTensor<float>();
        Adds(o, acc, 0.0f, (int32_t)cw);
        qOut.EnQue(o);
        auto od = qOut.DeQue<float>();
        DataCopy(dbG[which][c0], od, (uint32_t)cw);
        qOut.FreeTensor(od);
    }
}

// ---------------------------------------------------------------------------
// 4) dst += src
// ---------------------------------------------------------------------------
constexpr int64_t BWD_ADD_CHUNK = 8192;

__aicore__ inline void BwdAddBody(GM_ADDR dst_, GM_ADDR src_, int64_t count)
{
    const int64_t blkNum = (int64_t)GetBlockNum();
    const int64_t blk = (int64_t)GetBlockIdx();
    if (count <= 0) {
        return;
    }
    int64_t perCore = (count + blkNum - 1) / blkNum;
    perCore = ((perCore + 7) / 8) * 8;
    const int64_t start = blk * perCore;
    if (start >= count) {
        return;
    }
    int64_t cntAll = perCore;
    if (start + cntAll > count) {
        cntAll = count - start;
    }

    TPipe pipe;
    TQue<QuePosition::VECIN, 2> qIn;
    TQue<QuePosition::VECOUT, 1> qOut;
    pipe.InitBuffer(qIn, 2, BWD_ADD_CHUNK * sizeof(float));
    pipe.InitBuffer(qOut, 1, BWD_ADD_CHUNK * sizeof(float));

    GlobalTensor<float> dG;
    dG.SetGlobalBuffer((__gm__ float *)dst_ + start);
    GlobalTensor<float> sG;
    sG.SetGlobalBuffer((__gm__ float *)src_ + start);

    for (int64_t off = 0; off < cntAll; off += BWD_ADD_CHUNK) {
        int64_t c = cntAll - off;
        if (c > BWD_ADD_CHUNK) {
            c = BWD_ADD_CHUNK;
        }
        auto a = qIn.AllocTensor<float>();
        DataCopy(a, dG[off], (uint32_t)c);
        qIn.EnQue(a);
        auto b = qIn.AllocTensor<float>();
        DataCopy(b, sG[off], (uint32_t)c);
        qIn.EnQue(b);
        auto ad = qIn.DeQue<float>();
        auto bd = qIn.DeQue<float>();
        auto o = qOut.AllocTensor<float>();
        Add(o, ad, bd, (int32_t)c);
        qOut.EnQue(o);
        qIn.FreeTensor(ad);
        qIn.FreeTensor(bd);
        auto od = qOut.DeQue<float>();
        DataCopy(dG[off], od, (uint32_t)c);
        qOut.FreeTensor(od);
    }
}

}  // namespace

extern "C" __global__ __aicore__ void gru_bwd_hprev(GM_ADDR out, GM_ADDR hp, GM_ADDR tilingGM)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    if ASCEND_IS_AIV {
        BwdShiftBody(out, hp, tilingGM);
    }
}

extern "C" __global__ __aicore__ void gru_bwd_cell(GM_ADDR gi, GM_ADDR gh, GM_ADDR hp, GM_ADDR dy,
                                                   GM_ADDR dha, GM_ADDR dhb, GM_ADDR dgx,
                                                   GM_ADDR dgh, GM_ADDR tilingGM, int64_t t)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    if ASCEND_IS_AIV {
        BwdCellBody(gi, gh, hp, dy, dha, dhb, dgx, dgh, tilingGM, t);
    }
}

extern "C" __global__ __aicore__ void gru_bwd_colsum_partial(GM_ADDR dgx, GM_ADDR dgh, GM_ADDR ws,
                                                             GM_ADDR tilingGM)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    if ASCEND_IS_AIV {
        BwdColSumPartialBody(dgx, dgh, ws, tilingGM);
    }
}

extern "C" __global__ __aicore__ void gru_bwd_colsum_final(GM_ADDR ws, GM_ADDR dbih, GM_ADDR dbhh,
                                                           GM_ADDR tilingGM)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    if ASCEND_IS_AIV {
        BwdColSumFinalBody(ws, dbih, dbhh, tilingGM);
    }
}

extern "C" __global__ __aicore__ void gru_bwd_add(GM_ADDR dst, GM_ADDR src, GM_ADDR tilingGM,
                                                  int64_t count)
{
    (void)tilingGM;
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    if ASCEND_IS_AIV {
        BwdAddBody(dst, src, count);
    }
}
