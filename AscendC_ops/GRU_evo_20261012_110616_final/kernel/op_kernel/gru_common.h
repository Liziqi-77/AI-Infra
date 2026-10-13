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
 * \file gru_common.h
 * \brief Shared declarations for the fused GRU device kernels.
 *
 * The CANN kernel build assigns a core type (AIV / AIC) per translation unit, so
 * the vector kernels and the cube GEMM live in separate .cpp files
 * (gru_vector.cpp / gru_cube.cpp) and only meet here.
 */

#ifndef GRU_KERNEL_COMMON_H
#define GRU_KERNEL_COMMON_H

#include "kernel_operator.h"

#include "gru_tiling_data.h"

namespace gru_kernel {

using namespace AscendC;
using namespace Gru;

// The tiling blob is handed over as an int64-granular GM buffer and copied
// word-wise, so the device ABI never depends on packed struct layout.
__aicore__ inline void CopyTilingToLocal(GruTilingData *tiling, GM_ADDR tilingGM)
{
    auto *dst = reinterpret_cast<int64_t *>(tiling);
    auto *src = reinterpret_cast<__gm__ int64_t *>(tilingGM);
    for (uint64_t i = 0; i < sizeof(GruTilingData) / sizeof(int64_t); ++i) {
        dst[i] = src[i];
    }
}

// Same transport for the backward blob. `alignas(8)` on the struct makes
// sizeof() a multiple of 8, so the word-wise count is exact.
__aicore__ inline void CopyBwdTilingToLocal(GruBwdTilingData *tiling, GM_ADDR tilingGM)
{
    auto *dst = reinterpret_cast<int64_t *>(tiling);
    auto *src = reinterpret_cast<__gm__ int64_t *>(tilingGM);
    for (uint64_t i = 0; i < sizeof(GruBwdTilingData) / sizeof(int64_t); ++i) {
        dst[i] = src[i];
    }
}

}  // namespace gru_kernel

#endif  // GRU_KERNEL_COMMON_H
