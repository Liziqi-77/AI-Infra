#!/usr/bin/env python3
"""Tiny msprof target: a few forward passes at the production shape (grus only)."""
import os, sys, time
os.environ.setdefault("ASCEND_RT_VISIBLE_DEVICES", "0")
import torch
import torch_npu

torch.ops.load_library(os.environ["GRU_SO"])
B, S, I, H = 8000, 256, 128, 512
ITERS = int(os.environ.get("PROF_ITERS", "3"))
torch.manual_seed(0)
x = torch.randn(B, S, I).npu()
w_ih = torch.randn(3 * H, I).npu()
w_hh = torch.randn(3 * H, H).npu()
b_ih = torch.randn(3 * H).npu()
b_hh = torch.randn(3 * H).npu()
for _ in range(ITERS):
    out = torch.ops.npu.gru(x, w_ih, w_hh, b_ih, b_hh)
torch.npu.synchronize()
print("forward done", tuple(out.shape), flush=True)
time.sleep(0.2)
