#!/usr/bin/env python3
"""Reference (PyTorch) implementation of the fused single-layer GRU operator.

Operator contract (identical to the hand-written AscendC extension in kernel/):
    gru(x, w_ih, w_hh, b_ih, b_hh) -> out
        x    : [B, S, I]  (batch_first, dtype = fp16 / bf16 / fp32)
        w_ih : [3H, I]    gate order (r, z, n) along dim 0
        w_hh : [3H, H]    gate order (r, z, n) along dim 0
        b_ih : [3H], b_hh : [3H]
        out  : [B, S, H] in the dtype of x

    gru_backward(grad_out, x, out, w_ih, w_hh, b_ih, b_hh)
        -> (dW_ih [3H,I], dW_hh [3H,H], db_ih [3H], db_hh [3H])   (fp32 only)

This module exposes:
    Model                : nn.Module wrapping the recurrence; forward() returns
                           (out, dW_ih, dW_hh, db_ih, db_hh) so the same call
                           signature covers the fused forward + backward step.
    get_input_groups()   : deterministic per-case argument tuples (shared by the
                           reference and the AscendC implementation so both are
                           timed / compared on byte-identical inputs).
"""

import torch
import torch.nn as nn

# ---------------------------------------------------------------------------
# Deterministic test cases: (B, S, I, H, dtype_name)
#   case 0 is the production shape used throughout the baseline reports
#   (B=8000, S=256, I=128, H=512, fp32).
# ---------------------------------------------------------------------------
CASES = [
    (8000, 256, 128, 512, "fp32"),
    (1024, 128, 128, 512, "fp32"),
    (256, 64, 256, 256, "fp32"),
    (64, 32, 128, 128, "fp16"),
]

_DTYPES = {
    "fp32": torch.float32,
    "fp16": torch.float16,
    "bf16": torch.bfloat16,
}


def case_dtype(name):
    return _DTYPES[name]


def make_inputs(B, S, I, H, dtype_name="fp32", seed=1234):
    """Deterministic inputs for one case (all six tensors)."""
    g = torch.Generator().manual_seed(seed)
    dt = case_dtype(dtype_name)
    x = torch.randn(B, S, I, generator=g, dtype=torch.float32)
    w_ih = torch.randn(3 * H, I, generator=g, dtype=torch.float32) * 0.05
    w_hh = torch.randn(3 * H, H, generator=g, dtype=torch.float32) * 0.05
    b_ih = torch.randn(3 * H, generator=g, dtype=torch.float32) * 0.05
    b_hh = torch.randn(3 * H, generator=g, dtype=torch.float32) * 0.05
    grad_out = torch.randn(B, S, H, generator=g, dtype=torch.float32)
    to = lambda t: t.to(dt)
    return to(x), to(w_ih), to(w_hh), to(b_ih), to(b_hh), to(grad_out)


def get_input_groups():
    """Per-case positional argument tuples: (x, w_ih, w_hh, b_ih, b_hh, grad_out)."""
    groups = []
    for (B, S, I, H, name) in CASES:
        groups.append(list(make_inputs(B, S, I, H, name)))
    return groups


def get_init_inputs():
    return [[] for _ in CASES]


def gru_recurrence(x, w_ih, w_hh, b_ih, b_hh):
    """Straightforward fp32 unroll of the recurrence (differentiable)."""
    B, S, I = x.shape
    H = w_hh.shape[1]
    ox = x.to(torch.float32)

    Wr, Wz, Wn = w_ih[0:H], w_ih[H:2 * H], w_ih[2 * H:3 * H]
    Ur, Uz, Un = w_hh[0:H], w_hh[H:2 * H], w_hh[2 * H:3 * H]
    br, bz, bn = b_ih[0:H], b_ih[H:2 * H], b_ih[2 * H:3 * H]
    hr, hz, hn = b_hh[0:H], b_hh[H:2 * H], b_hh[2 * H:3 * H]

    h = torch.zeros(B, H, dtype=torch.float32, device=x.device)
    outs = []
    for t in range(S):
        xt = ox[:, t]
        r = torch.sigmoid(xt @ Wr.t() + br + h @ Ur.t() + hr)
        z = torch.sigmoid(xt @ Wz.t() + bz + h @ Uz.t() + hz)
        n = torch.tanh(xt @ Wn.t() + bn + r * (h @ Un.t() + hn))
        h = (1.0 - z) * n + z * h
        outs.append(h)
    return torch.stack(outs, dim=1)


class Model(nn.Module):
    """Reference fused forward + backward (fp32 reference maths)."""

    def forward(self, x, w_ih, w_hh, b_ih, b_hh, grad_out):
        params = [
            w_ih.to(torch.float32).detach().requires_grad_(True),
            w_hh.to(torch.float32).detach().requires_grad_(True),
            b_ih.to(torch.float32).detach().requires_grad_(True),
            b_hh.to(torch.float32).detach().requires_grad_(True),
        ]
        with torch.enable_grad():
            out = gru_recurrence(x.detach(), *params)
            grads = torch.autograd.grad(
                outputs=out,
                inputs=params,
                grad_outputs=grad_out.to(torch.float32),
            )
        out_d = out.detach()
        if x.dtype != torch.float32:
            out_d = out_d.to(x.dtype)
        return (out_d,) + tuple(g.detach() for g in grads)
