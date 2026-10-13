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
 * \file gru_bwd_pipe.cpp
 * \brief Cube+vector fused entry point for the GRU backward reverse sweep.
 *
 * The reverse sweep is an alternating chain  V(t) -> G(t) -> V(t-1) -> ...
 * where V is the vector cell and G is the cube dhb GEMM. Within one batch chunk
 * the two engines are strictly serial, so the legacy schedule (one AIV launch,
 * then one AIC launch, 2S-1 times per chunk) leaves cube and vector idle half
 * the time each.
 *
 * This entry fuses a V from one chunk with a G from a *different* chunk into a
 * single MIX_AIC_1_2 task. The two ops touch disjoint buffers, so no
 * cross-engine ordering is needed inside the launch; the host simply emits them
 * in launch order.
 *
 * Launch sizing: MIX_AIC_1_2 with blockDim = aicNum yields exactly aicNum AIC
 * blocks and 2*aicNum AIV blocks, which is what BwdCellBody (aivNum blocks) and
 * BwdGemmBody (<= aicNum blocks) expect.
 */

#include "gru_bwd_bodies.h"

extern "C" __global__ __aicore__ void gru_bwd_pipe(
    // ---- vector side: one reverse cell step ---------------------------------
    GM_ADDR cGi, GM_ADDR cGh, GM_ADDR cHp, GM_ADDR cDy, GM_ADDR cDha, GM_ADDR cDhb,
    GM_ADDR cDgx, GM_ADDR cDgh,
    // ---- cube side: dhb = dgh_prev @ w_hh ------------------------------------
    GM_ADDR gA, GM_ADDR gB, GM_ADDR gBias, GM_ADDR gC,
    // ---- control -------------------------------------------------------------
    GM_ADDR tilingC, GM_ADDR tilingG, int64_t selDH, int64_t tCell, int64_t doCell,
    int64_t doGemm)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_MIX_AIC_1_2);

    if ASCEND_IS_AIC {
        if (doGemm != 0) {
            BwdGemmBody<false, false>(gA, gB, gBias, gC, tilingG, selDH);
        }
    }
    if ASCEND_IS_AIV {
        if (doCell != 0) {
            BwdCellBody(cGi, cGh, cHp, cDy, cDha, cDhb, cDgx, cDgh, tilingC, tCell);
        }
    }
}
