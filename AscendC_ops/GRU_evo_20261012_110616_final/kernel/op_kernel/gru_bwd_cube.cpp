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
 * \file gru_bwd_cube.cpp
 * \brief Cube-side GEMM entry point for the GRU backward pass.
 *
 * One kernel covers all five backward GEMMs; `sel` picks the slot of the tiling
 * blob and, with it, the (transA, transB) pair the matmul object must be built
 * with:
 *
 *   sel 0  GI    = x_flat  @ w_ih^T + b_ih   A[M,K]  B[N,K]  transA=F transB=T  bias
 *   sel 1  GH    = hp_flat @ w_hh^T + b_hh   A[M,K]  B[N,K]  transA=F transB=T  bias
 *   sel 2  dhb   = dgh_t   @ w_hh            A[M,K]  B[K,N]  transA=F transB=F
 *   sel 3  dW_ih = dGx^T   @ x_flat          A[K,M]  B[K,N]  transA=T transB=F
 *   sel 4  dW_hh = dGh^T   @ hp_flat         A[K,M]  B[K,N]  transA=T transB=F
 *
 * transA / transB are compile-time: for a transposed A operand the MatmulType
 * ISTRANS template argument must agree with the runtime SetTensorA flag, so the
 * two A layouts live in distinct instantiations. The host generated every tiling
 * with dim = 1 (single-core window) and the kernel partitions the M extent across
 * AI cores by block index, exactly like the forward GEMM.
 */

#include <type_traits>

#include "lib/matmul_intf.h"

#include "gru_common.h"

using namespace AscendC;
using namespace matmul;
using namespace gru_kernel;

namespace {

// fp16 operands, fp32 accumulate -- same 11-bit mantissa rounding as HF32, but
// at the fp16 cube rate. See gru_cube.cpp.
using bF = MatmulType<TPosition::GM, CubeFormat::ND, half, false>;
using cFH = MatmulType<TPosition::GM, CubeFormat::ND, half, false>;
using cFF = MatmulType<TPosition::GM, CubeFormat::ND, float, false>;
// Every non-transposed-A output is written at fp16: GI/GH write the gate
// pre-activations the vector cells read back at fp16, and the transB=F flavour
// (used only by DH / DH_T) writes dhb, the recurrent hidden-gradient the cell
// reads back and re-narrows anyway. Only the transpose-A weight-gradient GEMMs
// (WI / WH) accumulate fp32 gradients. This matches the host tiling rule
// c16 = !transA in FillMatmulTilingEx.
template <bool TA, bool TB>
using cTypeOf = std::conditional_t<TA, cFF, cFH>;

// MatmulImpl defaults BiasT to CType; the bias is a small [3H] fp32 vector added
// inside the fp32 L0C accumulator, so pin it to fp32 explicitly.
using biasT = MatmulType<TPosition::GM, CubeFormat::ND, float>;

// A in [M, K] (ISTRANS = false) or [K, M] (ISTRANS = true).
template <bool TA>
struct ATypeOf;
template <>
struct ATypeOf<false> {
    using type = MatmulType<TPosition::GM, CubeFormat::ND, half, false>;
};
template <>
struct ATypeOf<true> {
    using type = MatmulType<TPosition::GM, CubeFormat::ND, half, true>;
};

template <bool TA, bool TB>
__aicore__ inline void BwdGemmBody(GM_ADDR a_, GM_ADDR b_, GM_ADDR bias_, GM_ADDR c_,
                                   GM_ADDR tilingGM, int64_t sel)
{
    GruBwdTilingData td;
    CopyBwdTilingToLocal(&td, tilingGM);
    const AscendC::tiling::TCubeTiling &tiling = td.mm[sel];

    const int64_t tileM = (int64_t)tiling.M;
    const int64_t N = (int64_t)tiling.N;
    const int64_t K = (int64_t)tiling.Ka;
    const int64_t fullM = td.fullM[sel];
    const int64_t mIdx = (int64_t)GetBlockIdx();

    int64_t tailM = fullM - mIdx * tileM;
    if (tailM <= 0) {
        return;
    }
    if (tailM > tileM) {
        tailM = tileM;
    }

    // A is [fullM, K] row-major when TA is false, so a core's M window is tileM
    // contiguous K-rows starting at mIdx * tileM * K. When TA is true A is
    // [K, fullM] and the M window is a column slice: the library takes A's row
    // stride from orgM, so orgM stays the FULL M and the window is selected by
    // offsetting the base pointer by mIdx * tileM elements (M is the minor axis).
    const int64_t offsetA = TA ? (mIdx * tileM) : (mIdx * K * tileM);
    const int64_t offsetC = mIdx * N * tileM;

    using aT = typename ATypeOf<TA>::type;
    using cT = cTypeOf<TA, TB>;
    using GemmF = MatmulImpl<aT, bF, cT, biasT>;
    using cgElem = std::conditional_t<TA, float, half>;

    GlobalTensor<half> aG;
    aG.SetGlobalBuffer((__gm__ half *)a_ + offsetA);
    GlobalTensor<half> bG;
    bG.SetGlobalBuffer((__gm__ half *)b_);
    GlobalTensor<float> biasG;
    biasG.SetGlobalBuffer((__gm__ float *)bias_);
    GlobalTensor<cgElem> cG;
    cG.SetGlobalBuffer((__gm__ cgElem *)c_ + offsetC);

    TPipe pipe;
    GemmF mm;
    mm.Init(&tiling, &pipe);
    mm.SetOrgShape((int32_t)(TA ? fullM : tailM), (int32_t)N, (int32_t)K);
    mm.SetSingleShape((int32_t)tailM, (int32_t)N, (int32_t)K);
    mm.SetTensorA(aG, TA);
    mm.SetTensorB(bG, TB);
    if (tiling.isBias) {
        mm.SetBias(biasG);
    }
    mm.IterateAll(cG);
    mm.End();
}

// Split-K flavour of the weight-gradient GEMM (A = [K, M], B = [K, N]).
//
// Block index picks a *K-slab* rather than an M-slab: core `i` contracts rows
// [i*tileK, (i+1)*tileK) of both A and B and writes a full [M, N] partial to its
// own C slot. Because every core reads a distinct slab, the B operand is read
// exactly once across the grid instead of once per M block, which is what the
// legacy M partition paid for (M = 3H = 1536 -> up to 12x B traffic). M stays
// whole, so the library's internal M loop keeps the cube busy; the host sums the
// per-slab partials afterwards.
template <bool TA>
__aicore__ inline void BwdGemmKSplitBody(GM_ADDR a_, GM_ADDR b_, GM_ADDR bias_, GM_ADDR c_,
                                         GM_ADDR tilingGM, int64_t sel)
{
    GruBwdTilingData td;
    CopyBwdTilingToLocal(&td, tilingGM);
    const AscendC::tiling::TCubeTiling &tiling = td.mm[sel];

    const int64_t M = (int64_t)tiling.M;      // whole 3H, not partitioned
    const int64_t N = (int64_t)tiling.N;
    const int64_t tileK = (int64_t)tiling.Ka;
    const int64_t fullK = td.chunkRows * td.S;
    const int64_t kIdx = (int64_t)GetBlockIdx();

    const int64_t k0 = kIdx * tileK;
    if (k0 >= fullK) {
        return;
    }
    int64_t tailK = fullK - k0;
    if (tailK > tileK) {
        tailK = tileK;
    }

    // A is [fullK, M] and B is [fullK, N], both row-major, so a K-slab is a
    // contiguous run of rows whose stride is the slab's column count. Both
    // operands therefore advance by k0 rows (M and N elements respectively).
    // C is written into this core's own [M, N] slot.
    const int64_t offsetA = k0 * M;
    const int64_t offsetB = k0 * N;
    const int64_t offsetC = kIdx * M * N;

    using aT = typename ATypeOf<TA>::type;
    using GemmF = MatmulImpl<aT, bF, cFF, biasT>;

    GlobalTensor<half> aG;
    aG.SetGlobalBuffer((__gm__ half *)a_ + offsetA);
    GlobalTensor<half> bG;
    bG.SetGlobalBuffer((__gm__ half *)b_ + offsetB);
    GlobalTensor<float> biasG;
    biasG.SetGlobalBuffer((__gm__ float *)bias_);
    GlobalTensor<float> cG;
    cG.SetGlobalBuffer((__gm__ float *)c_ + offsetC);

    TPipe pipe;
    GemmF mm;
    mm.Init(&tiling, &pipe);
    mm.SetOrgShape((int32_t)M, (int32_t)N, (int32_t)tailK);
    mm.SetSingleShape((int32_t)M, (int32_t)N, (int32_t)tailK);
    mm.SetTensorA(aG, TA);
    mm.SetTensorB(bG, false);
    if (tiling.isBias) {
        mm.SetBias(biasG);
    }
    mm.IterateAll(cG);
    mm.End();
}

}  // namespace

extern "C" __global__ __aicore__ void gru_bwd_gemm(GM_ADDR a, GM_ADDR b, GM_ADDR bias,
                                                   GM_ADDR c, GM_ADDR tilingGM, int64_t sel)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIC_ONLY);
    if ASCEND_IS_AIC {
        if (sel == Gru::BWD_GEMM_GI || sel == Gru::BWD_GEMM_GH ||
            sel == Gru::BWD_GEMM_DH_T) {
            BwdGemmBody<false, true>(a, b, bias, c, tilingGM, sel);
        } else if (sel == Gru::BWD_GEMM_DH) {
            BwdGemmBody<false, false>(a, b, bias, c, tilingGM, sel);
        } else if (sel == Gru::BWD_GEMM_WI || sel == Gru::BWD_GEMM_WH) {
            GruBwdTilingData td;
            CopyBwdTilingToLocal(&td, tilingGM);
            if (td.wKsplit > 0) {
                BwdGemmKSplitBody<true>(a, b, bias, c, tilingGM, sel);
            } else {
                BwdGemmBody<true, false>(a, b, bias, c, tilingGM, sel);
            }
        } else {
            BwdGemmBody<true, false>(a, b, bias, c, tilingGM, sel);
        }
    }
}
