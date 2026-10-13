#!/usr/bin/env python3
"""Backward timing for torch.ops.npu.gru_backward at B=8000 S=256 I=128 H=512 fp32 (A5).

Usage: GRU_SO=<path to gru_ext .so> python3 bench_bwd.py [warmup] [iters]
Backward latency is measured after a forward pass, with the forward excluded
from the timed loop (matches the 378.6 ms baseline in the brief).
"""
import os, sys, time
import torch
import torch_npu

torch.ops.load_library(os.environ["GRU_SO"])
torch.manual_seed(0)
B, S, I, H = 8000, 256, 128, 512
dev = "npu:0"
g = torch.Generator().manual_seed(1234)
x = torch.randn(B, S, I, dtype=torch.float32, generator=g).to(dev)
w_ih = (torch.randn(3 * H, I, generator=g) * 0.05).to(dev)
w_hh = (torch.randn(3 * H, H, generator=g) * 0.05).to(dev)
b_ih = (torch.randn(3 * H, generator=g) * 0.05).to(dev)
b_hh = (torch.randn(3 * H, generator=g) * 0.05).to(dev)
out = torch.ops.npu.gru(x, w_ih, w_hh, b_ih, b_hh)
grad = torch.randn(B, S, H, dtype=torch.float32, generator=g).to(dev)

warmup = int(sys.argv[1]) if len(sys.argv) > 1 else 3
iters = int(sys.argv[2]) if len(sys.argv) > 2 else 10
for _ in range(warmup):
    gw = torch.ops.npu.gru_backward(grad, x, out, w_ih, w_hh, b_ih, b_hh)
torch.npu.synchronize()

t0 = time.perf_counter()
for _ in range(iters):
    gw = torch.ops.npu.gru_backward(grad, x, out, w_ih, w_hh, b_ih, b_hh)
torch.npu.synchronize()
dt = (time.perf_counter() - t0) / iters
print(f"GRU_SO={os.environ['GRU_SO']}")
print(f"bwd B={B} S={S} I={I} H={H} fp32  warmup={warmup} iters={iters}"
      f"  -> {dt * 1e3:.3f} ms/iter   grads={len(gw)}")
