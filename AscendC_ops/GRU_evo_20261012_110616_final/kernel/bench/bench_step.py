#!/usr/bin/env python3
"""Full training step (forward + backward + AdamW) at B=8000 S=256 I=128 H=512 (A5).

Usage: GRU_SO=<path to gru_ext .so> python3 bench_step.py [warmup] [iters]
"""
import os, sys, time
import torch, torch_npu
torch.ops.load_library(os.environ["GRU_SO"])
torch.manual_seed(0)
B, S, I, H = 8000, 256, 128, 512
dev, g = "npu:0", torch.Generator().manual_seed(1234)
x = torch.randn(B, S, I, dtype=torch.float32, generator=g).to(dev)
p = [torch.nn.Parameter((torch.randn(3*H, I, generator=g) * 0.05).to(dev)),
     torch.nn.Parameter((torch.randn(3*H, H, generator=g) * 0.05).to(dev)),
     torch.nn.Parameter((torch.randn(3*H, generator=g) * 0.05).to(dev)),
     torch.nn.Parameter((torch.randn(3*H, generator=g) * 0.05).to(dev))]
opt = torch.optim.AdamW(p, lr=1e-3)
# Hoisted out of the timed loop: a fresh B*S*H fp32 randn on the CPU is ~1e9
# elements (multi-second) and would dominate the step measurement.
grad = torch.randn(B, S, H, dtype=torch.float32, generator=g).to(dev)
def step():
    out = torch.ops.npu.gru(x, p[0], p[1], p[2], p[3])
    gw = torch.ops.npu.gru_backward(grad, x, out, p[0], p[1], p[2], p[3])
    for q, gr in zip(p, gw):
        q.grad = gr
    opt.step(); opt.zero_grad()
warmup = int(sys.argv[1]) if len(sys.argv) > 1 else 3
iters = int(sys.argv[2]) if len(sys.argv) > 2 else 10
for _ in range(warmup):
    step()
torch.npu.synchronize()
t0 = time.perf_counter()
for _ in range(iters):
    step()
torch.npu.synchronize()
print(f"GRU_SO={os.environ['GRU_SO']}")
print(f"step B={B} S={S} I={I} H={H} fwd+bwd+AdamW  warmup={warmup} iters={iters}"
      f"  -> {(time.perf_counter()-t0)/iters*1e3:.3f} ms/step")
