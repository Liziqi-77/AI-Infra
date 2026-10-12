/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef GRU_OPS_H
#define GRU_OPS_H

#include <torch/extension.h>

#include <tuple>

namespace ascend_kernel {

// Fused single-layer GRU (batch_first=True).
//   x    : [B, S, I]  fp16 / bf16 / fp32
//   w_ih : [3H, I]    fp16 / bf16 / fp32   (gate order r, z, n along dim 0)
//   w_hh : [3H, H]    fp16 / bf16 / fp32
//   b_ih : [3H]       fp16 / bf16 / fp32
//   b_hh : [3H]       fp16 / bf16 / fp32
// returns out : [B, S, H] in the dtype of x (fp16 -> fp16, fp32 -> fp32), which
//               is what the reference module produces for that input dtype.
// All GEMM arithmetic runs in fp32 internally (cube GEMM, DT_FLOAT).
at::Tensor gru(const at::Tensor &x, const at::Tensor &w_ih, const at::Tensor &w_hh,
               const at::Tensor &b_ih, const at::Tensor &b_hh);

// Backward pass of the fused GRU (fp32 only).
//   grad_out : [B, S, H]  dL/dout
//   x        : [B, S, I]
//   out      : [B, S, H]  the forward result (reused as the BPTT hidden states)
//   w_ih     : [3H, I], w_hh : [3H, H], b_ih / b_hh : [3H]
// returns (dW_ih [3H, I], dW_hh [3H, H], db_ih [3H], db_hh [3H]).
// The gradient w.r.t. x is not produced: x is training data, never a parameter.
std::tuple<at::Tensor, at::Tensor, at::Tensor, at::Tensor>
gru_backward(const at::Tensor &grad_out, const at::Tensor &x, const at::Tensor &out,
             const at::Tensor &w_ih, const at::Tensor &w_hh,
             const at::Tensor &b_ih, const at::Tensor &b_hh);

}  // namespace ascend_kernel

#endif  // GRU_OPS_H
