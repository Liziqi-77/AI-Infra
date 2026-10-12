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
 * \file gru_vector.cpp
 * \brief Vector-side kernels for the fused GRU operator.
 *
 *   gru_cast_fp32 : x -> fp32 (acts as a pure copy when x is already fp32)
 *   gru_rec_elem  : one recurrent cell update for a single timestep
 *
 * The cell update for hidden row block R:
 *   u_r = giR + ghR        r = 1 / (1 + exp(-u_r))
 *   u_z = giZ + ghZ        z = 1 / (1 + exp(-u_z))
 *   u_n = giN + r * ghN    n = 1 - 2 / (1 + exp(2 * u_n))
 *   h_t = (1 - z) * n + z * h_{t-1}
 *
 * sigmoid / tanh are built from exp + reciprocal only, so no auxiliary
 * high-level-API tmp buffers are needed.
 */

#include "gru_common.h"

using namespace AscendC;
using namespace gru_kernel;

namespace {

// ---------------------------------------------------------------------------
// 1) Cast x -> fp32
// ---------------------------------------------------------------------------
template <typename inT>
__aicore__ inline void GruCastBody(GM_ADDR src, GM_ADDR dst, GM_ADDR tilingGM)
{
    GruTilingData td;
    CopyTilingToLocal(&td, tilingGM);

    const int64_t total = td.chunkRows * td.S * td.I;
    const int64_t blkNum = (int64_t)GetBlockNum();
    const int64_t blk = (int64_t)GetBlockIdx();

    // Keep every core's range a multiple of 16 elements so that the tail DataCopy
    // stays 32B aligned for both 2-byte and 4-byte element types.
    int64_t perCore = (total + blkNum - 1) / blkNum;
    perCore = ((perCore + 15) / 16) * 16;
    const int64_t start = blk * perCore;
    if (start >= total) {
        return;
    }
    int64_t cnt = perCore;
    if (start + cnt > total) {
        cnt = total - start;
    }

    constexpr int64_t CH = 4096;
    TPipe pipe;
    TQue<QuePosition::VECIN, 2> qIn;
    TQue<QuePosition::VECOUT, 2> qOut;
    pipe.InitBuffer(qIn, 2, CH * sizeof(inT));
    pipe.InitBuffer(qOut, 2, CH * sizeof(float));

    GlobalTensor<inT> srcG;
    srcG.SetGlobalBuffer((__gm__ inT *)src + start, cnt);
    GlobalTensor<float> dstG;
    dstG.SetGlobalBuffer((__gm__ float *)dst + start, cnt);

    for (int64_t off = 0; off < cnt; off += CH) {
        int64_t c = cnt - off;
        if (c > CH) {
            c = CH;
        }
        auto inLocal = qIn.AllocTensor<inT>();
        DataCopy(inLocal, srcG[off], (uint32_t)c);
        qIn.EnQue(inLocal);

        auto inD = qIn.DeQue<inT>();
        auto outLocal = qOut.AllocTensor<float>();
        Cast(outLocal, inD, RoundMode::CAST_NONE, (uint32_t)c);
        qOut.EnQue(outLocal);
        qIn.FreeTensor(inD);

        auto outD = qOut.DeQue<float>();
        DataCopy(dstG[off], outD, (uint32_t)c);
        qOut.FreeTensor(outD);
    }
}

// ---------------------------------------------------------------------------
// 2) Recurrent cell update for one timestep
// ---------------------------------------------------------------------------

// Rows of the hidden state handled per UB tile (H = 512 -> 8 KB per [R, H] fp32 buffer).
constexpr int64_t GRU_TILE_ROWS = 4;

// fp32 -> outT narrowing on the vector pipe; fp32 output is a pure move.
template <typename outT>
struct GruCastOut {
    static __aicore__ inline void Run(const LocalTensor<outT> &dst, const LocalTensor<float> &src, uint32_t n)
    {
        Cast(dst, src, RoundMode::CAST_RINT, n);
    }
};

template <>
struct GruCastOut<float> {
    static __aicore__ inline void Run(const LocalTensor<float> &dst, const LocalTensor<float> &src, uint32_t n)
    {
        Adds(dst, src, 0.0f, (int32_t)n);
    }
};

template <typename outT>
__aicore__ inline void GruRecElemBody(GM_ADDR gi_, GM_ADDR gh_,
                                      GM_ADDR hs_, GM_ADDR out_, GM_ADDR ghStore_,
                                      GM_ADDR tilingGM, int64_t t, int64_t doStore)
{
    GruTilingData td;
    CopyTilingToLocal(&td, tilingGM);
    const int64_t B = td.chunkRows;  // rows handled by this launch
    const int64_t S = td.S;
    const int64_t H = td.H;
    const int64_t G = 3 * H;  // merged gate extent (r | z | n contiguous)

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

    constexpr int64_t R = GRU_TILE_ROWS;
    const uint32_t tileCnt = (uint32_t)(R * H);
    const uint16_t rowLenF32 = (uint16_t)(H * sizeof(float) / 32);        // blocks of 32B
    // gi is [rows*S, 3H] and gh is [rows, 3H]: within one gate the rows sit G
    // floats apart (gi) / G floats apart (gh), so the per-gate block stride is
    // (S*G - H) for gi and (G - H) for gh.
    const uint16_t giRowStride = (uint16_t)((S * G - H) * sizeof(float) / 32);
    const uint16_t ghRowStride = (uint16_t)((G - H) * sizeof(float) / 32);
    const uint16_t outRowLen = (uint16_t)(H * sizeof(outT) / 32);
    const uint16_t outRowStride = (uint16_t)((S * H - H) * sizeof(outT) / 32);

    TPipe pipe;
    TQue<QuePosition::VECIN, 1> qGiR, qGiZ, qGiN, qGhR, qGhZ, qGhN, qHs;
    TQue<QuePosition::VECOUT, 1> qHsOut, qOut;
    pipe.InitBuffer(qGiR, 1, R * H * sizeof(float));
    pipe.InitBuffer(qGiZ, 1, R * H * sizeof(float));
    pipe.InitBuffer(qGiN, 1, R * H * sizeof(float));
    pipe.InitBuffer(qGhR, 1, R * H * sizeof(float));
    pipe.InitBuffer(qGhZ, 1, R * H * sizeof(float));
    pipe.InitBuffer(qGhN, 1, R * H * sizeof(float));
    pipe.InitBuffer(qHs, 1, R * H * sizeof(float));
    pipe.InitBuffer(qHsOut, 1, R * H * sizeof(float));
    pipe.InitBuffer(qOut, 1, R * H * sizeof(outT));
    TBuf<TPosition::VECCALC> tmpBuf;
    pipe.InitBuffer(tmpBuf, 7 * R * H * sizeof(float));

    LocalTensor<float> s0 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 0);
    LocalTensor<float> s1 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), R * H * sizeof(float));
    LocalTensor<float> s2 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 2 * R * H * sizeof(float));
    LocalTensor<float> s3 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 3 * R * H * sizeof(float));
    LocalTensor<float> s4 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 4 * R * H * sizeof(float));
    LocalTensor<float> s5 = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 5 * R * H * sizeof(float));
    // Constant-1 operand for the reciprocals (see the Compute section).
    LocalTensor<float> ones = tmpBuf.GetWithOffset<float>((uint32_t)(R * H), 6 * R * H * sizeof(float));

    GlobalTensor<float> giRg, giZg, giNg, ghRg, ghZg, ghNg, hsg;
    GlobalTensor<float> ghStoreG;
    GlobalTensor<outT> outg;

    for (int64_t r0 = 0; r0 < rows; r0 += R) {
        int64_t r = rows - r0;
        if (r > R) {
            r = R;
        }
        const int64_t rowBase = b0 + r0;  // first batch row of this tile
        const uint32_t cnt = (uint32_t)(r * H);
        const uint16_t rows16 = (uint16_t)r;

        // ---- CopyIn -------------------------------------------------------
        giRg.SetGlobalBuffer((__gm__ float *)gi_ + (rowBase * S + t) * G, (r - 1) * S * G + G);
        giZg.SetGlobalBuffer((__gm__ float *)gi_ + (rowBase * S + t) * G + H, (r - 1) * S * G + G);
        giNg.SetGlobalBuffer((__gm__ float *)gi_ + (rowBase * S + t) * G + 2 * H, (r - 1) * S * G + G);
        ghRg.SetGlobalBuffer((__gm__ float *)gh_ + rowBase * G, r * G);
        ghZg.SetGlobalBuffer((__gm__ float *)gh_ + rowBase * G + H, r * G);
        ghNg.SetGlobalBuffer((__gm__ float *)gh_ + rowBase * G + 2 * H, r * G);
        hsg.SetGlobalBuffer((__gm__ float *)hs_ + rowBase * H, r * H);

        auto giRl = qGiR.AllocTensor<float>();
        auto giZl = qGiZ.AllocTensor<float>();
        auto giNl = qGiN.AllocTensor<float>();
        auto ghRl = qGhR.AllocTensor<float>();
        auto ghZl = qGhZ.AllocTensor<float>();
        auto ghNl = qGhN.AllocTensor<float>();
        auto hsL = qHs.AllocTensor<float>();

        DataCopyParams dpGi;
        dpGi.blockCount = rows16;
        dpGi.blockLen = rowLenF32;
        dpGi.srcStride = giRowStride;
        dpGi.dstStride = 0;
        DataCopy(giRl, giRg, dpGi);
        DataCopy(giZl, giZg, dpGi);
        DataCopy(giNl, giNg, dpGi);

        DataCopyParams dpGh;
        dpGh.blockCount = rows16;
        dpGh.blockLen = rowLenF32;
        dpGh.srcStride = ghRowStride;
        dpGh.dstStride = 0;
        DataCopy(ghRl, ghRg, dpGh);
        DataCopy(ghZl, ghZg, dpGh);
        DataCopy(ghNl, ghNg, dpGh);

        DataCopyParams dpRow;
        dpRow.blockCount = rows16;
        dpRow.blockLen = rowLenF32;
        dpRow.srcStride = 0;
        dpRow.dstStride = 0;
        DataCopy(hsL, hsg, dpRow);

        qGiR.EnQue(giRl);
        qGiZ.EnQue(giZl);
        qGiN.EnQue(giNl);
        qGhR.EnQue(ghRl);
        qGhZ.EnQue(ghZl);
        qGhN.EnQue(ghNl);
        qHs.EnQue(hsL);

        auto giR = qGiR.DeQue<float>();
        auto giZ = qGiZ.DeQue<float>();
        auto giN = qGiN.DeQue<float>();
        auto ghR = qGhR.DeQue<float>();
        auto ghZ = qGhZ.DeQue<float>();
        auto ghN = qGhN.DeQue<float>();
        auto hsOld = qHs.DeQue<float>();

        // ---- Compute ------------------------------------------------------
        // The sigmoid/tanh factors below are `1 / (1 + exp(.) )`.  The reciprocal
        // is taken with Div against a constant-1 tensor rather than with
        // Reciprocal: Reciprocal is documented to remain below fp32 accuracy
        // (basic_arithmetic/Reciprocal.md: "float 的算子结果对比误差不满足双万分
        // 之一的要求，如果需要高精度，建议使用 Div 替代实现"), while Div is
        // 1-ulp.  That gap (~2^-10 relative) is exactly the fp32 accuracy budget,
        // so the fp32 gate cannot be met with Reciprocal.
        Duplicate(ones, 1.0f, cnt);

        // r = sigmoid(giR + ghR) -> s1
        Add(s0, giR, ghR, cnt);
        Muls(s1, s0, -1.0f, cnt);
        Exp(s2, s1, cnt);
        Adds(s2, s2, 1.0f, cnt);
        Div(s1, ones, s2, cnt);

        // z = sigmoid(giZ + ghZ) -> s3
        Add(s2, giZ, ghZ, cnt);
        Muls(s3, s2, -1.0f, cnt);
        Exp(s4, s3, cnt);
        Adds(s4, s4, 1.0f, cnt);
        Div(s3, ones, s4, cnt);

        // n = tanh(giN + r * ghN) -> s4   (1 - 2/(1 + exp(2u)))
        Mul(s0, s1, ghN, cnt);
        Add(s0, giN, s0, cnt);
        Muls(s2, s0, 2.0f, cnt);
        Exp(s4, s2, cnt);
        Adds(s4, s4, 1.0f, cnt);
        Div(s5, ones, s4, cnt);
        Muls(s5, s5, 2.0f, cnt);
        Muls(s5, s5, -1.0f, cnt);
        Adds(s4, s5, 1.0f, cnt);

        // h_t = (1 - z) * n + z * h_{t-1} -> s5
        Muls(s5, s3, -1.0f, cnt);
        Adds(s5, s5, 1.0f, cnt);
        Mul(s2, s5, s4, cnt);
        Mul(s0, s3, hsOld, cnt);
        Add(s5, s2, s0, cnt);

        // ---- Store the recurrent pre-activation (opt-in) -------------------
        // When the backward is allowed to read the forward's gate
        // pre-activations instead of recomputing them (GRU_STORE_GATES), the
        // per-step gh is written here into the full-batch [B*S, 3H] store at
        // column t. This runs *before* the gh queues are freed (they are the
        // source). The destination already sits in UB, so this is one extra
        // MTE3 transfer of the same tile -- no extra HBM read. The row stride
        // is identical to the gi operand's, so the backward cell can index the
        // store exactly like gi. qHsOut is a spare fp32 VECOUT buffer here (its
        // own use is later in the same iteration), so the staging needs no
        // extra UB: the cell's queue set already sits close to the UB limit and
        // an added queue silently overlapped and corrupted the output.
        if (doStore != 0) {
            DataCopyParams dpSt;
            dpSt.blockCount = rows16;
            dpSt.blockLen = rowLenF32;
            dpSt.srcStride = 0;
            dpSt.dstStride = giRowStride;

            const int64_t stBase = (rowBase * S + t) * G;
            const int64_t stExt = (r - 1) * S * G + G;

            ghStoreG.SetGlobalBuffer((__gm__ float *)ghStore_ + stBase, stExt);
            auto rl0 = qHsOut.AllocTensor<float>();
            Adds(rl0, ghR, 0.0f, (int32_t)cnt);
            qHsOut.EnQue(rl0);
            auto rd0 = qHsOut.DeQue<float>();
            DataCopy(ghStoreG, rd0, dpSt);
            qHsOut.FreeTensor(rd0);

            ghStoreG.SetGlobalBuffer((__gm__ float *)ghStore_ + stBase + H, stExt);
            auto rl1 = qHsOut.AllocTensor<float>();
            Adds(rl1, ghZ, 0.0f, (int32_t)cnt);
            qHsOut.EnQue(rl1);
            auto rd1 = qHsOut.DeQue<float>();
            DataCopy(ghStoreG, rd1, dpSt);
            qHsOut.FreeTensor(rd1);

            ghStoreG.SetGlobalBuffer((__gm__ float *)ghStore_ + stBase + 2 * H, stExt);
            auto rl2 = qHsOut.AllocTensor<float>();
            Adds(rl2, ghN, 0.0f, (int32_t)cnt);
            qHsOut.EnQue(rl2);
            auto rd2 = qHsOut.DeQue<float>();
            DataCopy(ghStoreG, rd2, dpSt);
            qHsOut.FreeTensor(rd2);
        }

        qGiR.FreeTensor(giR);
        qGiZ.FreeTensor(giZ);
        qGiN.FreeTensor(giN);
        qGhR.FreeTensor(ghR);
        qGhZ.FreeTensor(ghZ);
        qGhN.FreeTensor(ghN);
        qHs.FreeTensor(hsOld);

        // ---- CopyOut ------------------------------------------------------
        // Both transfers out of the VECCALC scratch go through the vector pipe
        // (Adds / Cast) so the only cross-pipe edges are the queue-managed
        // V->MTE3 ones taken care of by EnQue/DeQue.
        auto hsOut = qHsOut.AllocTensor<float>();
        Adds(hsOut, s5, 0.0f, (int32_t)cnt);
        qHsOut.EnQue(hsOut);

        auto outLocal = qOut.AllocTensor<outT>();
        GruCastOut<outT>::Run(outLocal, s5, cnt);
        qOut.EnQue(outLocal);

        auto hsOutD = qHsOut.DeQue<float>();
        DataCopy(hsg, hsOutD, dpRow);
        qHsOut.FreeTensor(hsOutD);

        auto outD = qOut.DeQue<outT>();
        outg.SetGlobalBuffer((__gm__ outT *)out_ + (rowBase * S + t) * H, (r - 1) * S * H + H);
        DataCopyParams dpOut;
        dpOut.blockCount = rows16;
        dpOut.blockLen = outRowLen;
        dpOut.srcStride = 0;
        dpOut.dstStride = outRowStride;
        DataCopy(outg, outD, dpOut);
        qOut.FreeTensor(outD);
    }
}

}  // namespace

extern "C" __global__ __aicore__ void gru_cast_fp32(GM_ADDR src, GM_ADDR dst, GM_ADDR tilingGM, int64_t inDtype)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    if (inDtype == 1) {
        GruCastBody<half>(src, dst, tilingGM);
    } else if (inDtype == 2) {
        GruCastBody<bfloat16_t>(src, dst, tilingGM);
    } else {
        GruCastBody<float>(src, dst, tilingGM);
    }
}

extern "C" __global__ __aicore__ void gru_rec_elem(GM_ADDR gi, GM_ADDR gh,
                                                   GM_ADDR hs, GM_ADDR out, GM_ADDR ghStore,
                                                   GM_ADDR tilingGM,
                                                   int64_t t, int64_t outDtype, int64_t doStore)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    if ASCEND_IS_AIV {
        if (outDtype == 1) {
            GruRecElemBody<half>(gi, gh, hs, out, ghStore, tilingGM, t, doStore);
        } else if (outDtype == 2) {
            GruRecElemBody<bfloat16_t>(gi, gh, hs, out, ghStore, tilingGM, t, doStore);
        } else {
            GruRecElemBody<float>(gi, gh, hs, out, ghStore, tilingGM, t, doStore);
        }
    }
}
