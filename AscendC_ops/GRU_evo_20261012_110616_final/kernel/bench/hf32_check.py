#!/usr/bin/env python3
"""HF32 acceptance check (a)+(c): software-HF32 reference agreement + TF32-class deviation.

There is no raw-GEMM entry point in this operator (underlying matmuls are internal
to torch.ops.npu.gru / gru_backward), so the "is the flag actually on?" check is
done at the operator level: the whole unrolled GRU recurrence is re-evaluated in
torch with every GEMM replaced by  matmul_fp64(round_hf32(A), round_hf32(B))
(fp32 accumulate semantics).  Agreement of the DEVICE kernel with that software
model is the (a) column; the model's own deviation from the exact fp64 recurrence
is the (c) TF32-class reference deviation.

Columns reported per case:
  |ours - truth|      : device kernel vs exact fp64 recurrence
  |hf32model - truth| : TF32-class reference vs exact fp64 recurrence   <- column (c)
  |ours - hf32model|  : device kernel vs software HF32 model            <- column (a)

round_hf32 is copied verbatim from probe_hf32/run_probe2.py (RNE truncation of the
low 13 fp32 mantissa bits).

Usage: GRU_SO=<so> GOLDEN_DIR=<dir> python3 hf32_check.py [case indices...]
"""
import os, sys, time
os.environ.setdefault("ASCEND_RT_VISIBLE_DEVICES", os.environ.get("GRU_DEV", "0"))
import torch, torch_npu

SO = os.environ["GRU_SO"]
GOLDEN_DIR = os.environ["GOLDEN_DIR"]
torch.ops.load_library(SO)
sys.path.insert(0, GOLDEN_DIR)
import model as golden


def round_hf32(x):
    """RNE fp32 -> hf32 (10 explicit mantissa bits, fp32 exponent)."""
    i = x.contiguous().view(torch.int32)
    lsb = (i >> 13) & 1
    bias = 0x0FFF + lsb
    r = (i + bias) & ~0x1FFF
    return r.view(torch.float32)


def mm_hf32(a, b):
    """HF32 GEMM model: round both operands, exact fp64 matmul, fp32 result."""
    return (round_hf32(a).double() @ round_hf32(b).double()).float()


def unroll(x, w_ih, w_hh, b_ih, b_hh, hf32):
    """Deterministic fp32 GRU recurrence, matching golden.Model._reference_formula.

    hf32=False -> exact fp32 matmul (the "truth" / frozen fp32 golden semantics)
    hf32=True  -> every GEMM routed through mm_hf32 (the HF32 software model)
    """
    H = w_hh.shape[0] // 3
    xf = x.detach().to(torch.float32).cpu()
    wi = w_ih.detach().to(torch.float32).cpu()
    wh = w_hh.detach().to(torch.float32).cpu()
    bi = b_ih.detach().to(torch.float32).cpu()
    bh = b_hh.detach().to(torch.float32).cpu()
    B, S, _ = xf.shape
    wt_i = wi.t().contiguous()
    wt_h = wh.t().contiguous()
    h = torch.zeros(B, H, dtype=torch.float32)
    out = torch.empty(B, S, H, dtype=torch.float32)
    mm = mm_hf32 if hf32 else (lambda a, b: (a.double() @ b.double()).float())
    for t in range(S):
        xt = xf[:, t].contiguous()
        gx = mm(xt, wt_i) + bi
        gh = mm(h, wt_h) + bh
        r = torch.sigmoid(gx[:, :H] + gh[:, :H])
        z = torch.sigmoid(gx[:, H:2 * H] + gh[:, H:2 * H])
        n = torch.tanh(gx[:, 2 * H:] + r * gh[:, 2 * H:])
        h = (1.0 - z) * n + z * h
        out[:, t] = h
    return out


def main():
    want = {int(v) for v in sys.argv[1:] if v.strip()}
    print(f"so     : {SO}")
    print(f"golden : {GOLDEN_DIR}")
    torch.manual_seed(0)
    ref = golden.Model(*golden.get_init_inputs())
    w_ih = ref.gru.weight_ih_l0.detach()
    w_hh = ref.gru.weight_hh_l0.detach()
    b_ih = ref.gru.bias_ih_l0.detach()
    b_hh = ref.gru.bias_hh_l0.detach()
    w_ih_d, w_hh_d = w_ih.npu(), w_hh.npu()
    b_ih_d, b_hh_d = b_ih.npu(), b_hh.npu()
    groups = golden.get_input_groups()

    print(f"\n{'idx':>3} {'shape':<18} {'dtype':<8} "
          f"{'|ours-truth|':>12} {'|hf32mod-truth|':>15} {'|ours-hf32mod|':>14}  verdict")
    for idx, group in enumerate(groups):
        if want and idx not in want:
            continue
        x = group[0]
        dt = str(x.dtype).replace("torch.", "")
        # only manageable B for the fp64 CPU unroll
        if x.shape[0] > 128:
            print(f"{idx:>3} {str(tuple(x.shape)):<18} {dt:<8}  skipped (B too large for CPU fp64 unroll)")
            continue
        t0 = time.time()
        with torch.no_grad():
            ours = torch.ops.npu.gru(x.npu(), w_ih_d, w_hh_d, b_ih_d, b_hh_d).cpu().float()
        truth = unroll(x, w_ih, w_hh, b_ih, b_hh, hf32=False)
        model = unroll(x, w_ih, w_hh, b_ih, b_hh, hf32=True)
        d_truth = (ours - truth).abs().max().item()
        d_modtr = (model - truth).abs().max().item()
        d_oursm = (ours - model).abs().max().item()
        ok = d_oursm <= 3 * max(d_modtr, 1e-6)
        print(f"{idx:>3} {str(tuple(x.shape)):<18} {dt:<8} "
              f"{d_truth:>12.3e} {d_modtr:>15.3e} {d_oursm:>14.3e}  "
              f"{'HF32-LIVE' if ok else 'MISMATCH'}  ({time.time()-t0:.1f}s)")
        sys.stdout.flush()
    print("\n(a) |ours - hf32model| small  => SetHF32 is genuinely active and matches the"
          " documented HF32 semantics")
    print("(c) |hf32model - truth|       => what a TF32-class reference deviates by on"
          " these cases (the accuracy bar this route is judged against)")


if __name__ == "__main__":
    main()
