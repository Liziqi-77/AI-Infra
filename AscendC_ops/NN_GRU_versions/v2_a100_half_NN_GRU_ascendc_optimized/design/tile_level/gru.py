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

"""Tile-level TileLang design for the fused single-layer GRU.

Fills in the TODO(tile-level) markers of `design/block_level/gru.py`.  This is
the design expression consumed by the AscendC translation step; the delivered
kernel is `kernel/op_kernel/gru_cube.cpp` + `kernel/op_kernel/gru_vector.cpp`.

Tiling summary (Atlas A2 / Ascend910B2C, DAV_2201, L1 512 KB, L0C 128 KB,
UB 192 KB, 20 AIC / 40 AIV):

  * Multi-core: M split of the GEMMs.  `tile_m = align16(ceil(M / aic_num))`,
    floored at 16; `n_blk = ceil(M / tile_m)`; each core owns a contiguous
    `tile_m`-row window plus its own GM offsets.  This is the "multi-core split
    extension" of the category guide, expressed on the host (`PlanCoreTile`).
  * UB split: the cell works on `cell_rows = 4` hidden rows per iteration
    (4 * 512 fp32 = 8 KB per buffer) because a full [rows, H] tile does not fit.
  * Buffers: 7 VECIN + 2 VECOUT queues plus a 6-slot VECCALC scratch, all sized
    `cell_rows * H` fp32; the cast kernel uses 2-deep VECIN/VECOUT double
    buffers of 4096 elements.
  * Branches: x dtype fp16 / bf16 / fp32 (cast kernel dispatches on a tag);
    output dtype fp16 (the NPU nn.GRU reference is fp16-only) with an fp32
    specialisation for the copy-out.

Precision: the whole recurrence runs in fp32; only the input/output tensors are
half precision.  sigmoid / tanh are built from `exp` + `reciprocal` only, so no
auxiliary high-level-API tmp buffer is needed and the vector chain stays on one
pipe (`v` -> `v`), which is why no explicit intra-kernel barrier appears below:

    sigmoid(x) = 1 / (1 + exp(-x))
    tanh(x)    = 1 - 2 / (1 + exp(2x))
"""

from dataclasses import dataclass

import tilelang
from tilelang import language as T

ALIGNED_H = 16
CELL_ROWS = 4
VEC_CHUNK = 4096  # elements per cast-kernel DataCopy


@dataclass(frozen=True)
class GruConfig:
    rows: int          # batch rows handled by this launch
    seq: int
    input_size: int
    hidden_size: int
    tile_m: int        # 16-aligned M window per core
    dtype: str = "float16"
    out_dtype: str = "float16"


# ---------------------------------------------------------------------------
# 1) Input projection: GI = xf @ W_ih^T + b_ih      (Cube)
# ---------------------------------------------------------------------------
@tilelang.jit(out_idx=[3], target="ascendc")
def gru_input_gemm_fwd(cfg: GruConfig):
    M = cfg.rows * cfg.seq
    N = cfg.hidden_size
    K = cfg.input_size
    tile_m = cfg.tile_m
    dtype, accum_dtype = cfg.dtype, "float"
    n_blk = (M + tile_m - 1) // tile_m

    @T.prim_func
    def main(
        xf: T.Tensor((M, K), dtype),
        w_ih: T.Tensor((N, K), dtype),
        b_ih: T.Tensor((N,), accum_dtype),
        gi: T.Tensor((M, N), accum_dtype),
    ):
        with T.Kernel(n_blk, is_npu=True) as (cid, vid):
            m0 = cid * tile_m
            block_m = T.min(tile_m, M - m0)

            a_l1 = T.alloc_L1([tile_m, K], dtype)
            b_l1 = T.alloc_L1([N, K], dtype)          # transB: weight is [N, K]
            a_l0 = T.alloc_L0A([tile_m, K], dtype)
            b_l0 = T.alloc_L0B([K, N], dtype)
            c_l0 = T.alloc_L0C([tile_m, N], accum_dtype)

            with T.Scope("C"):
                T.copy(xf[m0:m0 + block_m, 0:K], a_l1[0:block_m, :])
                T.copy(w_ih, b_l1)
                T.copy(a_l1[0:block_m, :], a_l0[0:block_m, :])
                T.copy(b_l1, b_l0, transpose=True)
                # org == single == the per-core window; the host supplies the
                # per-core GM offsets, mirroring the hand-split matmuls used
                # elsewhere in this repo (dim = 1 in the host tiling).
                T.gemm(a_l0[0:block_m, :], b_l0, c_l0[0:block_m, :])
                T.copy(c_l0[0:block_m, :], gi[m0:m0 + block_m, :])

    return main


# ---------------------------------------------------------------------------
# 2) Recurrent projection: GH = h @ W_hh^T + b_hh   (Cube)
# ---------------------------------------------------------------------------
@tilelang.jit(out_idx=[3], target="ascendc")
def gru_recurrent_gemm_fwd(cfg: GruConfig):
    M = cfg.rows
    N = cfg.hidden_size
    K = cfg.hidden_size
    tile_m = cfg.tile_m
    dtype, accum_dtype = cfg.dtype, "float"
    n_blk = (M + tile_m - 1) // tile_m

    @T.prim_func
    def main(
        h: T.Tensor((M, K), dtype),
        w_hh: T.Tensor((N, K), dtype),
        b_hh: T.Tensor((N,), accum_dtype),
        gh: T.Tensor((M, N), accum_dtype),
    ):
        with T.Kernel(n_blk, is_npu=True) as (cid, vid):
            m0 = cid * tile_m
            block_m = T.min(tile_m, M - m0)

            a_l1 = T.alloc_L1([tile_m, K], dtype)
            b_l1 = T.alloc_L1([N, K], dtype)
            a_l0 = T.alloc_L0A([tile_m, K], dtype)
            b_l0 = T.alloc_L0B([K, N], dtype)
            c_l0 = T.alloc_L0C([tile_m, N], accum_dtype)

            with T.Scope("C"):
                T.copy(h[m0:m0 + block_m, 0:K], a_l1[0:block_m, :])
                T.copy(w_hh, b_l1)
                T.copy(a_l1[0:block_m, :], a_l0[0:block_m, :])
                T.copy(b_l1, b_l0, transpose=True)
                T.gemm(a_l0[0:block_m, :], b_l0, c_l0[0:block_m, :])
                T.copy(c_l0[0:block_m, :], gh[m0:m0 + block_m, :])

    return main


# ---------------------------------------------------------------------------
# 3) Cast x -> fp32                                (Vector)
# ---------------------------------------------------------------------------
@tilelang.jit(out_idx=[1], target="ascendc")
def gru_cast_fwd(total, dtype="float16"):
    @T.prim_func
    def main(
        src: T.Tensor((total,), dtype),
        dst: T.Tensor((total,), "float32"),
    ):
        with T.Kernel(20, is_npu=True) as (cid, vid):
            with T.Scope("V"):
                per_core = T.ceildiv(total + 20 * 15, 20)  # rounded, then aligned
                per_core = T.ceildiv(per_core, 16) * 16
                start = cid * per_core
                with T.If(start < total), T.Then():
                    cnt = T.min(per_core, total - start)
                    # VECIN/VECOUT double buffered in VEC_CHUNK element chunks;
                    # `T.copy` emits the cast when the dtypes differ.
                    T.copy(src[start:start + cnt], dst[start:start + cnt])
    return main


# ---------------------------------------------------------------------------
# 4) Recurrent cell                                (Vector)
# ---------------------------------------------------------------------------
@tilelang.jit(target="ascendc")
def gru_cell_fwd(cfg: GruConfig, t, cell_rows=CELL_ROWS):
    rows, seq, N = cfg.rows, cfg.seq, cfg.hidden_size
    out_dtype, accum_dtype = cfg.out_dtype, "float"

    @T.prim_func
    def main(
        gi_r: T.Tensor((rows, seq, N), accum_dtype),
        gi_z: T.Tensor((rows, seq, N), accum_dtype),
        gi_n: T.Tensor((rows, seq, N), accum_dtype),
        gh_r: T.Tensor((rows, N), accum_dtype),
        gh_z: T.Tensor((rows, N), accum_dtype),
        gh_n: T.Tensor((rows, N), accum_dtype),
        h: T.Tensor((rows, N), accum_dtype),
        out: T.Tensor((rows, seq, N), out_dtype),
    ):
        with T.Kernel(20, is_npu=True) as (cid, vid):
            rows_per_core = T.ceildiv(rows, 20)
            b0 = cid * rows_per_core
            with T.Scope("V"):
                with T.If(b0 < rows), T.Then():
                    n_row = T.min(rows_per_core, rows - b0)
                    for r0 in T.serial(T.ceildiv(n_row, cell_rows)):
                        n_r = T.min(cell_rows, n_row - r0 * cell_rows)
                        base = b0 + r0 * cell_rows

                        # step t of every `seq` rows -> strided copy
                        u_r = T.alloc_ub([cell_rows, N], accum_dtype)
                        u_z = T.alloc_ub([cell_rows, N], accum_dtype)
                        u_n = T.alloc_ub([cell_rows, N], accum_dtype)
                        g_r = T.alloc_ub([cell_rows, N], accum_dtype)
                        g_z = T.alloc_ub([cell_rows, N], accum_dtype)
                        g_n = T.alloc_ub([cell_rows, N], accum_dtype)
                        h_old = T.alloc_ub([cell_rows, N], accum_dtype)
                        h_new = T.alloc_ub([cell_rows, N], accum_dtype)
                        o_t = T.alloc_ub([cell_rows, N], out_dtype)

                        T.copy(gi_r[base:base + n_r, t, 0:N], u_r[0:n_r, :])
                        T.copy(gi_z[base:base + n_r, t, 0:N], u_z[0:n_r, :])
                        T.copy(gi_n[base:base + n_r, t, 0:N], u_n[0:n_r, :])
                        T.copy(gh_r[base:base + n_r, 0:N], g_r[0:n_r, :])
                        T.copy(gh_z[base:base + n_r, 0:N], g_z[0:n_r, :])
                        T.copy(gh_n[base:base + n_r, 0:N], g_n[0:n_r, :])
                        T.copy(h[base:base + n_r, 0:N], h_old[0:n_r, :])

                        # r = sigmoid(u_r + g_r)
                        for i, j in T.Parallel(cell_rows, N):
                            h_new[i, j] = 1.0 / (1.0 + T.exp(-(u_r[i, j] + g_r[i, j])))
                        # z = sigmoid(u_z + g_z)   (reuse u_z as the z slot)
                        for i, j in T.Parallel(cell_rows, N):
                            u_z[i, j] = 1.0 / (1.0 + T.exp(-(u_z[i, j] + g_z[i, j])))
                        # n = tanh(u_n + r * g_n)
                        for i, j in T.Parallel(cell_rows, N):
                            u_n[i, j] = 1.0 - 2.0 / (
                                1.0 + T.exp(2.0 * (u_n[i, j] + h_new[i, j] * g_n[i, j])))
                        # h_t = (1 - z) * n + z * h_{t-1}
                        for i, j in T.Parallel(cell_rows, N):
                            h_new[i, j] = (1.0 - u_z[i, j]) * u_n[i, j] \
                                + u_z[i, j] * h_old[i, j]

                        T.copy(h_new[0:n_r, :], h[base:base + n_r, 0:N])
                        T.copy(h_new[0:n_r, :], o_t[0:n_r, :])
                        T.copy(o_t[0:n_r, :], out[base:base + n_r, t, 0:N])

    return main
