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
 * \brief Host side of the Hull-White short-rate Monte-Carlo path simulation:
 *        tiling (core split over the path axis) + a single kernel launch.
 *
 * The operator is a pure per-path sequential scan of length mStep-1, so the only
 * parallel axis is the path index.  Each AI core owns a contiguous run of paths
 * and performs the whole time recursion for them; no cross-core communication
 * and no workspace are needed.  The run length is chosen so that a core never
 * receives fewer than SLOT_BATCH paths, which keeps the per-core coefficient
 * prelude (a dozen vector primitives) from being duplicated across the grid for
 * a problem whose real work is a handful of micro-seconds.
 */

#include <cstdint>

#include <torch/extension.h>
#include <torch/library.h>

#include "torch_kernel_helper.h"
#include "tiling/platform/platform_ascendc.h"

#include "aclrtlaunch_hwm_kernel.h"

#include "ops.h"

namespace ascend_kernel {

namespace {

// Paths per core.  A core's UB state buffer is one 32-byte slot per owned path,
// so this bounds the UB footprint at 32 * SLOT_BATCH bytes of live state.
constexpr int64_t SLOT_BATCH = 16;

}  // namespace

at::Tensor hwm(const at::Tensor &step, const at::Tensor &hwparams, const at::Tensor &accrual,
               const at::Tensor &f1, const at::Tensor &f2, const at::Tensor &nrnd)
{
    // Keep the inputs alive and contiguous for the duration of the launch.
    const at::Tensor hw = hwparams.contiguous();
    const at::Tensor acc = accrual.contiguous();
    const at::Tensor fw1 = f1.contiguous();
    const at::Tensor fw2 = f2.contiguous();
    const at::Tensor rnd = nrnd.contiguous();

    TORCH_CHECK(hw.scalar_type() == at::kFloat, "hwm: hwparams must be float32");
    TORCH_CHECK(acc.scalar_type() == at::kFloat, "hwm: accrual must be float32");
    TORCH_CHECK(fw1.scalar_type() == at::kFloat, "hwm: f1 must be float32");
    TORCH_CHECK(fw2.scalar_type() == at::kFloat, "hwm: f2 must be float32");
    TORCH_CHECK(rnd.scalar_type() == at::kFloat, "hwm: nrnd must be float32");
    TORCH_CHECK(hw.dim() == 1 && hw.numel() == 3, "hwm: hwparams must have 3 elements");
    TORCH_CHECK(rnd.dim() == 2, "hwm: nrnd must be 2-D [nPath, mStep-1]");

    // The time grid length is host-visible metadata, so the output shape needs
    // no device-to-host read of `step`; `step` is the same quantity the CUDA
    // kernel reads back from the device and is accepted here for signature
    // fidelity only.
    const int64_t mStep = acc.numel();
    const int64_t nPath = rnd.size(0);
    const int64_t nCol = rnd.size(1);
    TORCH_CHECK(mStep >= 2, "hwm: mStep must be at least 2");
    TORCH_CHECK(nCol == mStep - 1, "hwm: nrnd must be [nPath, mStep-1]");
    TORCH_CHECK(fw1.numel() == nCol && fw2.numel() == nCol, "hwm: f1/f2 must have mStep-1 elements");
    TORCH_CHECK(step.numel() >= 1, "hwm: step must be non-empty");

    at::Tensor out = at::empty({nPath, mStep}, rnd.options());

    auto *plat = platform_ascendc::PlatformAscendCManager::GetInstance();
    TORCH_CHECK(plat != nullptr, "hwm: failed to obtain the Ascend platform");
    int64_t aivNum = static_cast<int64_t>(plat->GetCoreNumAiv());
    if (aivNum < 1) {
        aivNum = 1;
    }

    int64_t pathsPerCore = (nPath + aivNum - 1) / aivNum;
    if (pathsPerCore < SLOT_BATCH) {
        pathsPerCore = SLOT_BATCH;
    }
    int64_t blockDim = (nPath + pathsPerCore - 1) / pathsPerCore;
    if (blockDim < 1) {
        blockDim = 1;
    }
    TORCH_CHECK(blockDim <= 65535, "hwm: blockDim out of range");

    const uint32_t blk = static_cast<uint32_t>(blockDim);
    EXEC_KERNEL_CMD(hwm_kernel, blk, hw, acc, fw1, fw2, rnd, out, nPath, mStep, pathsPerCore);
    return out;
}

}  // namespace ascend_kernel
