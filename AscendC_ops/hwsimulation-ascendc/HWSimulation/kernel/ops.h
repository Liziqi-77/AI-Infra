/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef HWM_OPS_H
#define HWM_OPS_H

#include <torch/extension.h>

namespace ascend_kernel {

// Hull-White short-rate Monte-Carlo path simulation (GPU_Test/kernel.cu :: HWMCKernel).
//
//   step     : int32 [1]        number of time steps mStep (signature fidelity;
//                               the host derives mStep from accrual.numel())
//   hwparams : float [3]        {r0, a (mean reversion), sigma (volatility)}
//   accrual  : float [mStep]    time grid, accrual[0] = 0
//   f1       : float [mStep-1]  instantaneous forward at accrual[j]
//   f2       : float [mStep-1]  instantaneous forward at accrual[j+1]
//   nrnd     : float [nPath, mStep-1]  standard-normal draws, row-major
//
// returns rate : float [nPath, mStep], row-major, rate[i*mStep + j]
//
//   rate[i][0]   = r0
//   rate[i][j+1] = rate[i][j]*exp(-a*dt) + alpha_2 - alpha_1*exp(-a*dt)
//                  + sqrt(0.5*sig^2/a*(1-exp(-2*a*dt))) * nrnd[i][j]
//   alpha_k      = 0.5*((sig/a)*(1-exp(-a*accrual[k])))^2 + f_[k][j]
//   dt           = accrual[j+1] - accrual[j]
//
// The whole recursion runs on the AI cores; every path is an independent scan,
// so the grid splits only over the path axis.
at::Tensor hwm(const at::Tensor &step, const at::Tensor &hwparams, const at::Tensor &accrual,
               const at::Tensor &f1, const at::Tensor &f2, const at::Tensor &nrnd);

}  // namespace ascend_kernel

#endif  // HWM_OPS_H
