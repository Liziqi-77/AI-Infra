#!/usr/bin/env python3
"""Phase A: gradient correctness of the GRU backward kernels (fp32).

Compares `dW_ih / dW_hh / db_ih / db_hh` produced by the registered device
operator `torch.ops.npu.gru_backward` against a CPU fp32 `nn.GRU` autograd
reference on the exact same weights, inputs and incoming gradient.

Also re-checks the forward numerics, and (unless GRAD_CHECK_AUTOGRAD=0) runs one
backward step through the `nn.Module` wrapper to confirm the autograd plumbing
delivers gradients to the GRU parameters.

Env knobs:
    GRU_SO           path to gru_ext*.so (default: kernel/build/gru_ext*.so)
    GRAD_CHECK_B     batch          (default 32)
    GRAD_CHECK_S     sequence       (default 64)
    GRAD_CHECK_H     hidden size    (default 512)
    GRAD_CHECK_I     input size     (default 128)
    GRAD_CHECK_SEED  rng seed       (default 0)
    GRAD_CHECK_DEV   npu device id  (default 3)
    GRAD_CHECK_AUTOGRAD  0 to skip the autograd.Function path (default 1)

Exit code 0 when every gradient is within tolerance.
"""

import glob
import os
import sys
from pathlib import Path

os.environ.setdefault("ASCEND_RT_VISIBLE_DEVICES", os.environ.get("GRAD_CHECK_DEV", "3"))

import torch  # noqa: E402
import torch.nn as nn  # noqa: E402
import torch_npu  # noqa: E402  (registers the npu device)

_HERE = Path(__file__).resolve().parent
_SO = os.environ.get("GRU_SO") or sorted(glob.glob(str(_HERE / "kernel" / "build" / "gru_ext*.so")))[-1]
torch.ops.load_library(_SO)

B = int(os.environ.get("GRAD_CHECK_B", 32))
S = int(os.environ.get("GRAD_CHECK_S", 64))
H = int(os.environ.get("GRAD_CHECK_H", 512))
I = int(os.environ.get("GRAD_CHECK_I", 128))
SEED = int(os.environ.get("GRAD_CHECK_SEED", 0))
DEVICE = "npu:0"

# Absolute/relative tolerances. The device path accumulates fp32 rounding over
# the reverse sweep; both sides are fp32 so the gap is numerical, not structural.
ATOL = 2e-5
RTOL = 2e-3


def stats(name, got, want):
    got = got.detach().float().cpu().flatten()
    want = want.detach().float().cpu().flatten()
    diff = (got - want).abs()
    denom = want.abs().clamp_min(1e-6)
    max_abs = diff.max().item()
    max_rel = (diff / denom).max().item()
    mean_abs = diff.mean().item()
    ref_abs = want.abs().mean().item()
    # Normalised measures: these do not blow up on the near-zero entries the way
    # an element-wise max_rel does, so they are the meaningful accuracy figures.
    rel_l2 = (diff.norm() / want.norm().clamp_min(1e-12)).item()
    cos = torch.nn.functional.cosine_similarity(got, want, dim=0).item()
    # frac of entries inside a combined abs+rel tolerance
    tol = ATOL + RTOL * want.abs()
    frac_ok = (diff <= tol).float().mean().item()
    ok = (max_abs <= ATOL + RTOL * want.abs().max().item()) and rel_l2 < 5e-3
    print(
        f"  {name:<10} max_abs={max_abs:.3e}  mean_abs={mean_abs:.3e}  "
        f"rel_l2={rel_l2:.3e}  cos={cos:.8f}  frac|abs<atol+rtol|={frac_ok:.6f}  "
        f"{'OK' if ok else 'FAIL'}"
    )
    return ok, max_abs, max_rel


def main():
    torch.manual_seed(SEED)
    print(f"config: B={B} S={S} H={H} I={I} seed={SEED} device={DEVICE} so={_SO}")

    ref = nn.GRU(I, H, batch_first=True).to(torch.float32)
    ref.eval()
    w_ih = ref.weight_ih_l0.detach()
    w_hh = ref.weight_hh_l0.detach()
    b_ih = ref.bias_ih_l0.detach()
    b_hh = ref.bias_hh_l0.detach()

    # Device copies. The custom ops dispatch on `x` being an npu tensor but every
    # tensor argument must live on the device, otherwise the kernel DMAs from a
    # host address and the AICore traps with an MTE error.
    w_ih_d = w_ih.npu()
    w_hh_d = w_hh.npu()
    b_ih_d = b_ih.npu()
    b_hh_d = b_hh.npu()

    x = torch.randn(B, S, I)
    grad_coef = torch.randn(B, S, H)  # the incoming dL/dout

    # ---- reference (CPU fp32 autograd) ------------------------------------
    out_ref, _ = ref(x)
    (out_ref * grad_coef).sum().backward()
    g_wih = ref.weight_ih_l0.grad.detach()
    g_whh = ref.weight_hh_l0.grad.detach()
    g_bih = ref.bias_ih_l0.grad.detach()
    g_bhh = ref.bias_hh_l0.grad.detach()

    # ---- device forward ---------------------------------------------------
    x_npu = x.npu()
    out_npu = torch.ops.npu.gru(x_npu, w_ih_d, w_hh_d, b_ih_d, b_hh_d)
    out_got = out_npu.cpu()

    print("\n[forward]")
    f_ok, f_abs, f_rel = stats("out", out_got, out_ref)

    # ---- device backward --------------------------------------------------
    gcoef_npu = grad_coef.npu()
    dW_ih, dW_hh, db_ih, db_hh = torch.ops.npu.gru_backward(
        gcoef_npu, x_npu, out_npu, w_ih_d, w_hh_d, b_ih_d, b_hh_d
    )

    print("\n[parameter gradients vs. CPU fp32 autograd]")
    ok_wih, a_wih, r_wih = stats("dW_ih", dW_ih, g_wih)
    ok_whh, a_whh, r_whh = stats("dW_hh", dW_hh, g_whh)
    ok_bih, a_bih, r_bih = stats("db_ih", db_ih, g_bih)
    ok_bhh, a_bhh, r_bhh = stats("db_hh", db_hh, g_bhh)
    grads_ok = all([ok_wih, ok_whh, ok_bih, ok_bhh])

    # ---- autograd.Function wrapper path -----------------------------------
    auto_ok = True
    if os.environ.get("GRAD_CHECK_AUTOGRAD", "1") != "0":
        from model_new_ascendc import GRURegressorAscendC  # noqa: E402

        torch.manual_seed(SEED)
        model = GRURegressorAscendC(I, H, 1).to(DEVICE)
        with torch.no_grad():
            model.gru.weight_ih_l0.copy_(w_ih)
            model.gru.weight_hh_l0.copy_(w_hh)
            model.gru.bias_ih_l0.copy_(b_ih)
            model.gru.bias_hh_l0.copy_(b_hh)
        pred = model(x_npu)
        loss = (pred * torch.randn_like(pred)).sum()
        loss.backward()
        print("\n[autograd wrapper]")
        for name, p in model.gru.named_parameters():
            g = p.grad
            has = g is not None and torch.isfinite(g).all().item() and g.abs().sum().item() > 0
            print(f"  gru.{name:<16} grad={'present' if has else 'MISSING/ZERO'}"
                  f"  abs_sum={0.0 if g is None else g.abs().sum().item():.3e}")
            auto_ok = auto_ok and has
        print(f"  fc.weight grad present: {model.fc.weight.grad is not None}")

        # ---- strict: autograd-path grads vs. the SAME CPU reference grads ----
        # Drive `_GruForwardBackward` through autograd on the very same
        # functional that produced g_wih/... above ((out * grad_coef).sum()), so
        # the two sides are directly comparable. This checks the autograd
        # plumbing numerically, not merely that `grad` is non-None.
        from model_new_ascendc import _GruForwardBackward  # noqa: E402

        wy = w_ih.npu().requires_grad_(True)
        wv = w_hh.npu().requires_grad_(True)
        by = b_ih.npu().requires_grad_(True)
        bv = b_hh.npu().requires_grad_(True)
        out_ag = _GruForwardBackward.apply(x_npu, wy, wv, by, bv)
        (out_ag * gcoef_npu).sum().backward()
        print("\n[autograd.Function grads vs. the same CPU fp32 reference]")
        print(f"  grad_fn of out: {'present' if out_ag.grad_fn is not None else 'ABSENT'}")
        auto_ok = auto_ok and (out_ag.grad_fn is not None)
        for nm, gp in (("w_ih", wy), ("w_hh", wv), ("b_ih", by), ("b_hh", bv)):
            if gp.grad is None:
                print(f"  d{nm:<6} grad=ABSENT  FAIL")
                auto_ok = False
                continue
            ok_n, _, _ = stats(f"d{nm}", gp.grad, {"w_ih": g_wih, "w_hh": g_whh,
                                                  "b_ih": g_bih, "b_hh": g_bhh}[nm])
            auto_ok = auto_ok and ok_n

    print("\n=== summary ===")
    print(f"forward      : {'PASS' if f_ok else 'FAIL'}  (max_abs={f_abs:.3e}, max_rel={f_rel:.3e})")
    print(f"param grads  : {'PASS' if grads_ok else 'FAIL'}")
    print(f"  dW_ih max_abs={a_wih:.3e} max_rel={r_wih:.3e}")
    print(f"  dW_hh max_abs={a_whh:.3e} max_rel={r_whh:.3e}")
    print(f"  db_ih max_abs={a_bih:.3e} max_rel={r_bih:.3e}")
    print(f"  db_hh max_abs={a_bhh:.3e} max_rel={r_bhh:.3e}")
    print(f"autograd path: {'PASS' if auto_ok else 'FAIL'}")

    return 0 if (f_ok and grads_ok and auto_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
