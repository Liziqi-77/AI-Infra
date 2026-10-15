#!/usr/bin/env python3
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""AscendC-backed Hull-White short-rate Monte-Carlo path simulation.

The whole path recursion -- per-step coefficient derivation, the sequential
short-rate scan over the time axis, and the output emission -- is fused into a
single registered operator,

    torch.ops.npu.hwm(step, hwparams, accrual, f1, f2, nrnd) -> rate

Only tensor creation, tensor movement and the call into that operator happen in
Python.
"""

import glob
import json
import os

import torch
import torch.nn as nn

CASE_JSON = "kernel_case.json"

try:
    import hwm as _hwm  # noqa: F401  (importing registers torch.ops.npu.hwm)
except ImportError:  # pragma: no cover - fallback for an in-tree, uninstalled build
    _BUILD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "kernel", "build")
    _libs = glob.glob(os.path.join(_BUILD, "hwm_ext*.so"))
    if not _libs:
        raise
    torch.ops.load_library(_libs[0])


class ModelNew(nn.Module):
    """Hull-White short-rate path simulation whose recurrence runs on AI cores."""

    def __init__(self):
        super(ModelNew, self).__init__()

    def forward(self, step, hwparams, accrual, f1, f2, nrnd):
        return torch.ops.npu.hwm(step, hwparams, accrual, f1, f2, nrnd)


def get_input_groups():
    json_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), CASE_JSON)
    with open(json_path, "r") as f:
        case = json.load(f)

    inputs = case["inputs"]
    step = torch.tensor(inputs["step"], dtype=torch.int32)
    hwparams = torch.tensor(inputs["hwparams"], dtype=torch.float32)
    accrual = torch.tensor(inputs["accrual"], dtype=torch.float32)
    f1 = torch.tensor(inputs["f1"], dtype=torch.float32)
    f2 = torch.tensor(inputs["f2"], dtype=torch.float32)
    nrnd = torch.tensor(inputs["nrnd"], dtype=torch.float32)
    return [[step, hwparams, accrual, f1, f2, nrnd]]


def get_init_inputs():
    return []


if __name__ == "__main__":
    m = ModelNew()
    for group in get_input_groups():
        out = m(*[t.npu() for t in group])
        print("rate", tuple(out.shape), out.dtype)
        print(out[0, :4])
