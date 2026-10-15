#!/usr/bin/env python3
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

"""TileLang entry point for the fused single-layer GRU (ascendc backend).

Design: `design/block_level/gru.py` (grid / scope partition) and
`design/tile_level/gru.py` (buffers, copies, gate chain).

    for each batch chunk:
        xf = cast(x_chunk)
        GI = input_projection(xf)
        h  = 0
        for t in range(S):
            GH = recurrent_projection(h)
            h, out[:, t, :] = cell(GI[:, t], GH, h)

`forward` only validates, allocates and dispatches; the launch schedule lives in
`_launch_gru`, whose loops iterate over *kernel launches* (batch chunks, gates,
timesteps), never over tensor elements -- all arithmetic is inside the TileLang
kernels below.  `Model.gru` is a parameter container built with the reference's
deterministic seed, so both sides run on identical weights.
"""

import sys
from pathlib import Path

import torch
import torch.nn as nn

sys.path.insert(0, str(Path(__file__).resolve().parent))
from design.tile_level.gru import (  # noqa: E402
    GruConfig,
    gru_cast_fwd,
    gru_cell_fwd,
    gru_input_gemm_fwd,
    gru_recurrent_gemm_fwd,
)

BATCH_CHUNK = 1024
ALIGNED_H = 16
CORE_NUM = 20

# (kind, key) -> compiled kernel, process-wide
_KERNEL_CACHE: dict = {}


def _plan_core_tile(full_m, core_num):
    """16-aligned M window per core (mirrors the host `PlanCoreTile`)."""
    tile_m = -(-full_m // core_num)
    tile_m = -(-tile_m // ALIGNED_H) * ALIGNED_H
    return max(tile_m, ALIGNED_H)


def _launch_gru(model, x_half, xf_all, out):
    """Walk the batch chunks and the timesteps, launching one kernel per stage."""
    batch, seq, input_size = x_half.shape
    hidden = model.hidden_size
    out_dtype = "float16"

    for b0 in range(0, batch, BATCH_CHUNK):
        rows = min(BATCH_CHUNK, batch - b0)
        x_chunk = x_half[b0:b0 + rows]
        xf = xf_all[b0:b0 + rows]

        key = ("cast", rows * seq * input_size)
        cast = _KERNEL_CACHE.get(key)
        if cast is None:
            cast = gru_cast_fwd(rows * seq * input_size, out_dtype)
            _KERNEL_CACHE[key] = cast
        cast(x_chunk.reshape(-1), xf.reshape(-1))

        tile_in = _plan_core_tile(rows * seq, CORE_NUM)
        tile_rec = _plan_core_tile(rows, CORE_NUM)
        cfg_in = GruConfig(rows=rows, seq=seq, input_size=input_size,
                           hidden_size=hidden, tile_m=tile_in, dtype=out_dtype)
        cfg_rec = GruConfig(rows=rows, seq=seq, input_size=input_size,
                            hidden_size=hidden, tile_m=tile_rec, dtype=out_dtype)

        gi = torch.empty((3, rows * seq, hidden), dtype=torch.float32, device=x_half.device)
        gh = torch.empty((3, rows, hidden), dtype=torch.float32, device=x_half.device)
        h_chunk = torch.zeros((rows, hidden), dtype=torch.float32, device=x_half.device)
        out_chunk = out[b0:b0 + rows]

        # --- input projection, once per chunk ------------------------------
        xf_2d = xf.reshape(rows * seq, input_size)
        for gate in range(3):
            lo, hi = gate * hidden, (gate + 1) * hidden
            key = ("in", rows, seq, tile_in)
            gemm_in = _KERNEL_CACHE.get(key)
            if gemm_in is None:
                gemm_in = gru_input_gemm_fwd(cfg_in)
                _KERNEL_CACHE[key] = gemm_in
            gemm_in(xf_2d,
                    model.gru.weight_ih_l0[lo:hi],
                    model.gru.bias_ih_l0[lo:hi],
                    gi[gate])

        # --- recurrence ----------------------------------------------------
        for t in range(seq):
            for gate in range(3):
                lo, hi = gate * hidden, (gate + 1) * hidden
                key = ("rec", rows, tile_rec)
                gemm_rec = _KERNEL_CACHE.get(key)
                if gemm_rec is None:
                    gemm_rec = gru_recurrent_gemm_fwd(cfg_rec)
                    _KERNEL_CACHE[key] = gemm_rec
                gemm_rec(h_chunk,
                         model.gru.weight_hh_l0[lo:hi],
                         model.gru.bias_hh_l0[lo:hi],
                         gh[gate])

            key = ("cell", rows, seq, hidden, t)
            cell = _KERNEL_CACHE.get(key)
            if cell is None:
                cfg_cell = GruConfig(rows=rows, seq=seq, input_size=hidden,
                                     hidden_size=hidden, tile_m=ALIGNED_H,
                                     dtype=out_dtype, out_dtype=out_dtype)
                cell = gru_cell_fwd(cfg_cell, t)
                _KERNEL_CACHE[key] = cell
            cell(gi[0], gi[1], gi[2], gh[0], gh[1], gh[2], h_chunk, out_chunk)


class ModelNew(nn.Module):
    def __init__(self, input_size, hidden_size, num_layers=1, bias=True, batch_first=True):
        super().__init__()
        if not batch_first:
            raise ValueError("the fused kernel only implements batch_first=True")
        if not bias:
            raise ValueError("the fused kernel requires bias=True")
        if num_layers != 1:
            raise ValueError("the fused kernel only implements num_layers=1")
        self.input_size = input_size
        self.hidden_size = hidden_size
        # Parameter container only; same deterministic init as the reference.
        torch.manual_seed(0)
        self.gru = nn.GRU(input_size=input_size, hidden_size=hidden_size,
                          num_layers=num_layers, bias=bias, batch_first=batch_first).half()

    def forward(self, x: torch.Tensor, h0: torch.Tensor = None) -> torch.Tensor:
        if h0 is not None:
            raise ValueError("the fused kernel assumes a zero initial hidden state")
        if x.dim() != 3:
            raise ValueError("x must be [batch, seq, input_size]")

        x_half = x if x.dtype == torch.float16 else x.to(torch.float16)
        xf_all = torch.empty(x_half.shape, dtype=torch.float32, device=x_half.device)
        out = torch.empty((x_half.shape[0], x_half.shape[1], self.hidden_size),
                          dtype=torch.float16, device=x_half.device)

        _launch_gru(self, x_half, xf_all, out)
        return out


def get_init_inputs():
    return [128, 512, 1, True, True]
