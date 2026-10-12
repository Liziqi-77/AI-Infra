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
 * \file gru_tiling_data.h
 * \brief Host/device shared tiling blob for the fused GRU operator.
 *
 * The blob is passed by the host as a GM tensor and memcpy'd word-wise into a
 * local POD struct on device (see CopyTilingToLocal in gru.cpp), so the layout
 * is kept 8-byte packed and int64-heavy.
 */

#ifndef GRU_TILING_DATA_H
#define GRU_TILING_DATA_H

// Device (ascendc compiler) resolves kernel_tiling/kernel_tiling.h from the
// ascendc highlevel_api tree; host (plain g++) resolves adv_api/kernel_tiling.h.
// Both expose an identical AscendC::tiling::TCubeTiling POD.
#if defined(__CCE_AICORE__) || defined(__NPU_ARCH__) || defined(__DAV_C220_CUBE__) || defined(__DAV_C220_VEC__)
#include "kernel_tiling/kernel_tiling.h"
#else
#include "adv_api/kernel_tiling.h"
#endif

namespace Gru {

constexpr uint64_t STRUCT_ALIGNAS = 8;

#pragma pack(push, 8)
struct alignas(STRUCT_ALIGNAS) GruTilingData {
    int64_t B;         // batch (full problem)
    int64_t S;         // sequence length
    int64_t I;         // input_size
    int64_t H;         // hidden_size
    int64_t chunkRows; // batch rows handled by this launch (B when unchunked)
    int64_t aicNum;
    int64_t aivNum;
    int64_t elemRows;  // rows per UB tile in the recurrent cell kernel
    AscendC::tiling::TCubeTiling mmIn;   // GI = xf @ w_ih^T + b_ih   (M=chunkRows*S, N=H, K=I)
    AscendC::tiling::TCubeTiling mmRec;  // GH = h  @ w_hh^T + b_hh   (M=chunkRows,   N=H, K=H)
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Backward tiling blob (training path).
// ---------------------------------------------------------------------------

// Slot indices of the five backward GEMMs inside GruBwdTilingData::mm / fullM.
constexpr int64_t BWD_GEMM_GI = 0;  // GI    = x_flat  @ w_ih^T + b_ih (M=rows*S, N=3H, K=I)
constexpr int64_t BWD_GEMM_GH = 1;  // GH    = hp_flat @ w_hh^T + b_hh (M=rows*S, N=3H, K=H)
constexpr int64_t BWD_GEMM_DH = 2;  // dhb   = dgh_t   @ w_hh          (M=rows,   N=H,  K=3H)
constexpr int64_t BWD_GEMM_WI = 3;  // dW_ih = dGx^T   @ x_flat        (M=3H,     N=I,  K=rows*S)
constexpr int64_t BWD_GEMM_WH = 4;  // dW_hh = dGh^T   @ hp_flat       (M=3H,     N=H,  K=rows*S)
// Same GEMM as BWD_GEMM_DH but with the B operand supplied pre-transposed
// ([N, K] = w_hh^T instead of [K, N] = w_hh) so the cube takes its transposed-B
// path. Separate slot because transB is a compile-time template argument.
constexpr int64_t BWD_GEMM_DH_T = 5;  // dhb = dgh_t @ w_hh^T^T  (B in [N, K] layout)
constexpr int64_t BWD_GEMM_COUNT = 6;

#pragma pack(push, 8)
struct alignas(STRUCT_ALIGNAS) GruBwdTilingData {
    int64_t B;         // batch (full problem)
    int64_t S;         // sequence length
    int64_t I;         // input_size
    int64_t H;         // hidden_size
    int64_t chunkRows; // batch rows handled by this launch
    int64_t aicNum;
    int64_t aivNum;
    int64_t fullM[BWD_GEMM_COUNT];          // logical M of each backward GEMM
    // Number of K-way splits used for the weight-gradient GEMMs (WI/WH slot):
    //   > 0 -> split-K: every block owns a K-slab, M is the whole 3H, and each
    //          block writes a distinct [M, N] partial that the host reduces.
    //   == 0 -> split-M: the legacy layout (block index partitions M).
    int64_t wKsplit;
    AscendC::tiling::TCubeTiling mm[BWD_GEMM_COUNT];
};
#pragma pack(pop)

}  // namespace Gru

#endif  // GRU_TILING_DATA_H
