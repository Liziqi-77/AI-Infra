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
 * \file hwm.cpp
 * \brief Device side of the Hull-White short-rate Monte-Carlo path simulation.
 *
 * Reference (CUDA, GPU_Test/kernel.cu :: HWMCKernel), one thread per path i:
 *
 *     rate[i][0] = r0
 *     for j in 0 .. mStep-2:
 *         dt       = accrual[j+1] - accrual[j]
 *         beta(t)  = (sig/a) * (1 - exp(-a*t))
 *         alpha_k  = 0.5 * beta(accrual[k])^2 + f_[k][j]          (k = 1 -> f1, k = 2 -> f2)
 *         decay    = exp(-a*dt)
 *         var      = 0.5*sig^2/a * (1 - exp(-2*a*dt))            (sig^2*dt if a ~ 0)
 *         rate[i][j+1] = rate[i][j]*decay + alpha_2 - alpha_1*decay + sqrt(var)*nrnd[i][j]
 *
 * The recurrence is sequential in j and independent across paths, so the whole
 * simulation is carried out as one vector operation per time step, vectorised
 * over the path axis.
 *
 * Data layout
 * -----------
 * `rate` is row-major [nPath][mStep], so a *column* (fixed j, varying i) is
 * strided by mStep floats.  A stride-4-byte block copy moves one such column
 * into UB: DataCopyPad with blockCount = corePaths, blockLen = 4 bytes and
 * srcStride = (mStep-2)*4 bytes, whose destination blocks land in consecutive
 * 32-byte slots (dstStride = 0, the A2 form measures UB strides in 32-byte
 * dataBlocks).  Each path therefore ends up owning one 32-byte slot of the UB
 * buffer, with its live value in lane 0 and seven don't-care lanes that take
 * part in every vector op but are never read back.  The reverse copy (column
 * store) uses srcStride = 0 / dstStride = (mStep-1)*4 bytes.
 *
 * The nine per-step scalars (decay, alpha_2 - alpha_1*decay, sqrt(var)) depend
 * only on j and are identical for every path, so they are produced once as
 * 9-element vectors with the vector Exp/Sqrt primitives and then read back into
 * scalar registers with LocalTensor::GetValue (S pipe), which keeps the
 * transcendental work off the scalar pipe and out of the j loop.
 *
 * The j loop itself is four block-level vector ops on the running state:
 *     state = c1[j]*state ; col = c3[j]*nrnd_col + c2[j] ; state += col
 */

#include "kernel_operator.h"

using namespace AscendC;

namespace {

// Floats per path slot in the UB spreading layout: one 32-byte dataBlock.
constexpr int64_t SLOT_FLOATS = 8;
// Scratch slots in the coefficient buffer, each padded to a whole dataBlock.
constexpr int64_t COEF_SLOTS = 13;
constexpr int64_t COEF_SLOT_FLOATS = 32;
constexpr int64_t COEF_FLOATS = COEF_SLOTS * COEF_SLOT_FLOATS;

// Vector ops work on whole 32-byte blocks; a path count below this would leave
// the tail of the buffer outside the repeat coverage.
constexpr int64_t MIN_STATE_FLOATS = 64;

}  // namespace

extern "C" __global__ __aicore__ void hwm_kernel(GM_ADDR hwparams, GM_ADDR accrual, GM_ADDR f1, GM_ADDR f2,
                                                 GM_ADDR nrnd, GM_ADDR rate, int64_t nPath, int64_t mStep,
                                                 int64_t pathsPerCore)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);

    const int64_t nCol = mStep - 1;

    // ---- path range owned by this core ------------------------------------
    const int64_t blk = static_cast<int64_t>(GetBlockIdx());
    const int64_t pathBegin = blk * pathsPerCore;
    if (pathBegin >= nPath) {
        return;
    }
    int64_t corePaths = nPath - pathBegin;
    if (corePaths > pathsPerCore) {
        corePaths = pathsPerCore;
    }

    int64_t stateFloats = corePaths * SLOT_FLOATS;
    if (stateFloats < MIN_STATE_FLOATS) {
        stateFloats = MIN_STATE_FLOATS;
    }

    // ---- UB partition ------------------------------------------------------
    TPipe pipe;
    TBuf<TPosition::VECCALC> stateBuf;
    TBuf<TPosition::VECCALC> coefBuf;
    TQue<QuePosition::VECIN, 2> colQue;
    pipe.InitBuffer(stateBuf, stateFloats * static_cast<int64_t>(sizeof(float)));
    pipe.InitBuffer(coefBuf, COEF_FLOATS * static_cast<int64_t>(sizeof(float)));
    pipe.InitBuffer(colQue, 2, stateFloats * static_cast<int64_t>(sizeof(float)));

    // ---- GM views ----------------------------------------------------------
    GlobalTensor<float> hwG;
    hwG.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(hwparams), 3);
    GlobalTensor<float> accG;
    accG.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(accrual), mStep);
    GlobalTensor<float> f1G;
    f1G.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(f1), nCol);
    GlobalTensor<float> f2G;
    f2G.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(f2), nCol);
    GlobalTensor<float> nrG;
    nrG.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(nrnd), nPath * nCol);
    GlobalTensor<float> rateG;
    rateG.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(rate), nPath * mStep);

    LocalTensor<float> coef = coefBuf.Get<float>();
    LocalTensor<float> acc0 = coef[0 * COEF_SLOT_FLOATS];   // accrual[0 .. mStep-1]
    LocalTensor<float> acc1 = coef[1 * COEF_SLOT_FLOATS];   // accrual[1 .. mStep-1]
    LocalTensor<float> f1v = coef[2 * COEF_SLOT_FLOATS];    // f1[0 .. nCol-1]
    LocalTensor<float> f2v = coef[3 * COEF_SLOT_FLOATS];    // f2[0 .. nCol-1]
    LocalTensor<float> hwv = coef[4 * COEF_SLOT_FLOATS];    // {r0, a, sigma}
    LocalTensor<float> dtv = coef[5 * COEF_SLOT_FLOATS];
    LocalTensor<float> c1v = coef[6 * COEF_SLOT_FLOATS];
    LocalTensor<float> a1v = coef[7 * COEF_SLOT_FLOATS];
    LocalTensor<float> a2v = coef[8 * COEF_SLOT_FLOATS];
    LocalTensor<float> c2v = coef[9 * COEF_SLOT_FLOATS];
    LocalTensor<float> c3v = coef[10 * COEF_SLOT_FLOATS];
    LocalTensor<float> tv0 = coef[11 * COEF_SLOT_FLOATS];
    LocalTensor<float> tv1 = coef[12 * COEF_SLOT_FLOATS];

    // Zero the scratch so the don't-care lanes beyond the copied extent are
    // finite and the vector primitives never see stale UB content.
    Duplicate(coef, 0.0f, static_cast<int32_t>(COEF_FLOATS));
    SetFlag<HardEvent::V_MTE2>(EVENT_ID0);
    WaitFlag<HardEvent::V_MTE2>(EVENT_ID0);

    // ---- CopyIn: the three small per-step input vectors --------------------
    // acc1 is read from accrual+1 rather than sliced off acc0 in UB: a UB
    // LocalTensor view at a 4-byte offset would break the 32-byte alignment
    // the vector unit needs, whereas the GM side of DataCopyPad only requires
    // byte alignment.
    DataCopyPadExtParams<float> padP{false, 0, 0, 0.0f};
    DataCopyExtParams acc0Cp{1, static_cast<uint32_t>(mStep * sizeof(float)), 0u, 0u, 0u};
    DataCopyExtParams acc1Cp{1, static_cast<uint32_t>(nCol * sizeof(float)), 0u, 0u, 0u};
    DataCopyExtParams hwCp{1, static_cast<uint32_t>(3 * sizeof(float)), 0u, 0u, 0u};
    DataCopyPad(acc0, accG[0], acc0Cp, padP);
    DataCopyPad(acc1, accG[1], acc1Cp, padP);
    DataCopyPad(f1v, f1G[0], acc1Cp, padP);
    DataCopyPad(f2v, f2G[0], acc1Cp, padP);
    DataCopyPad(hwv, hwG[0], hwCp, padP);

    SetFlag<HardEvent::MTE2_S>(EVENT_ID0);
    WaitFlag<HardEvent::MTE2_S>(EVENT_ID0);
    const float r0 = hwv.GetValue(0);
    const float a = hwv.GetValue(1);
    const float sig = hwv.GetValue(2);

    SetFlag<HardEvent::MTE2_V>(EVENT_ID0);
    WaitFlag<HardEvent::MTE2_V>(EVENT_ID0);

    // ---- per-step coefficients as 9-element vectors ------------------------
    // Sixteen lanes (two 32-byte blocks) are processed even though only the
    // first nCol are meaningful; the surplus lanes are never read.
    const int32_t coefCnt = 16;

    // dt[j] = accrual[j+1] - accrual[j]
    Sub(dtv, acc1, acc0, coefCnt);

    // c1[j] = exp(-a*dt)
    Muls(tv0, dtv, -a, coefCnt);
    Exp(c1v, tv0, coefCnt);

    // c3[j] = sqrt( 0.5*sig^2/a * (1 - exp(-2*a*dt)) )   (sig^2*dt for a ~ 0)
    if (a < 1e-10f) {
        Muls(c3v, dtv, sig * sig, coefCnt);
    } else {
        Muls(tv0, dtv, -2.0f * a, coefCnt);
        Exp(tv1, tv0, coefCnt);
        // exp(-2 a dt) - 1, spelled as an add of -1: the tensor-minus-scalar
        // primitive (Subs) has no dav-c220 implementation (Atlas A2 does not
        // support it), while Adds does.
        Adds(tv1, tv1, -1.0f, coefCnt);
        Muls(c3v, tv1, -0.5f * sig * sig / a, coefCnt);    // variance
    }
    Sqrt(c3v, c3v, coefCnt);

    // alpha_1[j] = 0.5*((sig/a)*(1-exp(-a*accrual[j])))^2 + f1[j]
    Muls(tv0, acc0, -a, coefCnt);
    Exp(tv0, tv0, coefCnt);
    Adds(tv0, tv0, -1.0f, coefCnt);
    Muls(tv0, tv0, -sig / a, coefCnt);
    Mul(tv1, tv0, tv0, coefCnt);
    Muls(tv1, tv1, 0.5f, coefCnt);
    Add(a1v, tv1, f1v, coefCnt);

    // alpha_2[j] = 0.5*((sig/a)*(1-exp(-a*accrual[j+1])))^2 + f2[j]
    Muls(tv0, acc1, -a, coefCnt);
    Exp(tv0, tv0, coefCnt);
    Adds(tv0, tv0, -1.0f, coefCnt);
    Muls(tv0, tv0, -sig / a, coefCnt);
    Mul(tv1, tv0, tv0, coefCnt);
    Muls(tv1, tv1, 0.5f, coefCnt);
    Add(a2v, tv1, f2v, coefCnt);

    // c2[j] = alpha_2 - alpha_1*decay
    Mul(tv0, a1v, c1v, coefCnt);
    Sub(c2v, a2v, tv0, coefCnt);

    SetFlag<HardEvent::V_S>(EVENT_ID0);
    WaitFlag<HardEvent::V_S>(EVENT_ID0);

    // ---- recurrence --------------------------------------------------------
    LocalTensor<float> state = stateBuf.Get<float>();
    Duplicate(state, r0, static_cast<int32_t>(stateFloats));

    // Column 0 of every owned path is r0.
    DataCopyExtParams colOut{static_cast<uint16_t>(corePaths), static_cast<uint32_t>(sizeof(float)), 0u,
                             static_cast<uint32_t>((mStep - 1) * static_cast<int64_t>(sizeof(float))), 0u};
    SetFlag<HardEvent::V_MTE3>(EVENT_ID0);
    WaitFlag<HardEvent::V_MTE3>(EVENT_ID0);
    DataCopyPad(rateG[pathBegin * mStep], state, colOut);
    SetFlag<HardEvent::MTE3_V>(EVENT_ID0);
    WaitFlag<HardEvent::MTE3_V>(EVENT_ID0);

    DataCopyExtParams colIn{static_cast<uint16_t>(corePaths), static_cast<uint32_t>(sizeof(float)),
                            static_cast<uint32_t>((nCol - 1) * static_cast<int64_t>(sizeof(float))), 0u, 0u};

    for (int64_t j = 0; j < nCol; ++j) {
        const float k1 = c1v.GetValue(static_cast<uint32_t>(j));
        const float k2 = c2v.GetValue(static_cast<uint32_t>(j));
        const float k3 = c3v.GetValue(static_cast<uint32_t>(j));

        LocalTensor<float> col = colQue.AllocTensor<float>();
        DataCopyPad(col, nrG[pathBegin * nCol + j], colIn, padP);
        colQue.EnQue(col);
        col = colQue.DeQue<float>();

        Muls(state, state, k1, static_cast<int32_t>(stateFloats));
        Muls(col, col, k3, static_cast<int32_t>(stateFloats));
        Adds(col, col, k2, static_cast<int32_t>(stateFloats));
        Add(state, state, col, static_cast<int32_t>(stateFloats));

        colQue.FreeTensor(col);

        SetFlag<HardEvent::V_MTE3>(EVENT_ID0);
        WaitFlag<HardEvent::V_MTE3>(EVENT_ID0);
        DataCopyPad(rateG[pathBegin * mStep + j + 1], state, colOut);
        SetFlag<HardEvent::MTE3_V>(EVENT_ID0);
        WaitFlag<HardEvent::MTE3_V>(EVENT_ID0);
    }
}
