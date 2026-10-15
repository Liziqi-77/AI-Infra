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
    pipe.InitBuffer(qOut, 1, BWD_SHIFT_CHUNK * sizeof(float));

    GlobalTensor<float> outG;
    outG.SetGlobalBuffer((__gm__ float *)out_);
    GlobalTensor<float> hpG;
    hpG.SetGlobalBuffer((__gm__ float *)hp_);

    const int64_t inner = (S - 1) * H;  // elements shifted per batch row

    for (int64_t b = blk; b < rows; b += blkNum) {
        const int64_t base = b * S * H;

        // hp[b, 0, :] = 0
        for (int64_t off = 0; off < H; off += BWD_SHIFT_CHUNK) {
            int64_t c = H - off;
            if (c > BWD_SHIFT_CHUNK) {
                c = BWD_SHIFT_CHUNK;
            }
            auto o = qOut.AllocTensor<float>();
            Duplicate(o, 0.0f, (int32_t)c);
            qOut.EnQue(o);
            auto od = qOut.DeQue<float>();
            DataCopy(hpG[base + off], od, (uint32_t)c);
            qOut.FreeTensor(od);
        }

        // hp[b, 1:, :] = out[b, :S-1, :]
        for (int64_t off = 0; off < inner; off += BWD_SHIFT_CHUNK) {
            int64_t c = inner - off;
            if (c > BWD_SHIFT_CHUNK) {
                c = BWD_SHIFT_CHUNK;
            }
            auto in = qIn.AllocTensor<float>();
            DataCopy(in, outG[base + off], (uint32_t)c);
            qIn.EnQue(in);
            auto ind = qIn.DeQue<float>();
            auto o = qOut.AllocTensor<float>();
            Adds(o, ind, 0.0f, (int32_t)c);
            qOut.EnQue(o);
            qIn.FreeTensor(ind);
            auto od = qOut.DeQue<float>();
            DataCopy(hpG[base + H + off], od, (uint32_t)c);
            qOut.FreeTensor(od);
        }
    }
}

// ---------------------------------------------------------------------------
// 2) reverse cell for one timestep
// ---------------------------------------------------------------------------

// Rows of the hidden state per UB tile. Sized so the 25 float tiles this kernel
// keeps live (10 inputs, 5 outputs, 9 scratch, 1 constant) fit comfortably in UB.
constexpr int64_t BWD_CELL_ROWS = 2;
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
                                   GM_ADDR dghC_, GM_ADDR tilingGM, int64_t t)
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
    const uint16_t stride3H = (uint16_t)((S * 3 * H - H) * sizeof(float) / 32);
    const uint16_t strideSH = (uint16_t)((S * H - H) * sizeof(float) / 32);
    const uint16_t strideT = (uint16_t)((3 * H - H) * sizeof(float) / 32);

    const int64_t tot3H = B * S * 3 * H;
    const int64_t totSH = B * S * H;
    const int64_t totH = B * H;

    TPipe pipe;
    TQue<QuePosition::VECIN, BWD_CELL_QDEPTH> qGiR, qGiZ, qGiN, qGhR, qGhZ, qGhN, qHp, qDy, qDha, qDhb;
    TQue<QuePosition::VECOUT, BWD_CELL_QDEPTH> qDur, qDuz, qDun, qDghN, qDhaOut;
    pipe.InitBuffer(qGiR, BWD_CELL_QDEPTH, R * H * sizeof(float));
    pipe.InitBuffer(qGiZ, BWD_CELL_QDEPTH, R * H * sizeof(float));
    pipe.InitBuffer(qGiN, BWD_CELL_QDEPTH, R * H * sizeof(float));
    pipe.InitBuffer(qGhR, BWD_CELL_QDEPTH, R * H * sizeof(float));
    pipe.InitBuffer(qGhZ, BWD_CELL_QDEPTH, R * H * sizeof(float));
    pipe.InitBuffer(qGhN, BWD_CELL_QDEPTH, R * H * sizeof(float));
    pipe.InitBuffer(qHp, BWD_CELL_QDEPTH, R * H * sizeof(float));
    pipe.InitBuffer(qDy, BWD_CELL_QDEPTH, R * H * sizeof(float));
    pipe.InitBuffer(qDha, BWD_CELL_QDEPTH, R * H * sizeof(float));
    pipe.InitBuffer(qDhb, BWD_CELL_QDEPTH, R * H * sizeof(float));
    pipe.InitBuffer(qDur, BWD_CELL_QDEPTH, R * H * sizeof(float));
    pipe.InitBuffer(qDuz, BWD_CELL_QDEPTH, R * H * sizeof(float));
    pipe.InitBuffer(qDun, BWD_CELL_QDEPTH, R * H * sizeof(float));
    pipe.InitBuffer(qDghN, BWD_CELL_QDEPTH, R * H * sizeof(float));
    pipe.InitBuffer(qDhaOut, BWD_CELL_QDEPTH, R * H * sizeof(float));
    TBuf<TPosition::VECCALC> tmpBuf;
    pipe.InitBuffer(tmpBuf, (NT + 1) * R * H * sizeof(float));

    LocalTensor<float> s0 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 0);
    LocalTensor<float> s1 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), R * H * sizeof(float));
    LocalTensor<float> s2 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 2 * R * H * sizeof(float));
    LocalTensor<float> s3 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 3 * R * H * sizeof(float));
    LocalTensor<float> s4 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 4 * R * H * sizeof(float));
    LocalTensor<float> s5 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 5 * R * H * sizeof(float));
    LocalTensor<float> s6 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 6 * R * H * sizeof(float));
    LocalTensor<float> s7 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 7 * R * H * sizeof(float));
    LocalTensor<float> ones = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 8 * R * H * sizeof(float));

    GlobalTensor<float> giG, ghG, hpG, dyG, dhaG, dhbG, dgxG, dghG, dghCG;
    giG.SetGlobalBuffer((__gm__ float *)gi_, tot3H);
    ghG.SetGlobalBuffer((__gm__ float *)gh_, tot3H);
    hpG.SetGlobalBuffer((__gm__ float *)hp_, totSH);
    dyG.SetGlobalBuffer((__gm__ float *)dy_, totSH);
    dhaG.SetGlobalBuffer((__gm__ float *)dha_, totH);
    dhbG.SetGlobalBuffer((__gm__ float *)dhb_, totH);
    dgxG.SetGlobalBuffer((__gm__ float *)dgx_, tot3H);
    dghG.SetGlobalBuffer((__gm__ float *)dgh_, tot3H);
    // Per-step recurrent scratch: only [rows, 3H], overwritten every timestep.
    dghCG.SetGlobalBuffer((__gm__ float *)dghC_, B * 3 * H);

    for (int64_t r0 = 0; r0 < rows; r0 += R) {
        int64_t r = rows - r0;
        if (r > R) {
            r = R;
        }
        const int64_t rowBase = b0 + r0;
        const uint32_t cnt = (uint32_t)(r * H);
        const uint16_t rr = (uint16_t)r;

        const int64_t giBase = (rowBase * S + t) * 3 * H;
        const int64_t shBase = (rowBase * S + t) * H;
        const int64_t rowBaseH = rowBase * H;
        const int64_t tgBase = rowBase * 3 * H;

        DataCopyParams dp3H;
        dp3H.blockCount = rr;
        dp3H.blockLen = rowLenF;
        dp3H.srcStride = stride3H;
        dp3H.dstStride = 0;

        DataCopyParams dpSH;
        dpSH.blockCount = rr;
        dpSH.blockLen = rowLenF;
        dpSH.srcStride = strideSH;
        dpSH.dstStride = 0;

        DataCopyParams dpRow;
        dpRow.blockCount = rr;
        dpRow.blockLen = rowLenF;
        dpRow.srcStride = 0;
        dpRow.dstStride = 0;

        // ---- CopyIn -------------------------------------------------------
        auto giR = qGiR.AllocTensor<float>();
        DataCopy(giR, giG[giBase], dp3H);
        qGiR.EnQue(giR);
        auto giZ = qGiZ.AllocTensor<float>();
        DataCopy(giZ, giG[giBase + H], dp3H);
        qGiZ.EnQue(giZ);
        auto giN = qGiN.AllocTensor<float>();
        DataCopy(giN, giG[giBase + 2 * H], dp3H);
        qGiN.EnQue(giN);

        auto ghR = qGhR.AllocTensor<float>();
        DataCopy(ghR, ghG[giBase], dp3H);
        qGhR.EnQue(ghR);
        auto ghZ = qGhZ.AllocTensor<float>();
        DataCopy(ghZ, ghG[giBase + H], dp3H);
        qGhZ.EnQue(ghZ);
        auto ghN = qGhN.AllocTensor<float>();
        DataCopy(ghN, ghG[giBase + 2 * H], dp3H);
        qGhN.EnQue(ghN);

        auto hp = qHp.AllocTensor<float>();
        DataCopy(hp, hpG[shBase], dpSH);
        qHp.EnQue(hp);

        auto dy = qDy.AllocTensor<float>();
        DataCopy(dy, dyG[shBase], dpSH);
        qDy.EnQue(dy);

        auto dha = qDha.AllocTensor<float>();
        DataCopy(dha, dhaG[rowBaseH], dpRow);
        qDha.EnQue(dha);

        auto dhb = qDhb.AllocTensor<float>();
        DataCopy(dhb, dhbG[rowBaseH], dpRow);
        qDhb.EnQue(dhb);

        auto giRl = qGiR.DeQue<float>();
        auto giZl = qGiZ.DeQue<float>();
        auto giNl = qGiN.DeQue<float>();
        auto ghRl = qGhR.DeQue<float>();
        auto ghZl = qGhZ.DeQue<float>();
        auto ghNl = qGhN.DeQue<float>();
        auto hpL = qHp.DeQue<float>();
        auto dyL = qDy.DeQue<float>();
        auto dhaL = qDha.DeQue<float>();
        auto dhbL = qDhb.DeQue<float>();

        auto durL = qDur.AllocTensor<float>();
        auto duzL = qDuz.AllocTensor<float>();
        auto dunL = qDun.AllocTensor<float>();
        auto dghNL = qDghN.AllocTensor<float>();
        auto dhaOutL = qDhaOut.AllocTensor<float>();

        // ---- Compute ------------------------------------------------------
        Duplicate(ones, 1.0f, cnt);

        // s0 = dht = dy + dha + dhb
        Add(s0, dyL, dhaL, cnt);
        Add(s0, s0, dhbL, cnt);

        // s6 = r = sigmoid(giR + ghR)
        Add(s2, giRl, ghRl, cnt);
        Muls(s2, s2, -1.0f, cnt);
        Exp(s2, s2, cnt);
        Adds(s2, s2, 1.0f, cnt);
        Div(s6, ones, s2, cnt);

        // s7 = z = sigmoid(giZ + ghZ)
        Add(s3, giZl, ghZl, cnt);
        Muls(s3, s3, -1.0f, cnt);
        Exp(s3, s3, cnt);
        Adds(s3, s3, 1.0f, cnt);
        Div(s7, ones, s3, cnt);

        // s1 = n = tanh(giN + r * ghN)
        Mul(s4, s6, ghNl, cnt);
        Add(s4, giNl, s4, cnt);
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
        Sub(s3, hpL, s1, cnt);
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
        Adds(duzL, s5, 0.0f, (int32_t)cnt);

        // dghN = dun * r
        Mul(dghNL, s4, s6, cnt);

        // dur = dun * ghN * r * (1 - r)
        Mul(s5, s4, ghNl, cnt);
        Muls(s2, s6, -1.0f, cnt);
        Adds(s2, s2, 1.0f, cnt);
        Mul(s2, s2, s6, cnt);
        Mul(s2, s2, s5, cnt);
        Adds(durL, s2, 0.0f, (int32_t)cnt);

        // dun
        Adds(dunL, s4, 0.0f, (int32_t)cnt);

        // dha' = dht * z
        Mul(dhaOutL, s0, s7, cnt);

        qGiR.FreeTensor(giRl);
        qGiZ.FreeTensor(giZl);
        qGiN.FreeTensor(giNl);
        qGhR.FreeTensor(ghRl);
        qGhZ.FreeTensor(ghZl);
        qGhN.FreeTensor(ghNl);
        qHp.FreeTensor(hpL);
        qDy.FreeTensor(dyL);
        qDha.FreeTensor(dhaL);
        qDhb.FreeTensor(dhbL);

        qDur.EnQue(durL);
        qDuz.EnQue(duzL);
        qDun.EnQue(dunL);
        qDghN.EnQue(dghNL);
        qDhaOut.EnQue(dhaOutL);

        // ---- CopyOut ------------------------------------------------------
        DataCopyParams dpW3H = dp3H;  // strided into [rows, S, 3H]
        dpW3H.srcStride = 0;
        dpW3H.dstStride = stride3H;

        DataCopyParams dpWT = dp3H;  // contiguous [rows, 3H]
        dpWT.srcStride = 0;
        dpWT.dstStride = strideT;

        // dgx = (dur, duz, dun); dgh = (dur, duz, dghN). Each shared tile feeds
        // two destinations before it is freed: the [rows*S, 3H] buffer that the
        // parameter-gradient GEMMs and the bias reduction consume, and (for dgh)
        // the per-step [rows, 3H] scratch read by the recurrent GEMM.
        auto durD = qDur.DeQue<float>();
        DataCopy(dgxG[giBase], durD, dpW3H);
        DataCopy(dghG[giBase], durD, dpW3H);
        DataCopy(dghCG[tgBase], durD, dpWT);
        qDur.FreeTensor(durD);

        auto duzD = qDuz.DeQue<float>();
        DataCopy(dgxG[giBase + H], duzD, dpW3H);
        DataCopy(dghG[giBase + H], duzD, dpW3H);
        DataCopy(dghCG[tgBase + H], duzD, dpWT);
        qDuz.FreeTensor(duzD);

        auto dunD = qDun.DeQue<float>();
        DataCopy(dgxG[giBase + 2 * H], dunD, dpW3H);
        qDun.FreeTensor(dunD);

        auto dghND = qDghN.DeQue<float>();
        DataCopy(dghG[giBase + 2 * H], dghND, dpW3H);
        DataCopy(dghCG[tgBase + 2 * H], dghND, dpWT);
        qDghN.FreeTensor(dghND);

        // dha is contiguous [rows, H]
        DataCopyParams dpRowW = dpRow;
        dpRowW.srcStride = 0;
        dpRowW.dstStride = 0;
        auto dhaOutD = qDhaOut.DeQue<float>();
        DataCopy(dhaG[rowBaseH], dhaOutD, dpRowW);
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
    TQue<QuePosition::VECIN, 1> qLoad;
    TQue<QuePosition::VECOUT, 1> qOut;
    pipe.InitBuffer(qLoad, 1, RR * G * sizeof(float));
    pipe.InitBuffer(qOut, 1, G * sizeof(float));
    TBuf<TPosition::VECCALC> accBuf;
    pipe.InitBuffer(accBuf, G * sizeof(float));

    GlobalTensor<float> src[2];
    src[0].SetGlobalBuffer((__gm__ float *)dgx_, td.chunkRows * td.S * G);
    src[1].SetGlobalBuffer((__gm__ float *)dgh_, td.chunkRows * td.S * G);
    GlobalTensor<float> wsG;
    wsG.SetGlobalBuffer((__gm__ float *)ws_, 2 * blkNum * G);

    for (int64_t which = 0; which < 2; ++which) {
        auto acc = accBuf.Get<float>();
        Duplicate(acc, 0.0f, (int32_t)G);
        for (int64_t rr = 0; rr < rn; rr += RR) {
            int64_t c = rn - rr;
            if (c > RR) {
                c = RR;
            }
            auto ld = qLoad.AllocTensor<float>();
            DataCopy(ld, src[which][(r0 + rr) * G], (uint32_t)(c * G));
            qLoad.EnQue(ld);
            auto ldd = qLoad.DeQue<float>();
            for (int64_t i = 0; i < c; ++i) {
                Add(acc, acc, ldd[i * G], (int32_t)G);
            }
            qLoad.FreeTensor(ldd);
        }
        // Route the store through a VECOUT queue so the V -> MTE3 edge is
        // queue-managed (acc lives in a plain VECCALC buffer).
        auto o = qOut.AllocTensor<float>();
        Adds(o, acc, 0.0f, (int32_t)G);
        qOut.EnQue(o);
        auto od = qOut.DeQue<float>();
        DataCopy(wsG[(which * blkNum + blk) * G], od, (uint32_t)G);
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
                                                   GM_ADDR dgh, GM_ADDR dghC, GM_ADDR tilingGM,
                                                   int64_t t)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    if ASCEND_IS_AIV {
        BwdCellBody(gi, gh, hp, dy, dha, dhb, dgx, dgh, dghC, tilingGM, t);
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
