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
 * \file gru.cpp
 * \brief Host side of the fused GRU operator: tiling computation + kernel launches.
 *
 *  Structure (chunked along the batch so the [3, Bc*S, H] fp32 intermediate stays
 *  bounded; the recurrence is batch-element independent so the split is exact):
 *
 *    for each batch chunk of Bc rows:
 *        xf   = cast(x_chunk)                                    (AIV)
 *        GI   = xf   @ W_ih^T + b_ih   for gate g in {r, z, n}    (AIC)
 *        h    = 0
 *        for t in 0 .. S-1:
 *            GH = h @ W_hh^T + b_hh    for gate g in {r, z, n}    (AIC)
 *            cell(GI[:, t], GH, h) -> h, out[:, t, :]            (AIV)
 *
 *  Every launch shares one ACL stream, so all inter-kernel ordering (casting
 *  before GEMM, GEMM before the cell, cell before the next GEMM reading `h`) is
 *  supplied by stream order and needs no explicit flags.
 *
 *  Accumulation is fp32 throughout, and `out` is emitted in the dtype of `x`
 *  (fp16 -> fp16, fp32 -> fp32), mirroring the reference, whose module takes the
 *  dtype of its input. fp32 inputs therefore run a genuine fp32 path: the cast
 *  step is skipped, the cube GEMMs read fp32 A/B, and the recurrent cell writes
 *  fp32 output (the vector kernel is templated on the output type).
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include <torch/extension.h>
#include <torch/library.h>

#include "torch_kernel_helper.h"
#include "tiling/platform/platform_ascendc.h"
#include "adv_api/matmul/bmm_tiling.h"

#include "aclrtlaunch_gru_cast_fp32.h"
#include "aclrtlaunch_gru_gemm.h"
#include "aclrtlaunch_gru_rec_elem.h"

#include "aclrtlaunch_gru_bwd_hprev.h"
#include "aclrtlaunch_gru_bwd_cell.h"
#include "aclrtlaunch_gru_bwd_gemm.h"
#include "aclrtlaunch_gru_bwd_colsum_partial.h"
#include "aclrtlaunch_gru_bwd_colsum_final.h"
#include "aclrtlaunch_gru_bwd_add.h"
#include "aclrtlaunch_gru_bwd_pipe.h"

#include "gru_tiling_data.h"
#include "ops.h"

namespace ascend_kernel {

namespace {

using Gru::GruTilingData;

// ---------------------------------------------------------------------------
// Cross-op gate cache (P5, on by default; disable with GRU_STORE_GATES=0).
//
// The backward used to recompute the forward's gate pre-activations with two
// GEMMs (GI = xf @ W_ih^T, GH = hp @ W_hh^T). The forward already materialises
// both per chunk, so with this flag on it also *persists* them into the two
// full-batch buffers below ([B*S, 3H] fp32, in the same (batch, time) row order
// the backward cell indexes). gru_backward then reads the cached slices and
// skips the two GEMMs.
//
// The buffer is held here rather than threaded through the op signatures,
// which must stay unchanged. It is reused in place across steps (reallocated
// only when the shape/device changes) so the training loop sees no per-step
// allocator churn, and gru_backward validates (B, S, H) against the forward
// that filled it before trusting it.
struct GateStore {
    at::Tensor gi;  // [B*S, 3H] fp32, forward input projection  a_x
    at::Tensor gh;  // [S, B, 3H] fp32, forward recurrent projection a_h (step-major)
    // [S, B, H] fp32, h_{t-1} for every step. The forward cell writes step t's
    // own h_{t-1} operand straight in, so the backward skips gru_bwd_hprev
    // (which otherwise reads `out` back from HBM and re-writes these values).
    at::Tensor hp;
    int64_t B = -1, S = -1, H = -1;
};
GateStore &gateStore()
{
    static GateStore s;
    return s;
}
// Default on: unset or >0 enables the cache, =0 restores the recompute path.
bool storeGatesEnabled()
{
    const char *e = std::getenv("GRU_STORE_GATES");
    return (e == nullptr) || (std::atoi(e) != 0);
}

// dtype tags shared with op_kernel/gru.cpp
constexpr int64_t TAG_HALF = 1;
constexpr int64_t TAG_BF16 = 2;
constexpr int64_t TAG_FLOAT = 3;

// Batch rows per chunk. Bounds the fp32 input-projection workspace to
// 3 * rows * S * H * 4 bytes (~12.9 GB at rows=8192, S=256, H=512).
//
// This is the forward's counterpart of BWD_BATCH_CHUNK: a larger chunk shrinks
// the number of chunk-level launches and raises the recurrent projection's M
// extent (better cube occupancy per launch). Measured forward time falls
// monotonically with the chunk (512/1024/2048/4096/8000 -> 0.460/0.304/0.180/
// 0.145/0.121 s at B=8000), so the default is set to a single chunk for the
// main B=8000 config while still bounding the workspace. Env-overridable so the
// launch-count / workspace trade-off can be re-measured without a rebuild.
constexpr int64_t BATCH_CHUNK_DEFAULT = 8192;

int64_t FwdBatchChunk()
{
    static const int64_t chunk = [] {
        const char *env = std::getenv("GRU_FWD_CHUNK");
        if (env != nullptr) {
            const long long n = std::strtoll(env, nullptr, 10);
            if (n > 0) {
                return static_cast<int64_t>(n);
            }
        }
        return BATCH_CHUNK_DEFAULT;
    }();
    return chunk;
}

int64_t DtypeTag(const at::Tensor &t)
{
    switch (t.scalar_type()) {
        case at::kHalf:
            return TAG_HALF;
        case at::kBFloat16:
            return TAG_BF16;
        case at::kFloat:
            return TAG_FLOAT;
        default:
            TORCH_CHECK(false, "gru: unsupported input dtype ", t.scalar_type());
            return TAG_FLOAT;
    }
}

// Debug-only: print the generated cube tiling when GRU_TILING_DEBUG is set.
void DumpTiling(const char *what, int64_t M, int64_t N, int64_t K,
                const AscendC::tiling::TCubeTiling &t)
{
    if (std::getenv("GRU_TILING_DEBUG") == nullptr) {
        return;
    }
    std::fprintf(stderr,
                 "[tiling] %-4s shape(%lld,%lld,%lld) usedCore=%u M=%d N=%d Ka=%d Kb=%d "
                 "scM=%d scN=%d scK=%d baseM=%d baseN=%d baseK=%d depthA1=%d depthB1=%d "
                 "stepM=%d stepN=%d stepKa=%d stepKb=%d isBias=%d iterOrder=%d\n",
                 what, (long long)M, (long long)N, (long long)K, t.usedCoreNum, t.M, t.N,
                 t.Ka, t.Kb, t.singleCoreM, t.singleCoreN, t.singleCoreK, t.baseM, t.baseN,
                 t.baseK, t.depthA1, t.depthB1, t.stepM, t.stepN, t.stepKa, t.stepKb,
                 (int)t.isBias, t.iterateOrder);
}

void FillMatmulTiling(platform_ascendc::PlatformAscendC *plat,
                      int32_t l1Size, int32_t l0cSize, int32_t ubSize,
                      int64_t M, int64_t N, int64_t K,
                      AscendC::tiling::TCubeTiling &out, const char *what)
{
    matmul_tiling::MultiCoreMatmulTiling mm(*plat);
    mm.SetBufferSpace(l1Size, l0cSize, ubSize);
    // fp16 operands, fp32 accumulate (see gru_cube.cpp): the tiling must be
    // generated for the fp16 L0/L1 fractal sizes, not the fp32 ones. C is fp16
    // as well -- the gate pre-activations are read back at fp16 by the vector
    // cells (L0C accumulation stays fp32, only the writeout narrows).
    mm.SetAType(matmul_tiling::TPosition::GM, matmul_tiling::CubeFormat::ND,
                matmul_tiling::DataType::DT_FLOAT16, false);
    mm.SetBType(matmul_tiling::TPosition::GM, matmul_tiling::CubeFormat::ND,
                matmul_tiling::DataType::DT_FLOAT16, true);
    mm.SetCType(matmul_tiling::TPosition::GM, matmul_tiling::CubeFormat::ND,
                matmul_tiling::DataType::DT_FLOAT16);
    mm.SetBiasType(matmul_tiling::TPosition::GM, matmul_tiling::CubeFormat::ND,
                   matmul_tiling::DataType::DT_FLOAT);
    mm.EnableBias(true);
    // Single-core tiling: the kernel itself partitions the M extent across AI
    // cores by block index and hands each core its own GM offsets. Asking the
    // tiling generator to split as well would apply the split twice.
    mm.SetDim(1);
    mm.SetShape(static_cast<int32_t>(M), static_cast<int32_t>(N), static_cast<int32_t>(K));
    mm.SetOrgShape(static_cast<int32_t>(M), static_cast<int32_t>(N), static_cast<int32_t>(K));
    if (mm.GetTiling(out) != 0) {
        throw std::runtime_error(std::string("gru: matmul tiling failed for ") + what);
    }
    DumpTiling(what, M, N, K, out);
}

// M partition shared by the host (to size the launch) and the device (to place
// each core's window). `tileM` is kept 16-aligned so every core but the last
// works on a whole number of cube rows.
struct CoreTile {
    int64_t tileM;
    int64_t nBlk;
};

CoreTile PlanCoreTile(int64_t fullM, int64_t coreNum)
{
    int64_t tileM = (fullM + coreNum - 1) / coreNum;
    tileM = ((tileM + 15) / 16) * 16;
    if (tileM < 16) {
        tileM = 16;
    }
    CoreTile t;
    t.tileM = tileM;
    t.nBlk = (fullM + tileM - 1) / tileM;
    return t;
}

// K partition used by the split-K weight-gradient GEMMs. Same 16-alignment
// rule as PlanCoreTile so every block but the last contracts a whole number of
// cube rows.
struct KTile {
    int64_t tileK;
    int64_t nBlk;
};

KTile PlanKTile(int64_t fullK, int64_t coreNum)
{
    int64_t tileK = (fullK + coreNum - 1) / coreNum;
    tileK = ((tileK + 15) / 16) * 16;
    if (tileK < 16) {
        tileK = 16;
    }
    KTile t;
    t.tileK = tileK;
    t.nBlk = (fullK + tileK - 1) / tileK;
    return t;
}

// Same as FillMatmulTiling but with the transpose pattern / bias flag of the
// caller. The backward pass needs three distinct GEMM flavours:
//   transA=F transB=T bias   (GI, GH)   A[M,K] B[N,K]
//   transA=F transB=F no-bias(dhb)      A[M,K] B[K,N]
//   transA=T transB=F no-bias(dW)       A[K,M] B[K,N]
void FillMatmulTilingEx(platform_ascendc::PlatformAscendC *plat,
                        int32_t l1Size, int32_t l0cSize, int32_t ubSize,
                        int64_t M, int64_t N, int64_t K,
                        bool transA, bool transB, bool isBias,
                        AscendC::tiling::TCubeTiling &out, const char *what)
{
    matmul_tiling::MultiCoreMatmulTiling mm(*plat);
    mm.SetBufferSpace(l1Size, l0cSize, ubSize);
    mm.SetAType(matmul_tiling::TPosition::GM, matmul_tiling::CubeFormat::ND,
                matmul_tiling::DataType::DT_FLOAT16, transA);
    mm.SetBType(matmul_tiling::TPosition::GM, matmul_tiling::CubeFormat::ND,
                matmul_tiling::DataType::DT_FLOAT16, transB);
    // C is fp16 for every non-transposed-A output: the (transA=F, transB=T)
    // gate projections AND the transA=F, transB=F flavour, which is used only by
    // the dhb GEMM (sel DH / DH_T) whose output the cell reads back and stores at
    // fp16 anyway. The transpose-A weight-gradient GEMMs keep fp32 gradients.
    const bool c16 = !transA;
    mm.SetCType(matmul_tiling::TPosition::GM, matmul_tiling::CubeFormat::ND,
                c16 ? matmul_tiling::DataType::DT_FLOAT16
                    : matmul_tiling::DataType::DT_FLOAT);
    mm.SetBiasType(matmul_tiling::TPosition::GM, matmul_tiling::CubeFormat::ND,
                   matmul_tiling::DataType::DT_FLOAT);
    mm.EnableBias(isBias);
    mm.SetDim(1);
    mm.SetShape(static_cast<int32_t>(M), static_cast<int32_t>(N), static_cast<int32_t>(K));
    mm.SetOrgShape(static_cast<int32_t>(M), static_cast<int32_t>(N), static_cast<int32_t>(K));
    // Tuning override: force the cube base tile for the chosen slot so the
    // auto-tiler's choice can be compared against alternatives without a rebuild.
    // GRU_MM_SLOT accepts a comma-separated slot list (e.g. "WI,WH").
    if (const char *fix = std::getenv("GRU_MM_FIX")) {
        const char *slot = std::getenv("GRU_MM_SLOT");
        bool match = (slot == nullptr);
        for (const char *p = slot; !match && p != nullptr && *p != '\0';) {
            const char *comma = std::strchr(p, ',');
            const size_t len = (comma == nullptr) ? std::strlen(p) : static_cast<size_t>(comma - p);
            if (std::strlen(what) == len && std::strncmp(p, what, len) == 0) {
                match = true;
            }
            p = (comma == nullptr) ? nullptr : comma + 1;
        }
        if (match) {
            int bm = 0, bn = 0, bk = 0;
            if (std::sscanf(fix, "%d,%d,%d", &bm, &bn, &bk) == 3 && bm > 0 && bn > 0 && bk > 0) {
                mm.SetFixSplit(bm, bn, bk);
                std::fprintf(stderr, "[fixsplit] %s -> %d,%d,%d\n", what, bm, bn, bk);
            }
        }
    }
    // Optional: enable L1 double buffering for A/B/C/bias so the K loop can
    // overlap the L1 refill with the MMA instead of stalling on it.
    if (std::getenv("GRU_MM_DB") != nullptr) {
        mm.SetDoubleBuffer(true, true, true, true);
    }
    if (mm.GetTiling(out) != 0) {
        throw std::runtime_error(std::string("gru_backward: matmul tiling failed for ") + what);
    }
    DumpTiling(what, M, N, K, out);
}

// Probe knobs (measurement only): override the DH GEMM's K extent and its core
// count so the shape dependence of the recurrent GEMM can be attributed.
// GRU_DH_K changes the contraction length (wrong math, timing only);
// GRU_DH_CORES changes how many AI cores the M extent is split over.
int64_t DhCores(int64_t aicNum)
{
    static const int64_t n = [] {
        const char *env = std::getenv("GRU_DH_CORES");
        if (env != nullptr) {
            const long long v = std::strtoll(env, nullptr, 10);
            if (v > 0) {
                return static_cast<int64_t>(v);
            }
        }
        return static_cast<int64_t>(-1);
    }();
    return n > 0 ? n : aicNum;
}

int64_t DhK(int64_t k)
{
    static const int64_t v = [] {
        const char *env = std::getenv("GRU_DH_K");
        if (env != nullptr) {
            const long long x = std::strtoll(env, nullptr, 10);
            if (x > 0) {
                return static_cast<int64_t>(x);
            }
        }
        return static_cast<int64_t>(-1);
    }();
    return v > 0 ? v : k;
}

// Batch rows per backward chunk. BPTT keeps ~4 fp32 buffers of [rows*S, 3H]
// alive at once plus hp [rows, S, H], so this bounds the peak workspace while
// setting the number of times the 2*S-launch reverse sweep is replayed.
//
// The sweep is launch-latency bound: each chunk replays 2*S dependent launches
// whose per-step GEMM/cell occupy only a sliver of the cores, so a larger chunk
// both shrinks the launch count and raises the per-step M extent (better core
// occupancy). The compiled default is the tuned value; the env override exists so
// the launch-count / workspace trade-off can be re-measured without a rebuild.
constexpr int64_t BWD_BATCH_CHUNK_DEFAULT = 2048;

// Number of AI cores the weight-gradient GEMMs (WI/WH) are split over. Their M
// (= 3H = 1536) is partitioned by block index just like GI/GH, so raising the
// core count raises the M-parallelism without any reduction pass.
//
// These GEMMs have a huge contraction (K = chunkRows*S = 524288) and are
// bandwidth-bound, and the whole B operand is re-read by every M block. More
// cores therefore buy compute parallelism but cost B traffic: sweeping 4/6/8/
// 12/16/24 cores gives backward 0.540/0.436/0.432/0.350/0.376/0.362 s, so 12
// (half the AIC count) sits at the optimum and is the default. Env-overridable
// so the trade-off can be re-measured without a rebuild.
int64_t WCores(int64_t aicNum)
{
    static const int64_t n = [] {
        const char *env = std::getenv("GRU_W_CORES");
        if (env != nullptr) {
            const long long v = std::strtoll(env, nullptr, 10);
            if (v > 0) {
                return static_cast<int64_t>(v);
            }
        }
        return static_cast<int64_t>(-1);
    }();
    const int64_t def = std::min<int64_t>(aicNum, 12);
    return n > 0 ? std::min(n, aicNum) : def;
}

// Split-K mode for the weight-gradient GEMMs (WI/WH). Default on: their
// contraction K = chunkRows*S is partitioned across AI cores so each core reads
// a distinct K-slab and the B operand is transported once per launch instead of
// once per M block.
//
// GRU_W_MODE=msplit restores the legacy M partition (with GRU_W_CORES picking
// the block count) so the two strategies can be compared inside one binary;
// GRU_W_KSPLIT overrides the number of K slabs (default: all AIC cores).
bool WKSplitMode()
{
    static const bool m = [] {
        const char *env = std::getenv("GRU_W_MODE");
        return !(env != nullptr && std::strcmp(env, "msplit") == 0);
    }();
    return m;
}

int64_t WKSplit(int64_t aicNum)
{
    static const int64_t n = [] {
        const char *env = std::getenv("GRU_W_KSPLIT");
        if (env != nullptr) {
            const long long v = std::strtoll(env, nullptr, 10);
            if (v > 0) {
                return static_cast<int64_t>(v);
            }
        }
        return static_cast<int64_t>(-1);
    }();
    return n > 0 ? std::min(n, aicNum) : aicNum;
}

int64_t BwdBatchChunk()
{
    static const int64_t chunk = [] {
        const char *env = std::getenv("GRU_BWD_CHUNK");
        if (env != nullptr) {
            const long long n = std::strtoll(env, nullptr, 10);
            if (n > 0) {
                return static_cast<int64_t>(n);
            }
        }
        return BWD_BATCH_CHUNK_DEFAULT;
    }();
    return chunk;
}

}  // namespace

at::Tensor gru(const at::Tensor &x, const at::Tensor &w_ih, const at::Tensor &w_hh,
               const at::Tensor &b_ih, const at::Tensor &b_hh)
{
    TORCH_CHECK(x.dim() == 3, "gru: x must be [B, S, I]");
    TORCH_CHECK(w_ih.dim() == 2 && w_hh.dim() == 2, "gru: weights must be 2D");
    TORCH_CHECK(b_ih.dim() == 1 && b_hh.dim() == 1, "gru: biases must be 1D");

    const int64_t B = x.size(0);
    const int64_t S = x.size(1);
    const int64_t I = x.size(2);
    const int64_t H = w_ih.size(0) / 3;
    TORCH_CHECK(w_ih.size(0) == 3 * H && w_ih.size(1) == I, "gru: w_ih must be [3H, I]");
    TORCH_CHECK(w_hh.size(0) == 3 * H && w_hh.size(1) == H, "gru: w_hh must be [3H, H]");
    TORCH_CHECK(b_ih.size(0) == 3 * H && b_hh.size(0) == 3 * H, "gru: biases must be [3H]");
    TORCH_CHECK(x.scalar_type() == at::kHalf || x.scalar_type() == at::kBFloat16 ||
                    x.scalar_type() == at::kFloat,
                "gru: x must be fp16/bf16/fp32");
    TORCH_CHECK(B > 0 && S > 0, "gru: empty batch/sequence");

    const int64_t inTag = DtypeTag(x);
    // Output dtype follows the input dtype (the reference module computes in the
    // dtype of its input), so the recurrent cell writes with the same tag.
    const int64_t outTag = inTag;

    // The caller may hand over a half-precision module; the weights/biases are
    // promoted to fp32 for the host-side bookkeeping and to fp16 for the GEMM
    // operands (see below).
    at::Tensor wih = w_ih.contiguous();
    at::Tensor whh = w_hh.contiguous();
    at::Tensor bih = b_ih.contiguous();
    at::Tensor bhh = b_hh.contiguous();
    if (wih.scalar_type() != at::kFloat) {
        wih = wih.to(at::kFloat);
    }
    if (whh.scalar_type() != at::kFloat) {
        whh = whh.to(at::kFloat);
    }
    if (bih.scalar_type() != at::kFloat) {
        bih = bih.to(at::kFloat);
    }
    if (bhh.scalar_type() != at::kFloat) {
        bhh = bhh.to(at::kFloat);
    }
    // fp16 weight copies: the GEMM operands are fp16 (the cube rounds to an
    // 11-bit mantissa either way, so this costs no precision vs HF32). Weights
    // keep their fp32 copies for the bias / host-side use.
    const at::Tensor wih16 = wih.to(at::kHalf);
    const at::Tensor whh16 = whh.to(at::kHalf);

    const at::Tensor xIn = x.contiguous();
    at::Tensor out = at::empty({B, S, H}, x.options());
    // fp16 input projection operand (also reused as the WI GEMM's B operand).
    const at::Tensor xfAll = at::empty({B, S, I}, x.options().dtype(at::kHalf));

    auto *plat = platform_ascendc::PlatformAscendCManager::GetInstance();
    TORCH_CHECK(plat != nullptr, "gru: failed to obtain the Ascend platform");
    const int64_t aicNum = static_cast<int64_t>(plat->GetCoreNumAic());
    const int64_t aivNum = static_cast<int64_t>(plat->GetCoreNumAiv());
    TORCH_CHECK(aicNum > 0 && aivNum > 0, "gru: no AI cores reported by the platform");

    uint64_t l1Size = 0, l0cSize = 0, ubSize = 0;
    plat->GetCoreMemSize(platform_ascendc::CoreMemType::L1, l1Size);
    plat->GetCoreMemSize(platform_ascendc::CoreMemType::L0_C, l0cSize);
    plat->GetCoreMemSize(platform_ascendc::CoreMemType::UB, ubSize);

    // Tiling blob handed to the device as an int64-granular GM buffer. It is
    // rebuilt per batch chunk because the matmul tiling depends on the chunk's
    // M extent (the tail chunk can be shorter than BATCH_CHUNK).
    GruTilingData td = {};
    td.B = B;
    td.S = S;
    td.I = I;
    td.H = H;
    td.aicNum = aicNum;
    td.aivNum = aivNum;
    td.elemRows = 4;

    const int64_t bc = std::min(B, FwdBatchChunk());
    const uint32_t blkAiv = static_cast<uint32_t>(aivNum);
    const int64_t selIn = 0;
    const int64_t selRec = 1;
    const int64_t gate0 = 0;
    // Merged gate extent: a single GEMM covers the r | z | n gates as one
    // [rows, 3H] output, so the A operand (xf / hs) is read once per core
    // instead of three times. The gate blocks stay contiguous inside the N
    // extent, which the vector cell slices with a strided DataCopy.
    const int64_t G = 3 * H;

    // Opt-in: persist the gate pre-activations for the backward to read.
    const bool storeGates = storeGatesEnabled();
    GateStore &gs = gateStore();
    if (storeGates) {
        // gs.gh is step-major [S, B, G], NOT the [B*S, G] row-major layout of
        // gs.gi. Step t's slab is then one contiguous [rows, G] block, so the
        // recurrent-projection GEMM writes the gate pre-activations straight
        // into the backward's cache and the cell reads them in place -- the cell
        // no longer has to re-store them (12.58 GB of strided writes per step).
        const bool reuse = gs.gi.defined() && gs.gh.defined() && gs.hp.defined() &&
                           gs.gi.numel() == B * S * G &&
                           gs.gi.scalar_type() == at::kHalf &&
                           gs.gh.scalar_type() == at::kHalf &&
                           gs.gh.dim() == 3 && gs.gh.size(0) == S &&
                           gs.gh.size(1) == B && gs.gh.size(2) == G &&
                           gs.hp.dim() == 3 && gs.hp.size(0) == S &&
                           gs.hp.size(1) == B && gs.hp.size(2) == H &&
                           gs.gi.device() == x.device();
        if (!reuse) {
            // gi/gh are the two gate pre-activation caches the vector cells read
            // back; both are fp16 (see the cube), which halves the cells' single
            // largest MTE2 stream.
            gs.gi = at::empty({B * S, G}, x.options().dtype(at::kHalf));
            gs.gh = at::empty({S, B, G}, x.options().dtype(at::kHalf));
            gs.hp = at::empty({S, B, H}, x.options().dtype(at::kHalf));
        }
        gs.B = B;
        gs.S = S;
        gs.H = H;
    }

    for (int64_t b0 = 0; b0 < B; b0 += bc) {
        const int64_t rows = std::min(bc, B - b0);

        td.chunkRows = rows;
        const CoreTile tin = PlanCoreTile(rows * S, aicNum);
        const CoreTile trec = PlanCoreTile(rows, aicNum);
        FillMatmulTiling(plat, static_cast<int32_t>(l1Size),
                         static_cast<int32_t>(l0cSize), static_cast<int32_t>(ubSize),
                         tin.tileM, G, I, td.mmIn, "input projection");
        FillMatmulTiling(plat, static_cast<int32_t>(l1Size),
                         static_cast<int32_t>(l0cSize), static_cast<int32_t>(ubSize),
                         trec.tileM, G, H, td.mmRec, "recurrent projection");

        at::Tensor tilingCpu = at::empty({static_cast<int64_t>(sizeof(GruTilingData))},
                                         at::device(at::kCPU).dtype(at::kByte));
        std::memcpy(tilingCpu.data_ptr(), &td, sizeof(GruTilingData));
        at::Tensor tilingNpu = tilingCpu.to(at::kPrivateUse1);

        // ---- populate the fp16 input operand ------------------------------
        // xf is fp16 now (it is the GI GEMM's A operand). fp32 and fp16 inputs
        // ride through an elementwise copy, which the framework lowers to a
        // dtype-converting DMA (fp32 -> fp16) or a plain move (fp16 -> fp16); a
        // same-type vector Cast is not defined and produced NaNs. Only bf16
        // still needs the vector cast kernel.
        at::Tensor xf = xfAll.narrow(0, b0, rows);
        if (inTag == TAG_BF16) {
            at::Tensor xSrc = xIn.narrow(0, b0, rows);
            EXEC_KERNEL_CMD(gru_cast_fp32, blkAiv, xSrc, xf, tilingNpu, inTag);
        } else {
            xf.copy_(xIn.narrow(0, b0, rows));
        }

        // ---- input projection: [rows*S, 3H] (all gates in one GEMM) --------
        at::Tensor gi = storeGates ? gs.gi.narrow(0, b0 * S, rows * S)
                                   : at::empty({rows * S, G}, x.options().dtype(at::kHalf));
        const uint32_t blkIn = static_cast<uint32_t>(tin.nBlk);
        EXEC_KERNEL_CMD(gru_gemm, blkIn, xf, wih16, bih, gi, tilingNpu, selIn, gate0);

        // ---- running hidden state and its per-step recurrent projections ---
        // fp16: the recurrent projection's A operand and the cell's h_{t-1}
        // carrier. The cell keeps fp32 arithmetic internally.
        at::Tensor hs = at::zeros({rows, H}, x.options().dtype(at::kHalf));
        // With the gate cache on, gh is not scratch: the recurrent projection
        // writes step t's gate pre-activations directly into gs.gh (step-major,
        // so the slab is a contiguous [rows, G] view). The cell reads that very
        // view for its sigmoid/tanh inputs and never re-stores it -- this is
        // what removes the 12.58 GB per-step re-store from the cell.
        at::Tensor ghBuf = at::empty({rows, G}, x.options().dtype(at::kHalf));
        at::Tensor outChunk = out.narrow(0, b0, rows);
        const uint32_t blkRec = static_cast<uint32_t>(trec.nBlk);
        // The cell never re-stores gh now: the projection already put the gate
        // pre-activations exactly where the backward will read them.
        //
        // With the gate cache on the cell *does* write h_{t-1} to gs.hp: it is
        // the recurrent operand it just loaded, so staging it out costs one
        // MTE3 transfer that replaces the whole gru_bwd_hprev pass (which had
        // to read `out` back from HBM -- 4.19 GB -- and re-write the values).
        // The view is already offset by this chunk's first row; the kernel adds
        // the chunk-local row onto the full-batch step stride. gs.gh is reused
        // as a harmless placeholder when the cache is off.
        at::Tensor hpChunk = storeGates ? gs.hp.narrow(1, b0, rows) : ghBuf;
        // Named lvalues because EXEC_KERNEL_CMD's ConvertTypes takes refs.
        int64_t doStoreHp = storeGates ? 1 : 0;

        for (int64_t t = 0; t < S; ++t) {
            at::Tensor ghStep = storeGates ? gs.gh.select(0, t).narrow(0, b0, rows)
                                           : ghBuf;
            EXEC_KERNEL_CMD(gru_gemm, blkRec, hs, whh16, bhh, ghStep, tilingNpu, selRec, gate0);
            EXEC_KERNEL_CMD(gru_rec_elem, blkAiv, gi, ghStep, hs,
                            outChunk, hpChunk, tilingNpu, t, outTag, doStoreHp);
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// Backward pass: parameter gradients of the fused GRU.
//
//   dW_ih / dW_hh / db_ih / db_hh  from (grad_out, x, out, w_ih, w_hh)
//
// The batch is chunked exactly like the forward; the recurrence is independent
// across batch rows, so every chunk is a self-contained BPTT over its rows.
// Per chunk (rows = Bc batch rows, G = 3H):
//
//   hp   = shift(out)                                  gru_bwd_hprev   (AIV)
//   GI   = xf_flat @ W_ih^T + b_ih   [rows*S, G]       gru_bwd_gemm 0  (AIC)
//   GH   = hp      @ W_hh^T + b_hh   [rows*S, G]       gru_bwd_gemm 1  (AIC)
//   dha = dhb = 0
//   for t = S-1 .. 0:
//       dhb = dgh[:,t+1,:] @ W_hh    [rows, H]         gru_bwd_gemm 2  (AIC)
//       cell(dy[:,t], ..., dha, dhb) -> dgx, dgh[:,t,:] gru_bwd_cell    (AIV)
//   db_ih += colsum(dgx) ; db_hh += colsum(dgh)        gru_bwd_colsum_* (AIV)
//   dW_ih += dgx^T @ xf ; dW_hh += dgh^T @ hp          gru_bwd_gemm 3/4 (AIC, 1 core)
//
// Every launch shares one ACL stream, so ordering (GEMM before the cell that
// consumes it, cell before the GEMM that reads the step's dgh) is implicit.
// The transposed A operands of the dW GEMMs run with a single core so the
// M-window offset of a [K, M] operand never has to be computed.
// ---------------------------------------------------------------------------
std::tuple<at::Tensor, at::Tensor, at::Tensor, at::Tensor>
gru_backward(const at::Tensor &grad_out, const at::Tensor &x, const at::Tensor &out,
             const at::Tensor &w_ih, const at::Tensor &w_hh,
             const at::Tensor &b_ih, const at::Tensor &b_hh)
{
    using Gru::GruBwdTilingData;

    TORCH_CHECK(x.dim() == 3, "gru_backward: x must be [B, S, I]");
    TORCH_CHECK(grad_out.dim() == 3 && out.dim() == 3, "gru_backward: 3D grad/out required");
    TORCH_CHECK(w_ih.dim() == 2 && w_hh.dim() == 2, "gru_backward: weights must be 2D");
    TORCH_CHECK(b_ih.dim() == 1 && b_hh.dim() == 1, "gru_backward: biases must be 1D");
    // The vector side reads the gate blocks straight out of the [rows*S, 3H]
    // intermediates with 32-byte-strided DataCopy, which caps the strides at
    // 65535 blocks and needs whole 32-byte rows.
    TORCH_CHECK(x.scalar_type() == at::kFloat && grad_out.scalar_type() == at::kFloat &&
                    out.scalar_type() == at::kFloat,
                "gru_backward: fp32 only (the half-precision training path is not enabled)");

    const int64_t B = x.size(0);
    const int64_t S = x.size(1);
    const int64_t I = x.size(2);
    const int64_t H = w_ih.size(0) / 3;
    const int64_t G = 3 * H;
    TORCH_CHECK(w_ih.size(0) == G && w_ih.size(1) == I, "gru_backward: w_ih must be [3H, I]");
    TORCH_CHECK(w_hh.size(0) == G && w_hh.size(1) == H, "gru_backward: w_hh must be [3H, H]");
    TORCH_CHECK(b_ih.size(0) == G && b_hh.size(0) == G, "gru_backward: biases must be [3H]");
    TORCH_CHECK(grad_out.size(0) == B && grad_out.size(1) == S && grad_out.size(2) == H,
                "gru_backward: grad_out must be [B, S, H]");
    TORCH_CHECK(out.size(0) == B && out.size(1) == S && out.size(2) == H,
                "gru_backward: out must be [B, S, H]");
    TORCH_CHECK(H % 8 == 0, "gru_backward: hidden size must be a multiple of 8");
    TORCH_CHECK(S > 0 && B > 0, "gru_backward: empty batch/sequence");
    TORCH_CHECK((S * G - H) * 4 / 32 < 65536, "gru_backward: S * 3H stride exceeds 32B-block range");
    TORCH_CHECK((S * H - H) * 4 / 32 < 65536, "gru_backward: S * H stride exceeds 32B-block range");

    at::Tensor xIn = x.contiguous();
    at::Tensor goIn = grad_out.contiguous();
    at::Tensor outIn = out.contiguous();
    at::Tensor wih = w_ih.contiguous();
    at::Tensor whh = w_hh.contiguous();
    at::Tensor bih = b_ih.contiguous();
    at::Tensor bhh = b_hh.contiguous();

    // fp16 GEMM operands (see gru()): the cube rounds an fp16 operand to the
    // same 11-bit mantissa HF32 would, but at the fp16 MAC rate and half the L1
    // load. Only the A/B operands narrow; every accumulator (gate
    // pre-activations, dha/dhb, weight gradients, bias gradients) stays fp32.
    const at::Tensor xIn16 = xIn.to(at::kHalf);
    const at::Tensor wih16 = wih.to(at::kHalf);
    const at::Tensor whh16 = whh.to(at::kHalf);

    at::Tensor dWih = at::zeros({G, I}, x.options());
    at::Tensor dWhh = at::zeros({G, H}, x.options());
    at::Tensor dbih = at::zeros({G}, x.options());
    at::Tensor dbhh = at::zeros({G}, x.options());

    auto *plat = platform_ascendc::PlatformAscendCManager::GetInstance();
    TORCH_CHECK(plat != nullptr, "gru_backward: failed to obtain the Ascend platform");
    const int64_t aicNum = static_cast<int64_t>(plat->GetCoreNumAic());
    const int64_t aivNum = static_cast<int64_t>(plat->GetCoreNumAiv());
    TORCH_CHECK(aicNum > 0 && aivNum > 0, "gru_backward: no AI cores reported by the platform");

    uint64_t l1Size = 0, l0cSize = 0, ubSize = 0;
    plat->GetCoreMemSize(platform_ascendc::CoreMemType::L1, l1Size);
    plat->GetCoreMemSize(platform_ascendc::CoreMemType::L0_C, l0cSize);
    plat->GetCoreMemSize(platform_ascendc::CoreMemType::UB, ubSize);

    // Unused bias slot: every backward GEMM binds a bias GM pointer even when
    // the tiling says isBias = false, so hand it a real (never-read) buffer.
    at::Tensor dummyBias = at::zeros({G}, x.options());
    // Per-core partial column sums for the two bias gradients. Zero-initialised
    // as a safety net: the partial kernel publishes an explicit zero for cores
    // with no rows, but a zeroed workspace makes any such lapse harmless.
    at::Tensor ws = at::zeros({2 * aivNum * G}, x.options());

    GruBwdTilingData td = {};
    td.B = B;
    td.S = S;
    td.I = I;
    td.H = H;
    td.aicNum = aicNum;
    td.aivNum = aivNum;

    // Cap the batch chunk at BwdBatchChunk() to bound the workspace, then
    // rebalance to *equal* chunks. A ragged tail (e.g. B=8000 at chunk 2048 ->
    // 2048/2048/2048/1856) makes the small tail chunk pay the full per-chunk
    // epilogue (hprev + GI/GH + colsum + WI/WH + add) for a fraction of the
    // rows, and it becomes the critical path. Equal chunks remove that cost.
    const int64_t maxBc = std::min<int64_t>(B, BwdBatchChunk());
    const int64_t nBC = (B + maxBc - 1) / maxBc;
    const int64_t bc = (B + nBC - 1) / nBC;
    const uint32_t blkAiv = static_cast<uint32_t>(aivNum);
    const uint32_t blkOne = 1;

    const int64_t selGI = Gru::BWD_GEMM_GI;
    const int64_t selGH = Gru::BWD_GEMM_GH;
    const int64_t selDH = Gru::BWD_GEMM_DH;
    const int64_t selWI = Gru::BWD_GEMM_WI;
    const int64_t selWH = Gru::BWD_GEMM_WH;

    // Tuning variant: supply w_hh pre-transposed so the recurrent GEMM uses the
    // cube's transposed-B path (same math, different L0B load pattern).
    const bool dhTransB = std::getenv("GRU_DH_TRANSB") != nullptr;
    const int64_t selDHT = Gru::BWD_GEMM_DH_T;
    at::Tensor whhT;
    if (dhTransB) {
        whhT = whh16.t().contiguous();
    }

    // Default-on: if the forward cached its gate pre-activations for this exact
    // problem (GRU_STORE_GATES), skip the GI/GH recompute GEMMs and read the
    // stored slices instead. See GateStore above.
    const GateStore &gs = gateStore();
    const bool useStored = storeGatesEnabled() && gs.gi.defined() && gs.gh.defined() &&
                           gs.hp.defined() && gs.B == B && gs.S == S && gs.H == H;

    // Both gh layouts are step-major. With the cache on, the forward's
    // recurrent-projection GEMM wrote its gate pre-activations into the
    // step-major [S, B, G] gs.gh (see gru()); off, GH is recomputed from the
    // step-major hp and inherits that layout. Either way the cell reads gh with
    // the compact [rows, 3H]-slab stride (3H per row, not S*3H). dgh and hp are
    // step-major as well.
    td.ghStepMajor = 1;

    // P3: cross-chunk MIX pipeline (on by default, disable with GRU_BWD_PIPE=0).
    //
    // Two adjacent batch chunks have independent reverse sweeps, so the cube
    // dhb GEMM of one chunk can share a MIX task with the vector cell of the
    // other. Each chunk's op stream is  V(S-1), G(S-2), V(S-2), ..., G(0), V(0);
    // the host interleaves two such streams into 2S launches so that every
    // fused launch carries a cell on AIV and a GEMM on AIC, and the launch-order
    // skew makes every cross-stream dependency hold across a launch boundary
    // (no cross-core sync needed). See op_kernel/gru_bwd_pipe.cpp.
    //
    // Chunk size: two chunks are live at once, so the per-chunk scratch
    // (gi/gh/dgx/dgh are [rows*S, 3H] fp32) is doubled. Default 8192 rows --
    // i.e. a single chunk for B <= 8192, matching BATCH_CHUNK_DEFAULT on the
    // forward side.
    //
    // HF32 re-measurement (B=8000, warmup 3 / iters 10): with the cube no longer
    // on this path's critical path the cross-chunk MIX overlap stops paying for
    // its own overhead, and a single big chunk wins monotonically:
    //   2048 -> 170.15 ms, 4096 -> 167.35, 6000 -> 170.81, 7000 -> 174.33,
    //   8000 -> 160.21 ms (backward).
    // (`GRU_BWD_PIPE=0 GRU_BWD_CHUNK=8000` reaches the same 160.17 ms, so the
    // win is the chunk size, not the pipe toggle.) Peak device allocation at
    // 8000 rows is ~64 GB; override with GRU_BWD_PIPE_BC to trade the other way.
    // -----------------------------------------------------------------------
    const char *pipeEnv = std::getenv("GRU_BWD_PIPE");
    const bool pipeMode = ((pipeEnv == nullptr) || (std::atoi(pipeEnv) != 0)) && !dhTransB;

    if (pipeMode) {
        const char *pbcEnv = std::getenv("GRU_BWD_PIPE_BC");
        const int64_t pbcReq = (pbcEnv != nullptr) ? std::atoi(pbcEnv) : 8192;
        const int64_t pbc = std::max<int64_t>(1, std::min<int64_t>(B, pbcReq));
        const int64_t nchunks = (B + pbc - 1) / pbc;
        const uint32_t blkMix = static_cast<uint32_t>(aicNum);

        struct PChunk {
            int64_t rows = 0, KS = 0;
            int64_t nBlkGi = 0, nBlkDh = 0, nBlkW = 0;
            bool wk = false;
            at::Tensor xf, go, oc, hp, gi, gh, dgx, dgh, dha, dhb, tilingNpu;
        };

        auto makeTiling = [&](int64_t rows, GruBwdTilingData &td, int64_t &nGi, int64_t &nDh,
                              int64_t &nW, bool &wk) {
            td = {};
            td.B = B;
            td.S = S;
            td.I = I;
            td.H = H;
            td.aicNum = aicNum;
            td.aivNum = aivNum;
            td.chunkRows = rows;
            td.ghStepMajor = 1;
            const CoreTile tgi = PlanCoreTile(rows * S, aicNum);
            const CoreTile tdh = PlanCoreTile(rows, DhCores(aicNum));
            const CoreTile tw = PlanCoreTile(G, WCores(aicNum));
            const KTile tk = PlanKTile(rows * S, WKSplit(aicNum));
            const bool wKSplit = WKSplitMode();
            FillMatmulTilingEx(plat, static_cast<int32_t>(l1Size), static_cast<int32_t>(l0cSize),
                               static_cast<int32_t>(ubSize), tgi.tileM, G, I, false, true, true,
                               td.mm[selGI], "GI");
            FillMatmulTilingEx(plat, static_cast<int32_t>(l1Size), static_cast<int32_t>(l0cSize),
                               static_cast<int32_t>(ubSize), tgi.tileM, G, H, false, true, true,
                               td.mm[selGH], "GH");
            FillMatmulTilingEx(plat, static_cast<int32_t>(l1Size), static_cast<int32_t>(l0cSize),
                               static_cast<int32_t>(ubSize), tdh.tileM, H, DhK(G), false, false,
                               false, td.mm[selDH], "DH");
            const int64_t wtileM = wKSplit ? G : tw.tileM;
            const int64_t wtileK = wKSplit ? tk.tileK : rows * S;
            FillMatmulTilingEx(plat, static_cast<int32_t>(l1Size), static_cast<int32_t>(l0cSize),
                               static_cast<int32_t>(ubSize), wtileM, I, wtileK, true, false, false,
                               td.mm[selWI], "WI");
            FillMatmulTilingEx(plat, static_cast<int32_t>(l1Size), static_cast<int32_t>(l0cSize),
                               static_cast<int32_t>(ubSize), wtileM, H, wtileK, true, false, false,
                               td.mm[selWH], "WH");
            td.fullM[selGI] = rows * S;
            td.fullM[selGH] = rows * S;
            td.fullM[selDH] = rows;
            td.fullM[selWI] = G;
            td.fullM[selWH] = G;
            td.wKsplit = wKSplit ? tk.nBlk : 0;
            nGi = tgi.nBlk;
            nDh = tdh.nBlk;
            nW = wKSplit ? tk.nBlk : tw.nBlk;
            wk = wKSplit;
        };

        auto stageTiling = [&](const GruBwdTilingData &td) {
            at::Tensor tCpu = at::empty({static_cast<int64_t>(sizeof(GruBwdTilingData))},
                                        at::device(at::kCPU).dtype(at::kByte));
            std::memcpy(tCpu.data_ptr(), &td, sizeof(GruBwdTilingData));
            return tCpu.to(at::kPrivateUse1);
        };

        auto prepChunk = [&](PChunk &c, int64_t b0) {
            c.rows = std::min(pbc, B - b0);
            c.KS = c.rows * S;
            GruBwdTilingData td = {};
            makeTiling(c.rows, td, c.nBlkGi, c.nBlkDh, c.nBlkW, c.wk);
            c.tilingNpu = stageTiling(td);

            c.xf = xIn16.narrow(0, b0, c.rows);
            c.go = goIn.narrow(0, b0, c.rows);
            c.oc = outIn.narrow(0, b0, c.rows);
            // Step-major [S, rows, H]: h_{t-1} per step. When the forward stored
            // it (gate cache on) and the whole batch is one chunk, gs.hp *is*
            // this buffer -- step-major [S, B, H] with the batch equal to the
            // chunk, so the cell's (t*rows + row) index and the dW_hh GEMM's
            // contiguous [rows*S, H] view both line up and gru_bwd_hprev is
            // skipped. Otherwise the copy pass fills a private buffer.
            const bool hpStored = useStored && c.rows == B;
            c.hp = hpStored ? gs.hp : at::empty({S, c.rows, H}, x.options().dtype(at::kHalf));
            c.gi = useStored ? gs.gi.narrow(0, b0 * S, c.KS)
                             : at::empty({c.KS, G}, x.options().dtype(at::kHalf));
            // Step-major store: the cell indexes gh as [S, B, G] with the full
            // batch extent, so the whole tensor is handed over unchanged.
            c.gh = useStored ? gs.gh : at::empty({c.KS, G}, x.options().dtype(at::kHalf));
            // fp16: the gate gradients are the A operands of the DH / WI / WH
            // GEMMs (and of the colsum pass), so they narrow with the rest of
            // the GEMM operands.
            c.dgx = at::empty({c.KS, G}, x.options().dtype(at::kHalf));
            // Step-major: dgh.select(0, t) is the [rows, 3H] gradient for
            // timestep t, i.e. exactly the A operand the recurrent GEMM
            // (dhb = dgh @ w_hh) at step t-1 needs -- so the cell writes it once
            // and no separate per-step scratch is copied out.
            c.dgh = at::empty({S, c.rows, G}, x.options().dtype(at::kHalf));
            c.dha = at::zeros({c.rows, H}, x.options().dtype(at::kHalf));
            c.dhb = at::zeros({c.rows, H}, x.options().dtype(at::kHalf));

            const uint32_t blkGi = static_cast<uint32_t>(c.nBlkGi);
            if (!hpStored) {
                EXEC_KERNEL_CMD(gru_bwd_hprev, blkAiv, c.oc, c.hp, c.tilingNpu);
            }
            if (!useStored) {
                EXEC_KERNEL_CMD(gru_bwd_gemm, blkGi, c.xf, wih16, bih, c.gi, c.tilingNpu, selGI);
                EXEC_KERNEL_CMD(gru_bwd_gemm, blkGi, c.hp, whh16, bhh, c.gh, c.tilingNpu, selGH);
            }
        };

        auto finishChunk = [&](PChunk &c) {
            EXEC_KERNEL_CMD(gru_bwd_colsum_partial, blkAiv, c.dgx, c.dgh, ws, c.tilingNpu);
            EXEC_KERNEL_CMD(gru_bwd_colsum_final, blkAiv, ws, dbih, dbhh, c.tilingNpu);
            const int64_t cntWI = G * I;
            const int64_t cntWH = G * H;
            const uint32_t blkW = static_cast<uint32_t>(c.nBlkW);
            at::Tensor pWi = at::empty({c.wk ? c.nBlkW : 1, G, I}, x.options());
            at::Tensor pWh = at::empty({c.wk ? c.nBlkW : 1, G, H}, x.options());
            EXEC_KERNEL_CMD(gru_bwd_gemm, blkW, c.dgx, c.xf, dummyBias, pWi, c.tilingNpu, selWI);
            EXEC_KERNEL_CMD(gru_bwd_gemm, blkW, c.dgh, c.hp, dummyBias, pWh, c.tilingNpu, selWH);
            if (c.wk) {
                for (int64_t j = 0; j < c.nBlkW; ++j) {
                    at::Tensor sw = pWi.select(0, j);
                    at::Tensor sh = pWh.select(0, j);
                    EXEC_KERNEL_CMD(gru_bwd_add, blkAiv, dWih, sw, c.tilingNpu, cntWI);
                    EXEC_KERNEL_CMD(gru_bwd_add, blkAiv, dWhh, sh, c.tilingNpu, cntWH);
                }
            } else {
                EXEC_KERNEL_CMD(gru_bwd_add, blkAiv, dWih, pWi, c.tilingNpu, cntWI);
                EXEC_KERNEL_CMD(gru_bwd_add, blkAiv, dWhh, pWh, c.tilingNpu, cntWH);
            }
        };

        auto serialSweep = [&](PChunk &c) {
            const uint32_t blkDh = static_cast<uint32_t>(c.nBlkDh);
            for (int64_t t = S - 1; t >= 0; --t) {
                // dgh is step-major [S, rows, 3H]; select(0,t) is a contiguous
                // [rows, 3H] view. Hoisted to a named lvalue: EXEC_KERNEL_CMD's
                // ConvertTypes takes non-const lvalue refs, which cannot bind the
                // rvalue a select() returns.
                at::Tensor dghT = c.dgh.select(0, t);
                if (t < S - 1) {
                    at::Tensor dghPrev = c.dgh.select(0, t + 1);
                    EXEC_KERNEL_CMD(gru_bwd_gemm, blkDh, dghPrev, whh16, dummyBias,
                                    c.dhb, c.tilingNpu, selDH);
                }
                EXEC_KERNEL_CMD(gru_bwd_cell, blkAiv, c.gi, c.gh, c.hp, c.go, c.dha, c.dhb,
                                c.dgx, dghT, c.tilingNpu, t);
            }
        };

        // One fused launch: `cc` supplies the vector cell (op index iC of its
        // stream), `gc` supplies the cube dhb GEMM. Unused sides are flagged off.
        // gc's GEMM consumes the [rows, 3H] block its own cell wrote on the
        // previous fused launch, i.e. step tC.
        auto fuse = [&](PChunk &cc, PChunk &gc, int64_t iC, int64_t doC, int64_t doG) {
            const int64_t tC = S - 1 - iC / 2;
            at::Tensor cDgh = cc.dgh.select(0, tC);  // named lvalue: see serialSweep note
            at::Tensor gDgh = gc.dgh.select(0, tC);
            EXEC_KERNEL_CMD(gru_bwd_pipe, blkMix, cc.gi, cc.gh, cc.hp, cc.go, cc.dha, cc.dhb,
                            cc.dgx, cDgh, gDgh, whh16, dummyBias,
                            gc.dhb, cc.tilingNpu, gc.tilingNpu, selDH, tC, doC, doG);
        };

        for (int64_t c0 = 0; c0 < nchunks; c0 += 2) {
            PChunk a;
            PChunk b;
            prepChunk(a, c0 * pbc);
            const bool two = (c0 + 1 < nchunks);
            if (two) {
                prepChunk(b, (c0 + 1) * pbc);
                fuse(a, a, 0, 1, 0);  // stream A head: A.V(S-1) alone
                for (int64_t k = 0; k <= S - 2; ++k) {
                    fuse(b, a, 2 * k, 1, 1);      // B.V(S-1-k)  ||  A.G(S-2-k)
                    fuse(a, b, 2 * k + 2, 1, 1);  // A.V(S-2-k)  ||  B.G(S-2-k)
                }
                fuse(b, b, 2 * S - 2, 1, 0);  // stream B tail: B.V(0) alone
            } else {
                serialSweep(a);
            }
            finishChunk(a);
            if (two) {
                finishChunk(b);
            }
        }
        return std::make_tuple(dWih, dWhh, dbih, dbhh);
    }
    for (int64_t b0 = 0; !pipeMode && b0 < B; b0 += bc) {
        const int64_t rows = std::min(bc, B - b0);
        td.chunkRows = rows;

        const CoreTile tgi = PlanCoreTile(rows * S, aicNum);
        const CoreTile tdh = PlanCoreTile(rows, DhCores(aicNum));
        const CoreTile tw = PlanCoreTile(G, WCores(aicNum));
        const KTile tk = PlanKTile(rows * S, WKSplit(aicNum));
        const bool wKSplit = WKSplitMode();
        const int64_t KS = rows * S;

        FillMatmulTilingEx(plat, static_cast<int32_t>(l1Size), static_cast<int32_t>(l0cSize),
                           static_cast<int32_t>(ubSize), tgi.tileM, G, I, false, true, true,
                           td.mm[selGI], "GI");
        FillMatmulTilingEx(plat, static_cast<int32_t>(l1Size), static_cast<int32_t>(l0cSize),
                           static_cast<int32_t>(ubSize), tgi.tileM, G, H, false, true, true,
                           td.mm[selGH], "GH");
        FillMatmulTilingEx(plat, static_cast<int32_t>(l1Size), static_cast<int32_t>(l0cSize),
                           static_cast<int32_t>(ubSize), tdh.tileM, H, DhK(G),
                           false, false, false, td.mm[selDH], "DH");
        if (dhTransB) {
            FillMatmulTilingEx(plat, static_cast<int32_t>(l1Size), static_cast<int32_t>(l0cSize),
                               static_cast<int32_t>(ubSize), tdh.tileM, H, G, false, true, false,
                               td.mm[selDHT], "DHT");
        }
        const int64_t wtileM = wKSplit ? G : tw.tileM;
        const int64_t wtileK = wKSplit ? tk.tileK : KS;
        FillMatmulTilingEx(plat, static_cast<int32_t>(l1Size), static_cast<int32_t>(l0cSize),
                           static_cast<int32_t>(ubSize), wtileM, I, wtileK, true, false, false,
                           td.mm[selWI], "WI");
        FillMatmulTilingEx(plat, static_cast<int32_t>(l1Size), static_cast<int32_t>(l0cSize),
                           static_cast<int32_t>(ubSize), wtileM, H, wtileK, true, false, false,
                           td.mm[selWH], "WH");
        td.fullM[selGI] = KS;
        td.fullM[selGH] = KS;
        td.fullM[selDH] = rows;
        td.fullM[selDHT] = rows;
        td.fullM[selWI] = G;
        td.fullM[selWH] = G;
        td.wKsplit = wKSplit ? tk.nBlk : 0;

        at::Tensor tilingCpu = at::empty({static_cast<int64_t>(sizeof(GruBwdTilingData))},
                                         at::device(at::kCPU).dtype(at::kByte));
        std::memcpy(tilingCpu.data_ptr(), &td, sizeof(GruBwdTilingData));
        at::Tensor tilingNpu = tilingCpu.to(at::kPrivateUse1);

        at::Tensor xf = xIn16.narrow(0, b0, rows);
        at::Tensor go = goIn.narrow(0, b0, rows);
        at::Tensor oc = outIn.narrow(0, b0, rows);

        // ---- h_{t-1} for every timestep, then the two input projections -----
        // Step-major [S, rows, H]: the cell reads this step's [rows, H] slab
        // contiguously and the dW_hh GEMM needs hp's rows in the same order as
        // step-major dgh's (see the cell's hpBase). When the forward already
        // staged h_{t-1} into gs.hp (gate cache on) and the batch is a single
        // chunk, that tensor *is* this buffer -- contiguous [rows*S, H] for the
        // GEMM -- so the gru_bwd_hprev copy pass is skipped entirely.
        const bool hpStored = useStored && rows == B;
        at::Tensor hp = hpStored ? gs.hp : at::empty({S, rows, H}, x.options().dtype(at::kHalf));
        if (!hpStored) {
            EXEC_KERNEL_CMD(gru_bwd_hprev, blkAiv, oc, hp, tilingNpu);
        }

        at::Tensor gi = at::empty({KS, G}, x.options().dtype(at::kHalf));
        at::Tensor gh = at::empty({KS, G}, x.options().dtype(at::kHalf));
        const uint32_t blkGi = static_cast<uint32_t>(tgi.nBlk);
        if (useStored) {
            gi = gs.gi.narrow(0, b0 * S, KS);
            gh = gs.gh;  // step-major [S, B, G]: cell indexes by absolute row
        } else {
            EXEC_KERNEL_CMD(gru_bwd_gemm, blkGi, xf, wih16, bih, gi, tilingNpu, selGI);
            EXEC_KERNEL_CMD(gru_bwd_gemm, blkGi, hp, whh16, bhh, gh, tilingNpu, selGH);
        }

        // ---- reverse sweep over the timesteps -------------------------------
        at::Tensor dgx = at::empty({KS, G}, x.options().dtype(at::kHalf));
        // Step-major: dgh.select(0, t) is the [rows, 3H] gradient for timestep t
        // and doubles as the recurrent GEMM's A operand, so the cell writes it
        // once instead of also copying it into a per-step scratch buffer.
        at::Tensor dgh = at::empty({S, rows, G}, x.options().dtype(at::kHalf));
        at::Tensor dha = at::zeros({rows, H}, x.options().dtype(at::kHalf));
        at::Tensor dhb = at::zeros({rows, H}, x.options().dtype(at::kHalf));
        const uint32_t blkDh = static_cast<uint32_t>(tdh.nBlk);

        for (int64_t t = S - 1; t >= 0; --t) {
            // dgh is step-major [S, rows, 3H]; select(0,t) is a contiguous
            // [rows, 3H] view, named here because EXEC_KERNEL_CMD's ConvertTypes
            // takes non-const lvalue refs (an rvalue view cannot bind).
            at::Tensor dghT = dgh.select(0, t);
            if (t < S - 1) {
                at::Tensor dghPrev = dgh.select(0, t + 1);
                if (dhTransB) {
                    EXEC_KERNEL_CMD(gru_bwd_gemm, blkDh, dghPrev, whhT, dummyBias,
                                    dhb, tilingNpu, selDHT);
                } else {
                    EXEC_KERNEL_CMD(gru_bwd_gemm, blkDh, dghPrev, whh16, dummyBias,
                                    dhb, tilingNpu, selDH);
                }
            }
            EXEC_KERNEL_CMD(gru_bwd_cell, blkAiv, gi, gh, hp, go, dha, dhb, dgx, dghT,
                            tilingNpu, t);
        }

        // ---- bias gradients: two-pass column sum of dgx / dgh ---------------
        EXEC_KERNEL_CMD(gru_bwd_colsum_partial, blkAiv, dgx, dgh, ws, tilingNpu);
        EXEC_KERNEL_CMD(gru_bwd_colsum_final, blkAiv, ws, dbih, dbhh, tilingNpu);

        // ---- parameter gradients: dgx^T @ xf and dgh^T @ hp ------------------
        const int64_t cntWI = G * I;
        const int64_t cntWH = G * H;
        if (wKSplit) {
            // Each K-slab block emits a full [G, N] partial; the host sums them
            // into the running dW_* accumulator (which carries across chunks).
            const uint32_t blkW = static_cast<uint32_t>(tk.nBlk);
            at::Tensor dWihP = at::empty({tk.nBlk, G, I}, x.options());
            at::Tensor dWhhP = at::empty({tk.nBlk, G, H}, x.options());
            EXEC_KERNEL_CMD(gru_bwd_gemm, blkW, dgx, xf, dummyBias, dWihP, tilingNpu, selWI);
            EXEC_KERNEL_CMD(gru_bwd_gemm, blkW, dgh, hp, dummyBias, dWhhP, tilingNpu, selWH);
            for (int64_t j = 0; j < tk.nBlk; ++j) {
                at::Tensor pWi = dWihP.select(0, j);
                at::Tensor pWh = dWhhP.select(0, j);
                EXEC_KERNEL_CMD(gru_bwd_add, blkAiv, dWih, pWi, tilingNpu, cntWI);
                EXEC_KERNEL_CMD(gru_bwd_add, blkAiv, dWhh, pWh, tilingNpu, cntWH);
            }
        } else {
            at::Tensor dWihC = at::empty({G, I}, x.options());
            at::Tensor dWhhC = at::empty({G, H}, x.options());
            const uint32_t blkW = static_cast<uint32_t>(tw.nBlk);
            EXEC_KERNEL_CMD(gru_bwd_gemm, blkW, dgx, xf, dummyBias, dWihC, tilingNpu, selWI);
            EXEC_KERNEL_CMD(gru_bwd_gemm, blkW, dgh, hp, dummyBias, dWhhC, tilingNpu, selWH);
            EXEC_KERNEL_CMD(gru_bwd_add, blkAiv, dWih, dWihC, tilingNpu, cntWI);
            EXEC_KERNEL_CMD(gru_bwd_add, blkAiv, dWhh, dWhhC, tilingNpu, cntWH);
        }
    }

    return std::make_tuple(dWih, dWhh, dbih, dbhh);
}

}  // namespace ascend_kernel
