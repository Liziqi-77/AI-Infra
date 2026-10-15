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
"""Run the generated AscendC `hwm` operator on real NPU hardware, driven by the model's real data.

This reproduces **Part IV "GPU Simulation Output"** of
``GPU_Test/callablecln.cpp :: CallableCLN::BOC_MTM_Init_GPU_1()``.

In the original framework that section calls ``HWMCKernel`` (``GPU_Test/kernel.cu``) to fill
``h_rate[NPath * MStep]`` and then prints ``rate[i][j]`` together with
``rate_adj[i][j] = rate[i][j] + CS`` in ``std::cout<<setw(12)<<fixed<<setprecision(8)`` style.
The framework itself needs CUDA + QuantLib, neither of which exists on this machine, so the
substitution is: feed the same real inputs (extracted verbatim from the framework) into the
generated AscendC operator, execute it on the NPU, and compare the result against the two
baselines recorded in the case file:

* ``expected_output.rate``            - the kernel output reconstructed to full precision
* ``reference.rate_from_console_dump``- the framework's own console dump (Output_3), ~8 digits
* ``reference.rate_max_abs_diff_vs_dump = 7.21574623940846e-08``
                                      - the framework's own disagreement between the two,
                                        i.e. the floor this comparison can never beat

Usage::

    ASCEND_RT_VISIBLE_DEVICES=3 python3 run_in_model.py
    ASCEND_RT_VISIBLE_DEVICES=3 python3 run_in_model.py --device 3 --case /path/to/kernel_case.json

Exit code 0 = PASS, 1 = FAIL.
"""

import argparse
import json
import os
import sys
from pathlib import Path

# Must be set before torch_npu initialises the device.
_DEFAULT_DEVICE = os.environ.get("ASCEND_RT_VISIBLE_DEVICES", "3").split(",")[0] or "3"
os.environ.setdefault("ASCEND_RT_VISIBLE_DEVICES", _DEFAULT_DEVICE)

import torch  # noqa: E402  (after the device env var)

HERE = Path(__file__).resolve().parent

# The case file lives in NPU_Test/ next to the operator directory; the copy inside this
# directory is byte-identical and is used as a fallback so the runner stays self-contained.
_CASE_CANDIDATES = [
    HERE.parent / "kernel_case.json",
    HERE / "kernel_case.json",
]

# Comparison threshold. The framework's own two baselines disagree by 7.2e-8 (the console dump
# only carries ~8 significant digits), so anything at 1e-6 or below is indistinguishable from it.
TOL = 1e-6

# Credit spread applied by the framework when building rate_adj.
CS = 0.01687

N_PATH = 16
M_STEP = 10


def _resolve_case(explicit):
    if explicit:
        p = Path(explicit).expanduser().resolve()
        if not p.is_file():
            raise SystemExit(f"[FAIL] case file not found: {p}")
        return p
    for cand in _CASE_CANDIDATES:
        if cand.is_file():
            return cand
    raise SystemExit(
        "[FAIL] no kernel_case.json found; looked in: "
        + ", ".join(str(c) for c in _CASE_CANDIDATES)
    )


def _load_case(path):
    with open(path, "r") as f:
        return json.load(f)


def _load_model():
    """Load ModelNew from model_new_ascendc.py, reusing its registration path.

    model_new_ascendc.py imports the `hwm` wheel if installed and otherwise falls back to
    `torch.ops.load_library()` on `kernel/build/hwm_ext*.so`. Reusing it keeps this runner on
    exactly the loading mechanism the operator was verified with.
    """
    sys.path.insert(0, str(HERE))
    import model_new_ascendc

    return model_new_ascendc


def _matrix_to_list(t):
    return [[float(v) for v in row] for row in t.tolist()]


def _max_abs_diff(a, b):
    worst = 0.0
    count_over = 0
    worst_pos = (0, 0)
    for i in range(len(a)):
        for j in range(len(a[i])):
            d = abs(a[i][j] - b[i][j])
            if d > worst:
                worst = d
                worst_pos = (i, j)
            if d > TOL:
                count_over += 1
    return worst, count_over, worst_pos


def _print_part_iv(rate, rate_adj, hwparams):
    r0, alpha, sigma = hwparams
    print()
    print("==================================")
    print("Part IV GPU Simulation Output")
    print("---------------------------")
    print("HW Simulation results:")
    print("  Start Date: (from framework: 2024/07/24), 1Y Later: (from framework)")
    print(f"  Initial Short Rate(12M):  {r0}, ")
    print(f"  Alpha: {alpha}, ")
    print(f"  Sigma: {sigma}, ")
    print(f"  Credit Spread: {CS}")
    print()
    print("-" * 70)
    for i in range(N_PATH):
        print(f"Path: {i:2d}:" + "".join(f"{v:12.8f}" for v in rate[i]))
    print()
    print("-" * 70)
    for i in range(N_PATH):
        print(f"Path: {i:2d}:" + "".join(f"{v:12.8f}" for v in rate_adj[i]))
    print()


def main():
    global TOL

    ap = argparse.ArgumentParser(description="Run torch.ops.npu.hwm on real NPU and compare to the framework baselines.")
    ap.add_argument("--case", default=None, help="path to kernel_case.json (default: NPU_Test/kernel_case.json)")
    ap.add_argument("--device", default=None, help="NPU device id (default: $ASCEND_RT_VISIBLE_DEVICES or 3)")
    ap.add_argument("--tol", type=float, default=TOL, help=f"pass threshold (default {TOL})")
    args = ap.parse_args()

    TOL = args.tol

    if args.device is not None:
        os.environ["ASCEND_RT_VISIBLE_DEVICES"] = str(args.device)
    device_id = os.environ["ASCEND_RT_VISIBLE_DEVICES"]

    case_path = _resolve_case(args.case)
    case = _load_case(case_path)
    inputs = case["inputs"]

    print("=" * 70)
    print("hwm on-device run  (model Part IV: GPU Simulation Output)")
    print("=" * 70)
    print(f"case file     : {case_path}")
    print(f"case id       : {case.get('case_id')}")
    print(f"device        : npu:{device_id}  (ASCEND_RT_VISIBLE_DEVICES={device_id})")

    model_mod = _load_model()
    model = model_mod.ModelNew()

    step = torch.tensor(inputs["step"], dtype=torch.int32).npu()
    hwparams = torch.tensor(inputs["hwparams"], dtype=torch.float32).npu()
    accrual = torch.tensor(inputs["accrual"], dtype=torch.float32).npu()
    f1 = torch.tensor(inputs["f1"], dtype=torch.float32).npu()
    f2 = torch.tensor(inputs["f2"], dtype=torch.float32).npu()
    nrnd = torch.tensor(inputs["nrnd"], dtype=torch.float32).npu()

    print(f"NPath         : {nrnd.shape[0]}   MStep: {accrual.shape[0]}")
    print(f"hwparams      : r0={inputs['hwparams'][0]}  alpha={inputs['hwparams'][1]}  sigma={inputs['hwparams'][2]}")

    with torch.no_grad():
        out = model(step, hwparams, accrual, f1, f2, nrnd)
    torch.npu.synchronize()

    if tuple(out.shape) != (N_PATH, M_STEP):
        print(f"[FAIL] unexpected output shape {tuple(out.shape)}, expected {(N_PATH, M_STEP)}")
        return 1

    # rate_adj is a plain scalar shift performed by the framework on the host; do it on the CPU
    # so that no extra torch compute op is involved.
    rate = _matrix_to_list(out.float().cpu())
    rate_adj = [[v + CS for v in row] for row in rate]

    expected = case["expected_output"]["rate"]
    dump = case["reference"]["rate_from_console_dump"]
    dump_declared_diff = case["reference"]["rate_max_abs_diff_vs_dump"]

    _print_part_iv(rate, rate_adj, inputs["hwparams"])

    e_max, e_over, e_pos = _max_abs_diff(rate, expected)
    d_max, d_over, d_pos = _max_abs_diff(rate, dump)
    b_max, b_over, b_pos = _max_abs_diff(expected, dump)

    print("=" * 70)
    print("Comparison")
    print("=" * 70)
    print(f"[1] AscendC rate  vs expected_output.rate (full precision)")
    print(f"      max_abs_error = {e_max:.6e}   elements over tol({TOL:g}) = {e_over}/{N_PATH * M_STEP}")
    print(f"[2] AscendC rate  vs reference.rate_from_console_dump (framework Output_3, ~8 digits)")
    print(f"      max_abs_error = {d_max:.6e}   elements over tol({TOL:g}) = {d_over}/{N_PATH * M_STEP}")
    print(f"[3] framework self-consistency: expected_output.rate vs console dump")
    print(f"      max_abs_error = {b_max:.6e}   (declared in case file: {dump_declared_diff:.6e})")
    print()

    worst = max(e_max, d_max)
    ok = worst <= TOL
    if not ok:
        print("Worst offending elements (AscendC vs baseline):")
        for label, a, b, pos in (("expected", rate, expected, e_pos), ("dump", rate, dump, d_pos)):
            i, j = pos
            print(f"  vs {label}: rate[{i}][{j}] = {a[i][j]:.10f}, baseline = {b[i][j]:.10f}, "
                  f"diff = {abs(a[i][j] - b[i][j]):.6e}")

    print(f"Result: {'PASS' if ok else 'FAIL'}   (threshold {TOL:g}; "
          f"framework's own baseline disagreement {dump_declared_diff:.6e})")
    print("=" * 70)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
