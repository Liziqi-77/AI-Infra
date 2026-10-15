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

"""Block-level TileLang design for the fused single-layer GRU (batch_first=True).

    GI_g  = xf @ W_ih_g^T + b_ih_g        M = rows*S, N = H, K = I   (Cube)
    GH_g  = h  @ W_hh_g^T + b_hh_g        M = rows,   N = H, K = H   (Cube)
    (h, out_t) = cell(GI[:, t], GH, h)                              (Vector)

This file captures only the *block* layer: what the grid is, how the work is
partitioned over AI cores, which scope (Cube `C` / Vector `V`) owns which stage
and how the stages hand off.  Tile-level buffer sizes and the exact vector
instruction chain live in `design/tile_level/gru.py`.

Platform: Atlas A2 (Ascend910B2C) => NpuArch DAV_2201 => Ascend C high-level
Matmul API path (`ascendc-tiling-design/references/matmul/ascendc-api-matmul-tiling.md`).

--------------------------------------------------------------------------
Grid / partitioning
--------------------------------------------------------------------------
The recurrence is independent over batch rows, so the batch axis is the only
free axis and every kernel below partitions exactly one dimension:

  * `gru_input_gemm`     grid = n_blk_in  = ceil(rows*S, tileM_in)
  * `gru_recurrent_gemm` grid = n_blk_rec = ceil(rows,   tileM_rec)
  * `gru_cast_fp32`      grid = aiv_num   (1-D element split, 16-aligned)
  * `gru_cell`           grid = aiv_num   (row split, CELL_ROWS rows per UB tile)

`tileM` is rounded up to a multiple of 16 (ALIGNED_H) and floored at 16 so that
every core except the last works on a whole number of cube rows; the tail core
uses `SetSingleShape(tailM, N, K)`.  `host` picks the same tile size, so the
launch block-dim and the device window agree by construction.

Batch chunking: the host walks the batch in chunks of BATCH_CHUNK = 1024 rows so
the fp32 input-projection intermediate stays bounded
(3 * 1024 * S * H * 4 B = 1.6 GB at S=256, H=512).  Chunking is exact because
the rows are independent, and the loop is a plain sequential re-launch of the
same grid -- no cross-chunk state.

Timestep loop: `S` sequential iterations, each launching the recurrent GEMM
(3 gates) followed by the cell kernel.  All launches share one ACL stream, so
every dependency (cast -> GEMM, GEMM -> cell, cell -> next GEMM reading `h`) is
a stream-order edge and needs no explicit flag.

--------------------------------------------------------------------------
Cross-scope handoff
--------------------------------------------------------------------------
There is no Cube<->Vector cross-flag pair inside a single kernel here: the
stages are separate launches on one stream, so `GI`, `GH`, `h` and `out` are
plain GM tensors written by one launch and read by the next.  Inside the cell
kernel the Cube scope is unused (`KERNEL_TYPE_AIV_ONLY`), and inside the GEMM
kernel the Vector scope is unused (`KERNEL_TYPE_AIC_ONLY`) -- see the note in
`design/PERF_DESIGN.md` about the CANN toolchain tagging one core type per
translation unit.
"""

import tilelang
import tilelang.language as T

# --- shape / grid constants shared with the host -----------------------------
BATCH_CHUNK = 1024   # batch rows per host chunk
ALIGNED_H = 16       # M/K alignment for cube rows
CELL_ROWS = 4        # hidden rows per cell UB tile (4 * 512 fp32 = 8 KB per buffer)


@tilelang.jit(out_idx=[3], target="ascendc")
def gru_input_gemm(rows, seq, input_size, hidden_size, tile_m, dtype="float16"):
    """GI = xf @ W_ih^T + b_ih, sliced per gate.

    `sel=0` variant of `gru_gemm`: M = rows*S, N = H, K = I, one gate slice of
    the [3H, I] weight and [3H] bias per launch.
    """
    M = rows * seq
    N = hidden_size
    K = input_size
    n_blk = (M + tile_m - 1) // tile_m
    accum_dtype = "float"

    @T.prim_func
    def main(
        xf: T.Tensor((M, K), dtype),
        w_ih: T.Tensor((3 * N, K), dtype),
        b_ih: T.Tensor((3 * N,), accum_dtype),
        gi: T.Tensor((M, N), accum_dtype),
    ):
        with T.Kernel(n_blk, is_npu=True) as (cid, vid):
            m0 = cid * tile_m
            block_m = T.min(tile_m, M - m0)
            with T.Scope("C"):
                # TODO(tile-level): stage xf[m0:m0+block_m, :] through L1/L0,
                # transpose B on L1, run the block matmul with bias, and store
                # the fp32 accumulator tile to gi[m0:m0+block_m, :].
                # Both `T.copy` and `T.gemm` are slice-level ops; the tail core
                # uses `block_m < tile_m` and the K/N extents are always full.
                pass

    return main


@tilelang.jit(out_idx=[3], target="ascendc")
def gru_recurrent_gemm(rows, hidden_size, tile_m, dtype="float16"):
    """GH = h @ W_hh^T + b_hh, sliced per gate.

    `sel=1` variant of `gru_gemm`: M = rows, N = H, K = H.  Same M partition as
    the input projection, so the two share `PlanCoreTile` on the host.
    """
    M = rows
    N = hidden_size
    K = hidden_size
    n_blk = (M + tile_m - 1) // tile_m
    accum_dtype = "float"

    @T.prim_func
    def main(
        h: T.Tensor((M, K), dtype),
        w_hh: T.Tensor((3 * N, K), dtype),
        b_hh: T.Tensor((3 * N,), accum_dtype),
        gh: T.Tensor((M, N), accum_dtype),
    ):
        with T.Kernel(n_blk, is_npu=True) as (cid, vid):
            m0 = cid * tile_m
            block_m = T.min(tile_m, M - m0)
            with T.Scope("C"):
                # TODO(tile-level): identical structure to gru_input_gemm with
                # K = H; `h` is rewritten by the cell at every timestep, so the
                # read happens after the cell launch in stream order.
                pass

    return main


@tilelang.jit(out_idx=[1], target="ascendc")
def gru_cast_fp32(total, dtype="float16"):
    """xf = cast(x) to fp32 (identity when x is already fp32).

    Pure 1-D elementwise, so the grid is the AIV core count and each core owns a
    16-element-aligned contiguous range (keeps the tail DataCopy 32B-aligned for
    both 2-byte and 4-byte element types).
    """
    @T.prim_func
    def main(
        src: T.Tensor((total,), dtype),
        dst: T.Tensor((total,), "float32"),
    ):
        with T.Kernel(20, is_npu=True) as (cid, vid):
            with T.Scope("V"):
                # TODO(tile-level): per-core 16-aligned element range, VECIN /
                # VECOUT double buffering in 4096-element chunks.
                pass

    return main


@tilelang.jit(target="ascendc")
def gru_cell(rows, seq, hidden_size, t, cell_rows=CELL_ROWS, out_dtype="float16"):
    """One recurrent cell update at timestep `t`.

    grid = aiv_num; core `cid` owns rows [b0, b0+rows_per_core).  Within a core
    the hidden state is walked in tiles of `cell_rows` rows, because a [rows, H]
    tile does not fit UB at H = 512.

        u_r = giR + ghR         r = 1 / (1 + exp(-u_r))
        u_z = giZ + ghZ         z = 1 / (1 + exp(-u_z))
        u_n = giN + r * ghN     n = 1 - 2 / (1 + exp(2 * u_n))
        h_t = (1 - z) * n + z * h_{t-1}
    """
    N = hidden_size
    accum_dtype = "float"

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
            with T.Scope("V"):
                # TODO(tile-level): row range [b0, b0+rows_per_core) split into
                # cell_rows-row UB tiles; strided DataCopy for the gi slices
                # (step t out of every `seq` rows), contiguous rows for gh/h;
                # gate chain in fp32; `h` and `out[:, t, :]` both written back.
                pass

    return main
