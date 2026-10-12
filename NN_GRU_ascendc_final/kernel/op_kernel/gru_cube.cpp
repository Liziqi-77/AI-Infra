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
 * \file gru_cube.cpp
 * \brief Cube-side GEMM entry point for the fused GRU operator.
 *
 *   GI_g = xf @ W_ih_g^T + b_ih_g            (M = rows*S, N = H, K = I)
 *   GH_g = h  @ W_hh_g^T + b_hh_g            (M = rows,   N = H, K = H)
 *
 * `gate` selects the r / z / n slice of the [3H, .] weight and [3H] bias.
 * The tiling describes a single core-sized window (host generated it with
 * dim = 1); each AI core takes a contiguous run of `tileM` rows and supplies its
 * own GM offsets, mirroring the hand-split matmuls used elsewhere in this repo.
 */

#include "lib/matmul_intf.h"

#include "gru_common.h"

using namespace AscendC;
using namespace matmul;
using namespace gru_kernel;

namespace {

using aF = MatmulType<TPosition::GM, CubeFormat::ND, float>;
using bF = MatmulType<TPosition::GM, CubeFormat::ND, float>;
using cF = MatmulType<TPosition::GM, CubeFormat::ND, float>;
using GemmF = MatmulImpl<aF, bF, cF>;

__aicore__ inline void GruGemmBody(GM_ADDR a_, GM_ADDR b_, GM_ADDR bias_, GM_ADDR c_,
                                   GM_ADDR tilingGM, int64_t sel, int64_t gate)
{
    GruTilingData td;
    CopyTilingToLocal(&td, tilingGM);
    const AscendC::tiling::TCubeTiling &tiling = (sel == 0) ? td.mmIn : td.mmRec;

    const int64_t tileM = (int64_t)tiling.M;
    const int64_t N = (int64_t)tiling.N;
    const int64_t K = (int64_t)tiling.Ka;
    const int64_t fullM = (sel == 0) ? td.chunkRows * td.S : td.chunkRows;
    const int64_t mIdx = (int64_t)GetBlockIdx();

    int64_t tailM = fullM - mIdx * tileM;
    if (tailM <= 0) {
        return;
    }
    if (tailM > tileM) {
        tailM = tileM;
    }

    const int64_t offsetA = mIdx * K * tileM;  // A is [fullM, K] row-major
    const int64_t offsetC = mIdx * N * tileM;  // C is [fullM, N] row-major

    GlobalTensor<float> aG;
    aG.SetGlobalBuffer((__gm__ float *)a_ + offsetA);
    GlobalTensor<float> bG;
    bG.SetGlobalBuffer((__gm__ float *)b_ + gate * N * K);  // [N, K] row-major (transposed)
    GlobalTensor<float> biasG;
    biasG.SetGlobalBuffer((__gm__ float *)bias_ + gate * N);
    GlobalTensor<float> cG;
    cG.SetGlobalBuffer((__gm__ float *)c_ + offsetC);

    TPipe pipe;
    GemmF mm;
    mm.Init(&tiling, &pipe);
    mm.SetOrgShape((int32_t)tailM, (int32_t)N, (int32_t)K);
    mm.SetSingleShape((int32_t)tailM, (int32_t)N, (int32_t)K);
    mm.SetTensorA(aG, false);
    mm.SetTensorB(bG, true);
    if (tiling.isBias) {
        mm.SetBias(biasG);
    }
    mm.IterateAll(cG);
    mm.End();
}

}  // namespace

extern "C" __global__ __aicore__ void gru_gemm(GM_ADDR a, GM_ADDR b, GM_ADDR bias, GM_ADDR c,
                                               GM_ADDR tilingGM, int64_t sel, int64_t gate)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIC_ONLY);
    if ASCEND_IS_AIC {
        GruGemmBody(a, b, bias, c, tilingGM, sel, gate);
    }
}
