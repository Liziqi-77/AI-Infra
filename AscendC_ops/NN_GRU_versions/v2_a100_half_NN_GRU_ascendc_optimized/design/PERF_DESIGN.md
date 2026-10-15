# PERF_DESIGN — fused single-layer GRU (Atlas A2 / Ascend910B2C, DAV_2201)

Design-phase performance gate for Phase 3 (steps 1e / 2e) plus the optimization
list that step 3.5 would have to verify. Written against
`tilelang-perf-optimization` (anti-pattern list) and
`ascendc-tiling-design/references/matmul/ascendc-api-matmul-tiling.md`.

## 1. Operator shape and where the time goes

Per chunk of `rows` batch rows, `S` timesteps, `I = 128`, `H = 512`:

| stage | shape | flops | notes |
|-------|-------|-------|-------|
| input projection (3 gates) | M = rows·S, N = H, K = I | 2·rows·S·H·I | once per chunk, fully parallel |
| recurrent projection (3 gates) | M = rows, N = H, K = H | 2·rows·H·H | **S times**, serialised by the recurrence |
| cell | rows·H elements | ~10 ops/element | **S times** |

Amdahl view: with `I = 128` and `H = 512`, the per-timestep recurrent GEMM
(`K = H`) costs 4x the per-timestep share of the input projection per row. The
input projection is a single wide GEMM and parallelises across all cores; the
recurrent GEMM and the cell are on the critical path and have only `rows` rows of
parallelism. So the recurrence chain dominates, which is why the design keeps it
as tight as possible:

* all `S` iterations are host-side **stream-ordered launches** (no per-step
  workspace round trip, no host-device sync inside the loop);
* the hidden state `h` stays in GM in fp32 and is read/written by the next
  GEMM/cell in place, so no copy is needed between steps;
* the cell's gate arithmetic runs entirely in fp32 with `exp` + `reciprocal`,
  staying on one pipe.

## 2. Anti-pattern scan (design gate 1e / 2e)

| anti-pattern | status |
|---|---|
| elementwise scalar loop in the kernel | **clean** — all vector work is `T.copy` / vector-primitive level, tile-wide |
| redundant sync between same-pipe ops | **clean** — the cell chain is `v -> v` in-order FIFO, no barrier inserted |
| missing double buffering on the copy-in pipe | **clean** for the cast kernel (2-deep VECIN/VECOUT, 4096-element chunks); the cell uses 1-deep queues but its tile is only 8 KB/row-block, and the loop is dominated by the GM-resident recurrence |
| instruction fusion missed (`exp`+`reciprocal` split) | **clean** — `sigmoid(x) = 1/(1+exp(-x))`, `tanh(x) = 1-2/(1+exp(2x))`; no auxiliary adv_api tmp buffer |
| UB over-subscription | **clean** — cell uses 7·8 KB VECIN + 2·8 KB VECOUT + 6·8 KB scratch = 120 KB ≤ 192 KB; cast uses 2·8 KB + 2·16 KB |
| L0C/L1 over-subscription in the GEMM | **clean** — `MatmulApiTiling::GetTiling` sizes baseM/baseN/baseK from the reported L1/L0C/UB sizes |
| core idle in the tail | **clean** — `tileM` is 16-aligned and floored at 16, so every core but the last gets a full window; the last gets `tailM = M - mIdx·tileM` via `SetSingleShape` |

## 3. Multi-core / UB decisions (recorded for translation)

* Multi-core: M split (`PlanCoreTile`), 16-aligned, `nBlk = ceil(M / tileM)`,
  `blockDim = nBlk`; each core supplies its own GM offsets for A and C.
  `SetDim(1)` on the host is deliberate: the kernel does the split, so the tiling
  generator must not split as well (doing both applies the split twice).
  This is the "multi-core split extension" of the category guide, expressed
  kernel-side instead of host-side.
* UB split: `cell_rows = 4` hidden rows per iteration (4·512·4 = 8 KB per
  buffer); a full `[rows, H]` tile does not fit at any realistic `rows`.
* Batch chunking: `BATCH_CHUNK = 1024` bounds the fp32 input-projection
  intermediate `3·Bc·S·H·4 B` (~1.6 GB at S=256, H=512). Exact because GRU rows
  are independent.
* `KERNEL_TASK_TYPE` per translation unit: the CANN toolchain tags a core type
  per **source file**, not per kernel. Mixing `AIV_ONLY` and `AIC_ONLY` kernels in
  one file makes the whole file `K_TYPE_AIV`, which silently turns the Cube
  kernel into an empty no-op (the `if ASCEND_IS_AIC` body is never reached).
  Hence `gru_cube.cpp` (AIC) and `gru_vector.cpp` (AIV) are separate sources.

## 4. Step-3.5 optimization list (待验证清单)

Ordered by expected payoff on the critical path. **Not verified in this run** —
see `perf_tuning/SKIPPED.md`: TileLang cannot be executed here (no local
`tilelang` install, and `evaluate_tilelang.sh` needs `scp`/`ssh` to a remote
Ascend box).

1. **Persistent recurrent weight in L1.** `W_hh` is `[3H, H] = 3·512·512·4 B
   ≈ 3 MB` — too big for L1 (512 KB), but one gate slice is 1 MB, still too big.
   Sub-tile it: keep the `[H, K_chunk]` block resident across the `S` iterations
   instead of re-staging it every timestep.
2. **Batch the 3 gates into one GEMM.** `N` becomes `3H = 1536` and the three
   launches collapse into one, cutting launch overhead 3x on the critical path.
   The cell then reads the three slices from one `[rows, 3H]` buffer.
3. **Fuse the cell into the recurrent GEMM's epilogue** (fixpipe -> vector) so
   `h_t` is produced without a GM round trip; requires a cross-scope flag pair
   inside one kernel instead of stream-ordered launches.
4. **Split-K / L2 tiling for the input projection** when `rows·S` is large:
   it is a plain GEMM with M = rows·S and is currently one k-loop per core.
5. **Double-buffer the cell's CopyIn** (2-deep VECIN) if the tail rows make the
   copy non-overlapped.

Success criterion (from the workflow): geomean speedup vs the reference kernel
≥ 0.6x. Measured speedup for this run is in `../performance.json` (measured on the
AscendC delivery, not on TileLang).
